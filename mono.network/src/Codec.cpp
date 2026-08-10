#include "Codec.hpp"

#include <cstring>

namespace network {

	void WriteEndpoint(engine::core::ByteWriter &writer, const engine::net::Endpoint &value) {
		writer.WriteUInt8(static_cast<uint8_t>(value.Family));
		writer.WriteRaw(value.Address.data(), value.Address.size());
		writer.WriteUInt16(value.Port);
	}

	engine::net::Endpoint ReadEndpoint(engine::core::ByteReader &reader) {
		const uint8_t family = reader.ReadUInt8();

		engine::net::Endpoint value;
		if (!reader.ReadRaw(value.Address.data(), value.Address.size())) {
			return {};
		}
		value.Port = reader.ReadUInt16();

		switch (family) {
		case static_cast<uint8_t>(engine::net::AddressFamily::IPv4):
			value.Family = engine::net::AddressFamily::IPv4;
			break;
		case static_cast<uint8_t>(engine::net::AddressFamily::IPv6):
			value.Family = engine::net::AddressFamily::IPv6;
			break;
		default:
			// `None`, and anything a later version might add. The address bytes
			// were still consumed, so the fields after this one are where the
			// writer put them.
			return {};
		}
		return value;
	}

	void WriteSessionId(engine::core::ByteWriter &writer, const SessionId &value) {
		writer.WriteRaw(value.Value.data(), value.Value.size());
	}

	SessionId ReadSessionId(engine::core::ByteReader &reader) {
		SessionId value;
		if (!reader.ReadRaw(value.Value.data(), value.Value.size())) {
			return {};
		}
		return value;
	}
}
