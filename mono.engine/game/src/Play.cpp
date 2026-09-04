#include <engine/core/Bytes.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/game/Play.hpp>
#include <engine/scene/Characters.hpp>
#include <engine/scene/Controls.hpp>

#include <cmath>

namespace engine::game {

	namespace {
		// A tag, three floats and a flag. Stated as a constant because
		// `DecodeMoveInput` refuses anything else - see the header for why the
		// length and not the tag alone is what separates this from a shot.
		constexpr size_t MOVE_BYTES = 1 + 3 * sizeof(float) + 1;

		constexpr size_t MAXIMUM_TELEPORT_PLACE_BYTES = 256;
		constexpr size_t MAXIMUM_TELEPORT_DATA_BYTES = 64u * 1024u;
		constexpr size_t MAXIMUM_TELEPORT_MESSAGE_BYTES = 1024;

		bool ValidTeleportRequest(const TeleportRequest &request) {
			return request.Id != 0 && !request.Place.empty() &&
				   request.Place.size() <= MAXIMUM_TELEPORT_PLACE_BYTES &&
				   request.Data.size() <= MAXIMUM_TELEPORT_DATA_BYTES;
		}

		bool ValidTeleportResult(const TeleportRequestResult &result) {
			return result.Id != 0 && result.Message.size() <= MAXIMUM_TELEPORT_MESSAGE_BYTES &&
				   static_cast<uint8_t>(result.Decision) <=
					   static_cast<uint8_t>(TeleportRequestDecision::Processed);
		}

		std::vector<std::byte> FinishedBytes(const core::ByteWriter &writer) {
			const std::span<const std::byte> bytes = writer.Bytes();
			return {bytes.begin(), bytes.end()};
		}
	}

	std::vector<std::byte> EncodeJoinNotice(const JoinNotice &notice) {
		core::ByteWriter writer;
		writer.WriteUInt8(static_cast<uint8_t>(PlayMessage::AssignPlayer));
		writer.WriteUInt64(notice.Player.Id);
		return FinishedBytes(writer);
	}

	bool DecodeJoinNotice(std::span<const std::byte> message, JoinNotice &out) {
		core::ByteReader reader(message);

		const uint8_t tag = reader.ReadUInt8();
		if (reader.Failed() || tag != static_cast<uint8_t>(PlayMessage::AssignPlayer)) {
			return false;
		}

		const uint64_t id = reader.ReadUInt64();
		if (reader.Failed()) {
			return false;
		}

		out.Player = ecs::Entity(id);
		return true;
	}

	std::vector<std::byte> EncodeMoveInput(const MoveInput &input) {
		core::ByteWriter writer;
		writer.WriteUInt8(static_cast<uint8_t>(PlayMessage::Move));
		writer.WriteFloat(input.Direction.X);
		writer.WriteFloat(input.Direction.Y);
		writer.WriteFloat(input.Direction.Z);
		writer.WriteUInt8(input.Jump ? 1 : 0);
		return FinishedBytes(writer);
	}

	bool DecodeMoveInput(std::span<const std::byte> bytes, MoveInput &out) {
		if (bytes.size() != MOVE_BYTES) {
			return false;
		}

		core::ByteReader reader(bytes);
		if (reader.ReadUInt8() != static_cast<uint8_t>(PlayMessage::Move)) {
			return false;
		}

		MoveInput input;
		input.Direction.X = reader.ReadFloat();
		input.Direction.Y = reader.ReadFloat();
		input.Direction.Z = reader.ReadFloat();
		input.Jump = reader.ReadUInt8() != 0;

		if (reader.Failed()) {
			return false;
		}

		// **A NaN would poison the character's transform for the rest of the
		// run**, and it costs one client one packet to send. Every value that
		// arrives from a peer is checked here rather than where it is used,
		// because there is one decoder and several readers.
		if (!std::isfinite(input.Direction.X) || !std::isfinite(input.Direction.Y) ||
			!std::isfinite(input.Direction.Z)) {
			return false;
		}

		// **Normalised by the host and never by the sender.** A client that
		// sent a direction of length ten would otherwise walk ten times as
		// fast, which is the oldest cheat there is.
		const float length = input.Direction.Magnitude();
		out.Direction = length > 0.0f ? input.Direction * (1.0f / length) : core::Vector3{};
		out.Jump = input.Jump;
		return true;
	}

	bool ApplyMoveInput(ecs::Store &store, ecs::Entity player, const MoveInput &move) {
		const ecs::Entity character = scene::CharacterOf(store, player);

		const auto *rig = store.Get<scene::Character>(character);
		if (rig == nullptr) {
			return false;
		}

		auto *humanoid = store.GetMutable<scene::Humanoid>(rig->Humanoid);
		if (humanoid == nullptr) {
			return false;
		}

		humanoid->MoveDirection = move.Direction;
		humanoid->JumpRequested = humanoid->JumpRequested || move.Jump;
		return true;
	}

	std::vector<std::byte> EncodeTeleportRequest(const TeleportRequest &request) {
		if (!ValidTeleportRequest(request)) {
			return {};
		}

		core::ByteWriter writer;
		writer.WriteUInt8(static_cast<uint8_t>(PlayMessage::TeleportRequest));
		writer.WriteUInt64(request.Id);
		writer.WriteString(request.Place);
		const uint32_t dataBytes = static_cast<uint32_t>(request.Data.size());
		writer.WriteUInt32(dataBytes);
		writer.WriteRaw(request.Data.data(), request.Data.size());
		return FinishedBytes(writer);
	}

	bool DecodeTeleportRequest(std::span<const std::byte> bytes, TeleportRequest &out) {
		core::ByteReader reader(bytes);
		if (reader.ReadUInt8() != static_cast<uint8_t>(PlayMessage::TeleportRequest)) {
			return false;
		}

		TeleportRequest request;
		request.Id = reader.ReadUInt64();
		const std::string_view place = reader.ReadString();
		const uint32_t dataBytes = reader.ReadUInt32();
		if (reader.Failed() || request.Id == 0 || place.empty() ||
			place.size() > MAXIMUM_TELEPORT_PLACE_BYTES || dataBytes > MAXIMUM_TELEPORT_DATA_BYTES) {
			return false;
		}

		request.Place = place;
		request.Data.resize(dataBytes);
		if (dataBytes != 0 && !reader.ReadRaw(request.Data.data(), dataBytes)) {
			return false;
		}
		if (!reader.AtEnd()) {
			return false;
		}

		out = std::move(request);
		return true;
	}

	std::vector<std::byte> EncodeTeleportResult(const TeleportRequestResult &result) {
		if (!ValidTeleportResult(result)) {
			return {};
		}

		core::ByteWriter writer;
		writer.WriteUInt8(static_cast<uint8_t>(PlayMessage::TeleportResult));
		writer.WriteUInt64(result.Id);
		writer.WriteUInt8(static_cast<uint8_t>(result.Decision));
		writer.WriteString(result.Message);
		return FinishedBytes(writer);
	}

	bool DecodeTeleportResult(std::span<const std::byte> bytes, TeleportRequestResult &out) {
		core::ByteReader reader(bytes);
		if (reader.ReadUInt8() != static_cast<uint8_t>(PlayMessage::TeleportResult)) {
			return false;
		}

		TeleportRequestResult result;
		result.Id = reader.ReadUInt64();
		const uint8_t decision = reader.ReadUInt8();
		const std::string_view message = reader.ReadString();
		if (reader.Failed() || result.Id == 0 || message.size() > MAXIMUM_TELEPORT_MESSAGE_BYTES ||
			decision > static_cast<uint8_t>(TeleportRequestDecision::Processed) || !reader.AtEnd()) {
			return false;
		}

		result.Decision = static_cast<TeleportRequestDecision>(decision);
		result.Message = message;
		out = std::move(result);
		return true;
	}
}
