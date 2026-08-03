#include <engine/core/Metrics.hpp>
#include <engine/net/Packet.hpp>

namespace engine::net {

	bool Packet::WriteHeader(core::ByteWriter &writer, const PacketHeader &header, size_t payloadBytes) {
		if (payloadBytes > MAXIMUM_PAYLOAD_BYTES) {
			// Nothing written, rather than a truncated packet. A frame that says
			// one length and carries another is the exact shape the reader below
			// exists to refuse, and producing one locally would be a bug that
			// only ever surfaces on the far side.
			return false;
		}

		writer.WriteUInt32(MAGIC);
		writer.WriteUInt16(VERSION);
		writer.WriteUInt8(static_cast<uint8_t>(header.Channel));
		writer.WriteUInt16(header.Sequence);
		writer.WriteUInt16(header.Acknowledge);
		writer.WriteUInt32(header.AcknowledgeBits);
		writer.WriteUInt64(header.Counter);
		writer.WriteUInt16(static_cast<uint16_t>(payloadBytes));
		return true;
	}

	bool
	Packet::Write(core::ByteWriter &writer, const PacketHeader &header, std::span<const std::byte> payload) {
		if (!WriteHeader(writer, header, payload.size())) {
			return false;
		}
		if (!payload.empty()) {
			writer.WriteRaw(payload.data(), payload.size());
		}
		return true;
	}

	std::optional<Packet::Inbound> Packet::Read(core::ByteReader &reader) {
		const auto refuse = [&reader]() -> std::optional<Inbound> {
			// One flag carries the verdict for the whole buffer, so a caller
			// reading further from it cannot miss the refusal.
			reader.Fail();
			core::Metrics::Count("net.packet.refused", 1.0);
			return std::nullopt;
		};

		// Borrowed whole and then parsed, so that the bytes the tag was computed
		// over are the bytes that arrived rather than a re-serialisation of what
		// they parsed into. The two would agree today; the day they stop, every
		// packet is refused and it reads as a dead network.
		Inbound inbound;
		inbound.HeaderBytes = reader.ReadRawView(HEADER_BYTES);
		if (reader.Failed()) {
			return refuse();
		}

		core::ByteReader head(inbound.HeaderBytes);

		if (head.ReadUInt32() != MAGIC) {
			return refuse();
		}
		if (head.ReadUInt16() != VERSION) {
			// Refused rather than negotiated downward. A server speaking an old
			// version to an old client is a server running two protocols, and
			// the second one is the one nobody tests.
			return refuse();
		}

		const uint8_t channel = head.ReadUInt8();
		if (channel > static_cast<uint8_t>(ChannelKind::Handshake)) {
			// A byte outside the enum. Casting it anyway would produce a
			// `ChannelKind` no switch handles, and every `Describe` and every
			// dispatch downstream would then be reading a value the type says
			// cannot exist.
			return refuse();
		}
		inbound.Header.Channel = static_cast<ChannelKind>(channel);

		inbound.Header.Sequence = head.ReadUInt16();
		inbound.Header.Acknowledge = head.ReadUInt16();
		inbound.Header.AcknowledgeBits = head.ReadUInt32();
		inbound.Header.Counter = head.ReadUInt64();

		const uint16_t length = head.ReadUInt16();
		if (length > MAXIMUM_PAYLOAD_BYTES) {
			return refuse();
		}

		// ReadRawView refuses rather than clamping when the buffer is short, so
		// a length field claiming more than arrived cannot hand back a view over
		// memory the packet did not contain.
		inbound.Payload = reader.ReadRawView(length);
		if (reader.Failed()) {
			return refuse();
		}

		core::Metrics::Count("net.packet.read", 1.0);
		return inbound;
	}

	std::optional<ChannelKind> Packet::PeekChannel(std::span<const std::byte> datagram) {
		core::ByteReader reader(datagram);

		if (reader.ReadUInt32() != MAGIC || reader.ReadUInt16() != VERSION) {
			return std::nullopt;
		}

		const uint8_t channel = reader.ReadUInt8();
		if (reader.Failed() || channel > static_cast<uint8_t>(ChannelKind::Handshake)) {
			// Range-checked before the cast, the same as `Read` does it. A
			// router switching on a value outside the enum is the one place
			// where a byte from a stranger picks a code path the type says
			// cannot be reached.
			return std::nullopt;
		}

		return static_cast<ChannelKind>(channel);
	}

	bool Packet::IsNewer(uint16_t sequence, uint16_t against) {
		// The half-range comparison. `sequence` is newer when it sits within the
		// next 32768 values, which is what makes 0 newer than 65535 rather than
		// 65535 newer than everything for the rest of the session.
		//
		// Both halves are needed: the first covers the ordinary case and the
		// second covers the wrap, and either alone is wrong for half the range.
		constexpr uint16_t HALF = 32768;
		return (sequence > against && sequence - against <= HALF) ||
			   (sequence < against && against - sequence > HALF);
	}
}
