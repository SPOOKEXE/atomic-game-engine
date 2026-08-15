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

		std::vector<std::byte> Finish(const core::ByteWriter &writer) {
			const std::span<const std::byte> bytes = writer.Bytes();
			return {bytes.begin(), bytes.end()};
		}
	}

	std::vector<std::byte> EncodeJoinNotice(const JoinNotice &notice) {
		core::ByteWriter writer;
		writer.WriteUInt8(static_cast<uint8_t>(PlayMessage::AssignPlayer));
		writer.WriteUInt64(notice.Player.Id);
		return Finish(writer);
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
		return Finish(writer);
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
}
