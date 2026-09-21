#pragma once

#include <engine/world/PresentationBus.hpp>

#include <functional>

namespace engine::world {
	// One authenticated, reliable, ordered play connection owns one stream.
	// This codec grants no endpoint authority. The receiving adapter validates
	// directory ownership and message receipts before admitting them to a universe.
	// Payload forms carried by the private presentation connection.
	enum class PresentationStreamKind : uint8_t { Directory = 1, Routes, Message };
	// One decoded directory, route set, or presentation message.
	struct PresentationStreamFrame {
		// Selects which payload member is meaningful.
		PresentationStreamKind Kind = PresentationStreamKind::Message;
		// Complete endpoint directory or route set for Directory and Routes.
		PresentationDirectory Directory;
		// Owned presentation payload for Message.
		PresentationMessage Message;
	};
	// Result of classifying a received packet for this stream.
	enum class PresentationStreamReceive : uint8_t { Other, Accepted, Refused };

	// Bounded packetizer for one authenticated presentation connection.
	class PresentationStream {
	  public:
		// Fixed transport packet size used for stream fragments.
		static constexpr size_t PACKET_BYTES = 1024;
		// Largest encoded frame accepted before packet fragmentation.
		static constexpr size_t MAXIMUM_FRAME_BYTES = MAX_PRESENTATION_PAYLOAD + 4096;
		PresentationStream() = default;
		PresentationStream(const PresentationStream &) = delete;
		PresentationStream &operator=(const PresentationStream &) = delete;
		// Reports whether a packet carries this stream's framing marker.
		static bool Recognizes(std::span<const std::byte> packet);
		// Encodes and queues one bounded frame for ordered transmission.
		PresentationStatus Queue(const PresentationStreamFrame &frame);
		// Send must copy accepted packets and must not reenter this stream.
		// Refusal retains the exact next packet; each call has a bounded send budget.
		size_t Flush(const std::function<bool(std::span<const std::byte>)> &send, size_t maximumPackets = 64);
		// Malformed or over-budget presentation packets close only this stream.
		// The owner then retires its endpoints; unrelated play messages return Other.
		PresentationStreamReceive Receive(std::span<const std::byte> packet);
		// Transfers all fully decoded frames to the connection owner.
		std::vector<PresentationStreamFrame> Take();
		// Refuses future traffic and discards incomplete assembly state.
		void Close();
		// Reports whether the connection remains available for presentation traffic.
		bool Open() const {
			return Live;
		}
		// Encoded queued bytes, including the current assembly reservation, not heap capacity.
		PresentationQueueSize Incoming() const {
			return ReceivedSize;
		}
		// Returns encoded bytes and frames retained for transmission.
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
