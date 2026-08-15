#include "SocketChannel.hpp"

#include "platform/Socket.hpp"

#include <cstdint>

namespace engine::parallel {

	namespace {
		// The frame header. A length and nothing else - there is no version,
		// because both ends of a channel are the same binary by construction:
		// a host is `mono.server` started in host mode.
		constexpr size_t HEADER = sizeof(uint32_t);

		// How much is read from the socket in one call.
		constexpr size_t READ_CHUNK = 64u * 1024u;
	}

	SocketChannel::SocketChannel(int64_t handle, const ChannelSettings &settings)
		: Handle(handle), Settings_(settings) {}

	SocketChannel::~SocketChannel() {
		Close();
	}

	ChannelStatus SocketChannel::Send(std::span<const std::byte> frame) {
		if (frame.size() > Settings_.MaximumFrame) {
			// Refused whole rather than truncated, and checked before the
			// closed test so that an oversized frame reports the reason it will
			// still be oversized next time.
			return ChannelStatus::TooLarge;
		}

		std::lock_guard lock(Guard);
		if (!Usable()) {
			return ChannelStatus::Closed;
		}

		// First, because the room this frame needs may be room the socket has
		// already taken.
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
			// Little-endian by shift, the same as `core::ByteWriter`: written
			// the same way on every machine rather than however this one
			// happens to store an integer.
			header[index] = static_cast<std::byte>((length >> (8u * index)) & 0xFFu);
		}

		Outbound.insert(Outbound.end(), header, header + HEADER);
		Outbound.insert(Outbound.end(), frame.begin(), frame.end());

		PumpOut();
		return Usable() ? ChannelStatus::Ok : ChannelStatus::Closed;
	}

	ChannelStatus SocketChannel::Receive(std::vector<std::byte> &frame) {
		std::lock_guard lock(Guard);
		Pump();

		if (!Complete()) {
			// Closed *and* drained, in that order. A peer that exits cleanly
			// should not strip this end of what it already said.
			return Usable() ? ChannelStatus::Empty : ChannelStatus::Closed;
		}

		const size_t length = LengthAt(InboundRead);
		const size_t start = InboundRead + HEADER;

		// Assigned rather than moved into, so the caller's capacity survives
		// and a channel polled every tick stops allocating.
		frame.assign(
			Inbound.begin() + static_cast<ptrdiff_t>(start),
			Inbound.begin() + static_cast<ptrdiff_t>(start + length)
		);

		InboundRead = start + length;
		Compact(Inbound, InboundRead);
		return ChannelStatus::Ok;
	}

	size_t SocketChannel::Pending() const {
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

	size_t SocketChannel::PendingBytes() const {
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

	bool SocketChannel::Open() const {
		std::lock_guard lock(Guard);
		Pump();
		return Usable();
	}

	void SocketChannel::Close() {
		std::lock_guard lock(Guard);
		if (Handle < 0) {
			return;
		}

		// One last push, so a host that queued a final frame and exited does
		// not take it with it.
		PumpOut();

		platform::SocketClose(Handle);
		Handle = -1;
	}

	void SocketChannel::Pump() const {
		PumpOut();
		PumpIn();
	}

	bool SocketChannel::Usable() const {
		return Handle >= 0 && PeerAlive;
	}

	bool SocketChannel::Complete() const {
		if (InboundRead + HEADER > Inbound.size()) {
			return false;
		}
		return InboundRead + HEADER + LengthAt(InboundRead) <= Inbound.size();
	}

	size_t SocketChannel::LengthAt(size_t offset) const {
		uint32_t length = 0;
		for (size_t index = 0; index < HEADER; index++) {
			length |= static_cast<uint32_t>(Inbound[offset + index]) << (8u * index);
		}
		return length;
	}

	void SocketChannel::Compact(std::vector<std::byte> &buffer, size_t &read) {
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

	void SocketChannel::PumpOut() const {
		while (Handle >= 0 && OutboundRead < Outbound.size()) {
			const size_t remaining = Outbound.size() - OutboundRead;
			const ptrdiff_t written = platform::SocketSend(Handle, Outbound.data() + OutboundRead, remaining);

			if (written >= 0) {
				OutboundRead += static_cast<size_t>(written);
				continue;
			}
			if (written == platform::SOCKET_BLOCKED) {
				// The socket is full. Not an error: the rest stays buffered and
				// goes out on a later call.
				break;
			}

			// The peer is gone, so nothing queued will ever arrive and holding
			// it would be a leak with a hopeful name.
			PeerAlive = false;
			break;
		}
		Compact(Outbound, OutboundRead);
	}

	void SocketChannel::PumpIn() const {
		while (Handle >= 0 && PeerAlive) {
			if (Inbound.size() - InboundRead >= Settings_.Capacity) {
				// Left in the kernel deliberately. That is what makes the
				// peer's `Send` start refusing, which is backpressure reaching
				// the producer rather than the machine filling up quietly.
				break;
			}

			const size_t before = Inbound.size();
			Inbound.resize(before + READ_CHUNK);

			const ptrdiff_t got = platform::SocketReceive(Handle, Inbound.data() + before, READ_CHUNK);

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
			if (got == platform::SOCKET_BLOCKED) {
				break;
			}

			PeerAlive = false;
			break;
		}
	}
}
