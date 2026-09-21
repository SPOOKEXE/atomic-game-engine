#include <engine/core/Bytes.hpp>
#include <engine/render/PortalShadowTransport.hpp>

#include <cmath>

namespace engine::render {
	namespace {
		constexpr uint32_t MAGIC = 0x54434853;
		constexpr uint16_t VERSION = 1;
		constexpr uint8_t PULL = 1, PACKET = 2;
		constexpr size_t MAX_TEXT = 256;

		bool Fail(std::string &error) {
			error = "invalid or over-budget portal shadow transport";
			return false;
		}
		bool Text(std::string_view text) {
			return !text.empty() && text.size() <= MAX_TEXT && text.find('\0') == std::string_view::npos;
		}
		bool Valid(const PortalShadowPull &pull) {
			return pull.Part <= PORTAL_SHADOW_CANCEL_PART && pull.ParentEye.RequestId &&
				   Text(pull.ParentEye.PortalKey) && pull.TargetEye.RequestId &&
				   Text(pull.TargetEye.PortalKey) && Text(pull.TargetProducer.World) &&
				   Text(pull.TargetProducer.Channel) && pull.TargetProducer.Session &&
				   pull.TargetProducer.Generation &&
				   (!pull.BodyBounds ||
					(std::isfinite(pull.BodyBounds->Minimum.X) && std::isfinite(pull.BodyBounds->Minimum.Y) &&
					 std::isfinite(pull.BodyBounds->Minimum.Z) && std::isfinite(pull.BodyBounds->Maximum.X) &&
					 std::isfinite(pull.BodyBounds->Maximum.Y) && std::isfinite(pull.BodyBounds->Maximum.Z) &&
					 pull.BodyBounds->Minimum.X <= pull.BodyBounds->Maximum.X &&
					 pull.BodyBounds->Minimum.Y <= pull.BodyBounds->Maximum.Y &&
					 pull.BodyBounds->Minimum.Z <= pull.BodyBounds->Maximum.Z));
		}
		void Header(core::ByteWriter &writer, uint8_t kind) {
			writer.WriteUInt32(MAGIC);
			writer.WriteUInt16(VERSION);
			writer.WriteUInt8(kind);
			writer.WriteUInt8(0);
		}
		bool Header(core::ByteReader &reader, uint8_t kind) {
			return reader.ReadUInt32() == MAGIC && reader.ReadUInt16() == VERSION &&
				   reader.ReadUInt8() == kind && reader.ReadUInt8() == 0 && !reader.Failed();
		}
		void Key(core::ByteWriter &writer, const PortalExchangeKey &key) {
			writer.WriteUInt64(key.RequestId);
			writer.WriteString(key.PortalKey);
			writer.WriteUInt64(key.CameraRevision);
			writer.WriteUInt64(key.SeamRevision);
		}
		bool ReadText(core::ByteReader &reader, std::string &out) {
			const auto text = reader.ReadString();
			if (reader.Failed() || !Text(text)) return false;
			out = text;
			return true;
		}
		bool Key(core::ByteReader &reader, PortalExchangeKey &key) {
			key.RequestId = reader.ReadUInt64();
			if (!ReadText(reader, key.PortalKey)) return false;
			key.CameraRevision = reader.ReadUInt64();
			key.SeamRevision = reader.ReadUInt64();
			return !reader.Failed();
		}
		void Pull(core::ByteWriter &writer, const PortalShadowPull &pull) {
			Key(writer, pull.ParentEye);
			writer.WriteString(pull.TargetProducer.World);
			writer.WriteString(pull.TargetProducer.Channel);
			writer.WriteUInt64(pull.TargetProducer.Session);
			writer.WriteUInt64(pull.TargetProducer.Generation);
			Key(writer, pull.TargetEye);
			writer.WriteUInt8(pull.BodyBounds.has_value());
			if (pull.BodyBounds)
				for (const float value :
					 {pull.BodyBounds->Minimum.X,
					  pull.BodyBounds->Minimum.Y,
					  pull.BodyBounds->Minimum.Z,
					  pull.BodyBounds->Maximum.X,
					  pull.BodyBounds->Maximum.Y,
					  pull.BodyBounds->Maximum.Z})
					writer.WriteFloat(value);
			writer.WriteUInt8(pull.Part);
		}
		bool Pull(core::ByteReader &reader, PortalShadowPull &pull) {
			if (!Key(reader, pull.ParentEye) || !ReadText(reader, pull.TargetProducer.World) ||
				!ReadText(reader, pull.TargetProducer.Channel))
				return false;
			pull.TargetProducer.Session = reader.ReadUInt64();
			pull.TargetProducer.Generation = reader.ReadUInt64();
			if (!Key(reader, pull.TargetEye)) return false;
			const auto hasBounds = reader.ReadUInt8();
			if (hasBounds > 1) return false;
			if (hasBounds) {
				core::AABB bounds;
				bounds.Minimum = {reader.ReadFloat(), reader.ReadFloat(), reader.ReadFloat()};
				bounds.Maximum = {reader.ReadFloat(), reader.ReadFloat(), reader.ReadFloat()};
				pull.BodyBounds = bounds;
			}
			pull.Part = reader.ReadUInt8();
			return !reader.Failed() && Valid(pull);
		}
		bool
		Payload(const PortalShadowPull &pull, PortalImageStatus status, std::span<const std::byte> bytes) {
			if (status > PortalImageStatus::Failed) return false;
			if (status != PortalImageStatus::Ok || pull.Part == PORTAL_SHADOW_CANCEL_PART)
				return bytes.empty();
			PortalShadowSnapshot snapshot;
			std::string error;
			if (pull.Part == PORTAL_SHADOW_MANIFEST_PART) {
				if (!DecodePortalShadowManifest(bytes, snapshot, error)) return false;
			} else {
				uint8_t tile = 0;
				if (!DecodePortalShadowTileMetadata(bytes, snapshot, tile, error) || tile + 1 != pull.Part)
					return false;
			}
			return snapshot.Producer == pull.TargetProducer && snapshot.Eye == pull.TargetEye;
		}
	}

	bool
	EncodePortalShadowPull(const PortalShadowPull &pull, std::vector<std::byte> &out, std::string &error) {
		if (!Valid(pull)) return Fail(error);
		core::ByteWriter writer;
		Header(writer, PULL);
		Pull(writer, pull);
		if (writer.Size() > MAX_PORTAL_EXCHANGE_BYTES) return Fail(error);
		out.assign(writer.Bytes().begin(), writer.Bytes().end());
		error.clear();
		return true;
	}
	bool DecodePortalShadowPull(std::span<const std::byte> bytes, PortalShadowPull &out, std::string &error) {
		if (bytes.size() > MAX_PORTAL_EXCHANGE_BYTES) return Fail(error);
		core::ByteReader reader(bytes);
		PortalShadowPull pull;
		if (!Header(reader, PULL) || !Pull(reader, pull) || reader.Remaining()) return Fail(error);
		out = std::move(pull);
		error.clear();
		return true;
	}
	bool EncodePortalShadowPacket(
		const PortalShadowPacket &packet, std::vector<std::byte> &out, std::string &error
	) {
		if (!Valid(packet.Pull) || packet.Payload.size() > MAX_PORTAL_EXCHANGE_BYTES) return Fail(error);
		core::ByteWriter writer;
		Header(writer, PACKET);
		Pull(writer, packet.Pull);
		writer.WriteUInt8(static_cast<uint8_t>(packet.Status));
		writer.WriteUInt32(static_cast<uint32_t>(packet.Payload.size()));
		if (packet.Payload.size() > MAX_PORTAL_EXCHANGE_BYTES - writer.Size() ||
			!Payload(packet.Pull, packet.Status, packet.Payload))
			return Fail(error);
		std::vector<std::byte> encoded;
		encoded.reserve(writer.Size() + packet.Payload.size());
		encoded.insert(encoded.end(), writer.Bytes().begin(), writer.Bytes().end());
		encoded.insert(encoded.end(), packet.Payload.begin(), packet.Payload.end());
		out = std::move(encoded);
		error.clear();
		return true;
	}
	bool
	DecodePortalShadowPacket(std::span<const std::byte> bytes, PortalShadowPacket &out, std::string &error) {
		if (bytes.size() > MAX_PORTAL_EXCHANGE_BYTES) return Fail(error);
		core::ByteReader reader(bytes);
		PortalShadowPacket packet;
		if (!Header(reader, PACKET) || !Pull(reader, packet.Pull)) return Fail(error);
		packet.Status = static_cast<PortalImageStatus>(reader.ReadUInt8());
		const auto size = reader.ReadUInt32();
		if (reader.Failed() || size != reader.Remaining()) return Fail(error);
		const auto payload = reader.ReadRawView(size);
		if (reader.Failed() || !Payload(packet.Pull, packet.Status, payload)) return Fail(error);
		packet.Payload.assign(payload.begin(), payload.end());
		out = std::move(packet);
		error.clear();
		return true;
	}
	bool IsPortalShadowPacket(std::span<const std::byte> bytes) {
		core::ByteReader reader(bytes);
		return Header(reader, PACKET);
	}
}
