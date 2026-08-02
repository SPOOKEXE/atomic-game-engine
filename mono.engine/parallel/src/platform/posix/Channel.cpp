// The channel that crosses a process boundary.
//
// A connected socket pair rather than two pipes: one handle per end instead of
// four, bidirectional without any wiring, and `MSG_NOSIGNAL` so a write to a
// dead peer returns an error rather than killing this process. A driver that
// died of `SIGPIPE` because a host crashed would turn one host's fault into the
// whole server's, which is the opposite of what processes are here for.
//
// **A stream carrying frames.** `SOCK_STREAM` is used rather than
// `SOCK_SEQPACKET` because a frame may be megabytes and a datagram socket
// refuses anything past its buffer. So the framing is this file's: a four-byte
// length, then the bytes. `SOCK_SEQPACKET` would move the partial-write problem
// into the kernel rather than solve it.
//
// **Never blocks, so it buffers.** The handles are non-blocking, which means a
// write can be partial and a read can stop mid-frame. Neither is allowed to
// reach the caller: `Send` takes the whole frame or none of it, `Receive` hands
// over a whole frame or nothing. Both keep a buffer, and every call pumps the
// socket first. There is no `Flush` on the interface deliberately — a caller
// that had to remember one would be a caller that can tell which transport it
// holds.
//
// **The bound is on this side's buffers.** When the socket will not take any
// more, the outbound buffer grows to `Capacity` and then `Send` refuses. The
// unread bytes sit in the kernel and in the peer's socket, which is
// backpressure arriving the way it should: the producer is told, rather than
// the machine filling up.

#include <engine/core/Log.hpp>
#include <engine/parallel/Channel.hpp>

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <mutex>
#include <sys/socket.h>
#include <unistd.h>

namespace engine::parallel {

	namespace {
		// Where a child started with an endpoint finds it.
		//
		// Three: after standard input, output and error, which the child
		// inherits so that a host's log lands where its supervisor's does.
		constexpr int INHERITED = 3;

		// The frame header. A length and nothing else — there is no version,
		// because both ends of a channel are the same binary by construction:
		// a host is `mono.server` started in host mode.
		constexpr size_t HEADER = sizeof(uint32_t);

		// How much is read from the socket in one call.
		constexpr size_t READ_CHUNK = 64u * 1024u;

		bool MakeNonBlocking(int handle) {
			const int flags = ::fcntl(handle, F_GETFL, 0);
			if (flags < 0) {
				return false;
			}
			return ::fcntl(handle, F_SETFL, flags | O_NONBLOCK) == 0;
		}

		// One end of a socket pair, with the buffers that make it look like a
		// queue of whole frames.
		class SocketChannel final : public Channel {
		  public:
			SocketChannel(int handle, const ChannelSettings &settings)
				: Handle(handle), Settings_(settings) {}

			~SocketChannel() override {
				Close();
			}

			ChannelStatus Send(std::span<const std::byte> frame) override {
				if (frame.size() > Settings_.MaximumFrame) {
					// Refused whole rather than truncated, and checked before
					// the closed test so that an oversized frame reports the
					// reason it will still be oversized next time.
					return ChannelStatus::TooLarge;
				}

				std::lock_guard lock(Guard);
				if (!Usable()) {
					return ChannelStatus::Closed;
				}

				// First, because the room this frame needs may be room the
				// socket has already taken.
				Pump();
				if (!Usable()) {
					return ChannelStatus::Closed;
				}

				const size_t needed = HEADER + frame.size();
				if (Outbound.size() - OutboundRead + needed > Settings_.Capacity) {
					return ChannelStatus::Full;
				}

				const auto length = static_cast<uint32_t>(frame.size());
				std::byte header[HEADER];
				for (size_t index = 0; index < HEADER; index++) {
					// Little-endian by shift, the same as `core::ByteWriter`:
					// written the same way on every machine rather than
					// however this one happens to store an integer.
					header[index] = static_cast<std::byte>((length >> (8u * index)) & 0xFFu);
				}

				Outbound.insert(Outbound.end(), header, header + HEADER);
				Outbound.insert(Outbound.end(), frame.begin(), frame.end());

				PumpOut();
				return Usable() ? ChannelStatus::Ok : ChannelStatus::Closed;
			}

			ChannelStatus Receive(std::vector<std::byte> &frame) override {
				std::lock_guard lock(Guard);
				Pump();

				if (!Complete()) {
					// Closed *and* drained, in that order. A peer that exits
					// cleanly should not strip this end of what it already
					// said.
					return Usable() ? ChannelStatus::Empty : ChannelStatus::Closed;
				}

				const size_t length = LengthAt(InboundRead);
				const size_t start = InboundRead + HEADER;

				// Assigned rather than moved into, so the caller's capacity
				// survives and a channel polled every tick stops allocating.
				frame.assign(
					Inbound.begin() + static_cast<ptrdiff_t>(start),
					Inbound.begin() + static_cast<ptrdiff_t>(start + length)
				);

				InboundRead = start + length;
				Compact(Inbound, InboundRead);
				return ChannelStatus::Ok;
			}

			size_t Pending() const override {
				std::lock_guard lock(Guard);
				Pump();

				size_t frames = 0;
				size_t cursor = InboundRead;
				while (cursor + HEADER <= Inbound.size()) {
					const size_t length = LengthAt(cursor);
					if (cursor + HEADER + length > Inbound.size()) {
						break;
					}
					cursor += HEADER + length;
					frames++;
				}
				return frames;
			}

			size_t PendingBytes() const override {
				std::lock_guard lock(Guard);
				Pump();

				size_t bytes = 0;
				size_t cursor = InboundRead;
				while (cursor + HEADER <= Inbound.size()) {
					const size_t length = LengthAt(cursor);
					if (cursor + HEADER + length > Inbound.size()) {
						break;
					}
					cursor += HEADER + length;
					bytes += length;
				}
				return bytes;
			}

			bool Open() const override {
				std::lock_guard lock(Guard);
				Pump();
				return Usable();
			}

			void Close() override {
				std::lock_guard lock(Guard);
				if (Handle < 0) {
					return;
				}

				// One last push, so a host that queued a final frame and exited
				// does not take it with it.
				PumpOut();

				::close(Handle);
				Handle = -1;
			}

		  private:
			// Moves whatever the socket will take, in both directions.
			//
			// **Both, on every entry point, including the read-only ones.** A
			// frame larger than the socket buffer cannot leave in one write, so
			// what is left sits in `Outbound` until something pushes it — and
			// the caller that is going to push it is usually the one waiting
			// for the answer, calling nothing but `Receive`. Pumping only
			// inbound there deadlocks the pair at the first frame too big to
			// fit, which is a bug that hides completely behind small messages.
			//
			// This is also why the interface has no `Flush`: a caller that had
			// to remember one could tell which transport it held.
			void Pump() const {
				PumpOut();
				PumpIn();
			}

			// Whether this end can still carry anything. Held-lock only.
			bool Usable() const {
				return Handle >= 0 && PeerAlive;
			}

			// Whether a whole frame is buffered.
			bool Complete() const {
				if (InboundRead + HEADER > Inbound.size()) {
					return false;
				}
				return InboundRead + HEADER + LengthAt(InboundRead) <= Inbound.size();
			}

			size_t LengthAt(size_t offset) const {
				uint32_t length = 0;
				for (size_t index = 0; index < HEADER; index++) {
					length |= static_cast<uint32_t>(Inbound[offset + index]) << (8u * index);
				}
				return length;
			}

			// Drops the consumed prefix once it is worth the move.
			//
			// Not on every read: a channel carrying small frames would then
			// memmove its whole buffer per frame. Half is the usual compromise
			// and it keeps the amortised cost linear.
			static void Compact(std::vector<std::byte> &buffer, size_t &read) {
				if (read == 0) {
					return;
				}
				if (read == buffer.size()) {
					buffer.clear();
					read = 0;
					return;
				}
				if (read * 2 < buffer.size()) {
					return;
				}
				buffer.erase(buffer.begin(), buffer.begin() + static_cast<ptrdiff_t>(read));
				read = 0;
			}

			// Writes as much of the outbound buffer as the socket will take.
			//
			// Const because `Pending` is, and what it changes is a buffer
			// rather than anything a caller can observe as state.
			void PumpOut() const {
				while (Handle >= 0 && OutboundRead < Outbound.size()) {
					const size_t remaining = Outbound.size() - OutboundRead;
					const ssize_t written = ::send(
						Handle, Outbound.data() + OutboundRead, remaining, MSG_NOSIGNAL | MSG_DONTWAIT
					);

					if (written > 0) {
						OutboundRead += static_cast<size_t>(written);
						continue;
					}
					if (written < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
						// The socket is full. Not an error: the rest stays
						// buffered and goes out on a later call.
						break;
					}

					// EPIPE or ECONNRESET. The peer is gone, so nothing queued
					// will ever arrive and holding it would be a leak with a
					// hopeful name.
					PeerAlive = false;
					break;
				}
				Compact(Outbound, OutboundRead);
			}

			// Reads whatever the socket has, up to the buffer's cap.
			void PumpIn() const {
				while (Handle >= 0 && PeerAlive) {
					if (Inbound.size() - InboundRead >= Settings_.Capacity) {
						// Left in the kernel deliberately. That is what makes
						// the peer's `Send` start refusing, which is
						// backpressure reaching the producer rather than the
						// machine filling up quietly.
						break;
					}

					const size_t before = Inbound.size();
					Inbound.resize(before + READ_CHUNK);

					const ssize_t got = ::recv(Handle, Inbound.data() + before, READ_CHUNK, MSG_DONTWAIT);

					if (got > 0) {
						Inbound.resize(before + static_cast<size_t>(got));
						continue;
					}

					Inbound.resize(before);
					if (got == 0) {
						// Orderly shutdown by the peer.
						PeerAlive = false;
						break;
					}
					if (errno == EINTR) {
						continue;
					}
					if (errno == EAGAIN || errno == EWOULDBLOCK) {
						break;
					}

					PeerAlive = false;
					break;
				}
			}

			// `mutable` throughout, because pumping the socket is what every
			// call does first and two of them are `const`. Nothing here is
			// observable state: a frame is where it is, and whether it has
			// reached the kernel yet is not a caller's question.
			mutable std::mutex Guard;
			mutable int Handle = -1;
			ChannelSettings Settings_;

			mutable std::vector<std::byte> Outbound;
			mutable size_t OutboundRead = 0;

			mutable std::vector<std::byte> Inbound;
			mutable size_t InboundRead = 0;

			// Separate from the handle: this end may still be open and holding
			// frames the peer sent before it went away.
			mutable bool PeerAlive = true;
		};
	}

	// --- ChannelEnd --------------------------------------------------------

	ChannelEnd::~ChannelEnd() {
		Close();
	}

	ChannelEnd::ChannelEnd(ChannelEnd &&other) noexcept : Value(other.Value) {
		other.Value = -1;
	}

	ChannelEnd &ChannelEnd::operator=(ChannelEnd &&other) noexcept {
		if (this == &other) {
			return *this;
		}
		Close();
		Value = other.Value;
		other.Value = -1;
		return *this;
	}

	void ChannelEnd::Close() {
		if (Value >= 0) {
			::close(static_cast<int>(Value));
			Value = -1;
		}
	}

	void ChannelEnd::Adopt(int64_t handle) {
		Close();
		Value = handle;
	}

	// --- factories ---------------------------------------------------------

	ProcessChannel MakeProcessChannel(const ChannelSettings &settings) {
		ProcessChannel pair;

		int handles[2] = {-1, -1};
		if (::socketpair(AF_UNIX, SOCK_STREAM, 0, handles) != 0) {
			ENGINE_ERROR("could not create a process channel: {}", std::strerror(errno));
			return pair;
		}

		// Close-on-exec on both. Without it a spawned child inherits *every*
		// handle this process holds, including the driver's own end of this
		// very channel — and while the child holds a copy of that end, the
		// kernel counts the socket as still having a peer. Neither side would
		// ever see the other go away, which is the one thing the channel is
		// relied on to notice.
		//
		// The end the child is meant to have is placed by `dup2` at spawn
		// time, and `dup2` clears close-on-exec on what it creates.
		::fcntl(handles[0], F_SETFD, FD_CLOEXEC);
		::fcntl(handles[1], F_SETFD, FD_CLOEXEC);

		// Only this end. The remote one is made non-blocking by whoever adopts
		// it, because `O_NONBLOCK` is a property of the open file description
		// and setting it here would set it for the child too — which is
		// correct, but relying on that to cross an exec is the kind of thing
		// that works until somebody re-opens the handle.
		if (!MakeNonBlocking(handles[0])) {
			ENGINE_ERROR("could not configure a process channel: {}", std::strerror(errno));
			::close(handles[0]);
			::close(handles[1]);
			return pair;
		}

		pair.Local = std::make_unique<SocketChannel>(handles[0], settings);
		pair.Remote.Adopt(handles[1]);
		return pair;
	}

	bool HasInheritedChannel() {
		// A socket, specifically. The slot is an ordinary handle number and a
		// program run from a shell that happened to leave one open there would
		// otherwise be told it is a supervised host — and would then try to
		// speak a protocol down somebody's log file.
		int kind = 0;
		socklen_t size = sizeof(kind);
		if (::getsockopt(INHERITED, SOL_SOCKET, SO_TYPE, &kind, &size) != 0) {
			return false;
		}
		return kind == SOCK_STREAM;
	}

	std::unique_ptr<Channel> AdoptInheritedChannel(const ChannelSettings &settings) {
		if (!HasInheritedChannel()) {
			return nullptr;
		}
		if (!MakeNonBlocking(INHERITED)) {
			ENGINE_ERROR("could not configure the inherited channel: {}", std::strerror(errno));
			return nullptr;
		}
		return std::make_unique<SocketChannel>(INHERITED, settings);
	}
}
