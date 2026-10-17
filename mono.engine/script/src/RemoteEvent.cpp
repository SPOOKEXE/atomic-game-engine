#include <engine/core/Bytes.hpp>
#include <engine/script/RemoteEvent.hpp>

namespace engine::script {
	bool EncodeRemoteEvent(
		std::string_view event, std::span<const std::byte> payload, std::vector<std::byte> &out
	) {
		out.clear();
		if (event.empty() || payload.size() > REMOTE_EVENT_MAXIMUM_PAYLOAD_BYTES) return false;
		core::ByteWriter writer(0, REMOTE_EVENT_MAXIMUM_MESSAGE_BYTES);
		writer.WriteUInt32(REMOTE_EVENT_MAGIC);
		writer.WriteUInt16(REMOTE_EVENT_VERSION);
		writer.WriteString(event);
		writer.WriteUInt32(static_cast<uint32_t>(payload.size()));
		writer.WriteRaw(payload.data(), payload.size());
		out = writer.TakeBytes();
		return true;
	}

	bool DecodeRemoteEvent(std::span<const std::byte> bytes, RemoteEventMessage &out) {
		if (bytes.size() > REMOTE_EVENT_MAXIMUM_MESSAGE_BYTES) return false;
		core::ByteReader reader(bytes);
		const uint32_t magic = reader.ReadUInt32();
		const uint16_t version = reader.ReadUInt16();
		const std::string_view event = reader.ReadString();
		const uint32_t payloadBytes = reader.ReadUInt32();
		if (reader.Failed() || magic != REMOTE_EVENT_MAGIC || version != REMOTE_EVENT_VERSION ||
			event.empty() || payloadBytes > REMOTE_EVENT_MAXIMUM_PAYLOAD_BYTES ||
			payloadBytes != reader.Remaining())
			return false;

		RemoteEventMessage decoded;
		decoded.Event = event;
		decoded.Payload.resize(payloadBytes);
		if (payloadBytes > 0) reader.ReadRaw(decoded.Payload.data(), payloadBytes);
		if (reader.Failed()) return false;
		out = std::move(decoded);
		return true;
	}
}
