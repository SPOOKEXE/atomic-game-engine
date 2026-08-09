#pragma once

// One end of a socket pair, presented as a queue of whole frames.
//
// **A stream carrying frames.** A stream socket is used rather than a datagram
// one because a frame may be megabytes and a datagram socket refuses anything
// past its buffer. So the framing is this file's: a four-byte length, then the
// bytes. A sequenced-packet socket would move the partial-write problem into
// the kernel rather than solve it, and Windows has no such socket at all.
//
// **Never blocks, so it buffers.** The handle is non-blocking, which means a
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
//
// **Nothing here can kill the process.** A send to a dead peer fails rather
// than raising anything — see `platform::SocketSend`. A driver that died
// because a host crashed would turn one host's fault into the whole server's,
// which is the opposite of what processes are here for.
//
// Private to the module. It carries no operating system of its own: the handle
// is a number and the four calls that touch it are `platform/Socket.hpp`. That
// is what lets this file — the framing, which is the part worth getting right
// once — be shared rather than copied per platform.

#include <engine/parallel/Channel.hpp>

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <span>
#include <vector>

namespace engine::parallel {

	// A Channel over one handle of a connected socket pair.
	class SocketChannel final : public Channel {
	  public:
		// @param handle   The socket end, which this takes ownership of and
		//                 which must already be non-blocking. Negative for
		//                 none, which is the same sentinel `ChannelEnd` uses.
		// @param settings How the frames and the buffers are sized.
		SocketChannel(int64_t handle, const ChannelSettings &settings);

		~SocketChannel() override;

		ChannelStatus Send(std::span<const std::byte> frame) override;

		ChannelStatus Receive(std::vector<std::byte> &frame) override;

		size_t Pending() const override;

		size_t PendingBytes() const override;

		bool Open() const override;

		void Close() override;

	  private:
		// Moves whatever the socket will take, in both directions.
		//
		// **Both, on every entry point, including the read-only ones.** A frame
		// larger than the socket buffer cannot leave in one write, so what is
		// left sits in `Outbound` until something pushes it — and the caller
		// that is going to push it is usually the one waiting for the answer,
		// calling nothing but `Receive`. Pumping only inbound there deadlocks
		// the pair at the first frame too big to fit, which is a bug that hides
		// completely behind small messages.
		//
		// This is also why the interface has no `Flush`: a caller that had to
		// remember one could tell which transport it held.
		void Pump() const;

		// Whether this end can still carry anything. Held-lock only.
		bool Usable() const;

		// Whether a whole frame is buffered.
		bool Complete() const;

		// Reads the frame header sitting at an offset into the inbound buffer.
		size_t LengthAt(size_t offset) const;

		// Drops the consumed prefix once it is worth the move.
		//
		// Not on every read: a channel carrying small frames would then memmove
		// its whole buffer per frame. Half is the usual compromise and it keeps
		// the amortised cost linear.
		static void Compact(std::vector<std::byte> &buffer, size_t &read);

		// Writes as much of the outbound buffer as the socket will take.
		//
		// Const because `Pending` is, and what it changes is a buffer rather
		// than anything a caller can observe as state.
		void PumpOut() const;

		// Reads whatever the socket has, up to the buffer's cap.
		void PumpIn() const;

		// `mutable` throughout, because pumping the socket is what every call
		// does first and two of them are `const`. Nothing here is observable
		// state: a frame is where it is, and whether it has reached the kernel
		// yet is not a caller's question.
		mutable std::mutex Guard;
		mutable int64_t Handle = -1;
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
