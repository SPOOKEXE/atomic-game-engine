#pragma once

#include <engine/world/PresentationBus.hpp>

#include <functional>

namespace engine::world {
	// One authenticated, reliable, ordered play connection owns one stream.
	// This codec grants no endpoint authority. The receiving adapter validates
	// directory ownership and message receipts before admitting them to a universe.
	enum class PresentationStreamKind : uint8_t { Directory = 1, Routes, Message };
	struct PresentationStreamFrame {
		PresentationStreamKind Kind = PresentationStreamKind::Message;
		PresentationDirectory Directory;
		PresentationMessage Message;
	};
	enum class PresentationStreamReceive : uint8_t { Other, Accepted, Refused };

	class PresentationStream {
	  public:
		static constexpr size_t PACKET_BYTES = 1024;
		static constexpr size_t MAXIMUM_FRAME_BYTES = MAX_PRESENTATION_PAYLOAD + 4096;
		PresentationStream() = default;
		PresentationStream(const PresentationStream &) = delete;
		PresentationStream &operator=(const PresentationStream &) = delete;
		static bool Recognizes(std::span<const std::byte> packet);
		PresentationStatus Queue(const PresentationStreamFrame &frame);
		// Send must copy accepted packets and must not reenter this stream.
		// Refusal retains the exact next packet; each call has a bounded send budget.
		size_t Flush(const std::function<bool(std::span<const std::byte>)> &send, size_t maximumPackets = 64);
		// Malformed or over-budget presentation packets close only this stream.
		// The owner then retires its endpoints; unrelated play messages return Other.
		PresentationStreamReceive Receive(std::span<const std::byte> packet);
		std::vector<PresentationStreamFrame> Take();
		void Close();
		bool Open() const {
			return Live;
		}
		// Encoded queued bytes, including the current assembly reservation, not heap capacity.
		PresentationQueueSize Incoming() const {
			return ReceivedSize;
		}
		PresentationQueueSize Outgoing() const {
			return SendSize;
		}
		// Invalid incoming presentation packets, excluding ordinary send backpressure.
		uint64_t Refused() const {
			return Refusals;
		}

	  private:
		PresentationStreamReceive Fail();
		bool Live = true;
		uint64_t Refusals = 0;
		uint64_t SendingSequence = 1;
		uint64_t ReceivingSequence = 1;
		size_t SendingOffset = 0;
		uint32_t ReceivingBytes = 0;
		std::vector<std::vector<std::byte>> Sending;
		std::vector<std::byte> Assembling;
		std::vector<PresentationStreamFrame> Received;
		PresentationQueueSize SendSize;
		PresentationQueueSize ReceivedSize;
		core::ByteWriter Packet{PACKET_BYTES};
	};
}
