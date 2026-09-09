#include <engine/core/Bytes.hpp>
#include <engine/core/Log.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/game/Play.hpp>
#include <engine/scene/Characters.hpp>
#include <engine/scene/Controls.hpp>
#include <engine/script/PortalTransfer.hpp>

#include <cmath>

namespace engine::game {

	namespace {
		bool ValidPlayerMotion(const PlayerMotion &sample) {
			return sample.Player != ecs::NULL_ENTITY && sample.Root != ecs::NULL_ENTITY &&
				   sample.Player != sample.Root && sample.Motion.InputTick != 0 &&
				   script::ValidPortalTransferMotion(sample.Motion);
		}
		// A tag, three floats, a flag and input step duration. Stated as a constant because
		// `DecodeMoveInput` refuses anything else - see the header for why the
		// length and not the tag alone is what separates this from a shot.
		constexpr size_t MOVE_BYTES = 1 + 3 * sizeof(float) + 1 + sizeof(double);

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
		writer.WriteDouble(input.StepSeconds);
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
		input.StepSeconds = reader.ReadDouble();

		if (reader.Failed()) {
			return false;
		}

		// **A NaN would poison the character's transform for the rest of the
		// run**, and it costs one client one packet to send. Every value that
		// arrives from a peer is checked here rather than where it is used,
		// because there is one decoder and several readers.
		if (!std::isfinite(input.Direction.X) || !std::isfinite(input.Direction.Y) ||
			!std::isfinite(input.Direction.Z) || !std::isfinite(input.StepSeconds) || input.StepSeconds < 0) {
			return false;
		}

		// **Normalised by the host and never by the sender.** A client that
		// sent a direction of length ten would otherwise walk ten times as
		// fast, which is the oldest cheat there is.
		const float length = input.Direction.Magnitude();
		input.Direction = length > 0.0f ? input.Direction * (1.0f / length) : core::Vector3{};
		out = input;
		return true;
	}

	std::vector<std::byte> EncodePlayerMotion(const PlayerMotion &sample) {
		if (!ValidPlayerMotion(sample)) return {};
		core::ByteWriter writer;
		writer.WriteUInt8(static_cast<uint8_t>(PlayMessage::PlayerMotion));
		writer.WriteUInt64(sample.Player.Id);
		writer.WriteUInt64(sample.Root.Id);
		if (!script::WritePortalTransferMotion(writer, sample.Motion)) return {};
		return {writer.Bytes().begin(), writer.Bytes().end()};
	}

	bool DecodePlayerMotion(std::span<const std::byte> bytes, PlayerMotion &out) {
		core::ByteReader reader(bytes);
		if (reader.ReadUInt8() != static_cast<uint8_t>(PlayMessage::PlayerMotion)) return false;
		PlayerMotion sample;
		sample.Player = ecs::Entity(reader.ReadUInt64());
		sample.Root = ecs::Entity(reader.ReadUInt64());
		if (!script::ReadPortalTransferMotion(reader, sample.Motion) || reader.Remaining() != 0 ||
			!ValidPlayerMotion(sample))
			return false;
		out = sample;
		return true;
	}

	std::optional<PlayerMotion>
	CapturePlayerMotion(const ecs::Store &store, ecs::Entity player, uint64_t consumedInput) {
		if (store.AdoptOnly() || consumedInput == 0) return {};
		if (const auto applied = script::AppliedPortalPlayerInput(store, player)) consumedInput = *applied;
		const auto *rig = store.Get<scene::Character>(scene::CharacterOf(store, player));
		if (!rig) return {};
		const auto *root = store.Get<scene::Transform>(rig->Root);
		const auto *velocity = store.Get<scene::Motion>(rig->Root);
		const auto *humanoid = store.Get<scene::Humanoid>(rig->Humanoid);
		if (!root || !velocity || !humanoid) return {};
		PlayerMotion sample{
			player,
			rig->Root,
			{script::PortalTransferIncarnation(store),
			 store.Time().Tick,
			 consumedInput,
			 root->Frame,
			 velocity->Linear,
			 velocity->Angular,
			 humanoid->WalkSpeed,
			 humanoid->JumpSpeed,
			 humanoid->Grounded,
			 store.Time().Elapsed}
		};
		if (!ValidPlayerMotion(sample)) return {};
		return sample;
	}

	bool ApplyMoveInput(ecs::Store &store, ecs::Entity player, const MoveInput &move, uint64_t inputTick) {
		const bool forwarded = script::ForwardPortalPlayerMove(
			store, player, move.Direction, move.Jump, inputTick, move.StepSeconds
		);
		const ecs::Entity character = scene::CharacterOf(store, player);

		const auto *rig = store.Get<scene::Character>(character);
		if (rig == nullptr) {
			return forwarded;
		}

		auto *humanoid = store.GetMutable<scene::Humanoid>(rig->Humanoid);
		if (humanoid == nullptr) {
			return forwarded;
		}

		const auto disposition = script::SchedulePortalPlayerMove(
			store, player, move.Direction, move.Jump, inputTick, move.StepSeconds
		);
		if (disposition != script::PortalInputDisposition::Immediate)
			return disposition == script::PortalInputDisposition::Queued;
		script::ClosePortalPlayerMoveForwarding(store, player);
		const auto previousDirection = humanoid->MoveDirection;
		humanoid->MoveDirection = move.Direction;
		humanoid->JumpRequested = humanoid->JumpRequested || move.Jump;
		if (previousDirection != core::Vector3{} && humanoid->MoveDirection == core::Vector3{}) {
			ENGINE_LOG(
				core::LogLevel::Trace,
				"portal-input-stop",
				"route=native incarnation={} player={} humanoid={} world_tick={} input_tick={} "
				"direction=0,0,0",
				script::PortalTransferIncarnation(store),
				player.Id,
				rig->Humanoid.Id,
				store.Time().Tick,
				inputTick
			);
		}
		static const core::LogCategory controlTrace("portal-input");
		if (controlTrace.Enabled(core::LogLevel::Trace)) {
			const auto *root = store.Get<scene::Transform>(rig->Root);
			ENGINE_LOG(
				core::LogLevel::Trace,
				"portal-input",
				"route=native incarnation={} player={} world_tick={} input_tick={} delta={} input_step={} "
				"direction={},{},{} position={},{},{} humanoid={} applied_direction={},{},{}",
				script::PortalTransferIncarnation(store),
				player.Id,
				store.Time().Tick,
				inputTick,
				store.Time().Delta,
				move.StepSeconds,
				move.Direction.X,
				move.Direction.Y,
				move.Direction.Z,
				root ? root->Frame.Position.X : 0,
				root ? root->Frame.Position.Y : 0,
				root ? root->Frame.Position.Z : 0,
				rig->Humanoid.Id,
				humanoid->MoveDirection.X,
				humanoid->MoveDirection.Y,
				humanoid->MoveDirection.Z
			);
		}
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
