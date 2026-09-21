#include <engine/core/Bytes.hpp>
#include <engine/game/Play.hpp>
#include <engine/scene/Characters.hpp>
#include <engine/scene/Controls.hpp>
#include <engine/scene/Registration.hpp>
#include <engine/scene/Services.hpp>
#include <engine/script/PortalTransfer.hpp>
#include <engine/script/Runtime.hpp>
#include <engine/world/Universe.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <limits>

namespace {
	TEST_CASE(
		"movement carries client tick duration independently of world time", "[game][play][move-input]"
	) {
		using namespace engine;
		game::MoveInput input{{0, 0, 1}, true, 1.0 / 120};
		const auto bytes = game::EncodeMoveInput(input);
		game::MoveInput decoded;
		REQUIRE(game::DecodeMoveInput(bytes, decoded));
		CHECK(decoded.Direction == input.Direction);
		CHECK(decoded.Jump);
		CHECK(decoded.StepSeconds == input.StepSeconds);
		for (double step :
			 {-1.0,
			  -std::numeric_limits<double>::infinity(),
			  std::numeric_limits<double>::infinity(),
			  std::numeric_limits<double>::quiet_NaN()}) {
			input.StepSeconds = step;
			CHECK_FALSE(game::DecodeMoveInput(game::EncodeMoveInput(input), decoded));
			CHECK(decoded.StepSeconds == 1.0 / 120);
		}
		input.StepSeconds = 0;
		REQUIRE(game::DecodeMoveInput(game::EncodeMoveInput(input), decoded));
		CHECK(decoded.StepSeconds == 0);
		input.StepSeconds = 2;
		REQUIRE(game::DecodeMoveInput(game::EncodeMoveInput(input), decoded));
		CHECK(decoded.StepSeconds == 2);
		for (size_t length = 0; length < bytes.size(); ++length)
			CHECK_FALSE(game::DecodeMoveInput(std::span(bytes).first(length), decoded));
		auto extra = bytes;
		extra.push_back(std::byte{0});
		CHECK_FALSE(game::DecodeMoveInput(extra, decoded));
	}
	TEST_CASE(
		"native player motion carries one pose and connection input frontier", "[game][play][player-motion]"
	) {
		using namespace engine;
		game::PlayerMotion sample{};
		sample.Player = ecs::Entity{1};
		sample.Root = ecs::Entity{2};
		sample.Motion.DestinationIncarnation = 202;
		sample.Motion.DestinationTick = 50;
		sample.Motion.InputTick = 9000;
		sample.Motion.Frame = core::CFrame(core::Vector3{3, 4, 5});
		sample.Motion.Linear = {1, 2, 3};
		sample.Motion.WalkSpeed = 16;
		sample.Motion.JumpSpeed = 7;
		sample.Motion.Grounded = true;
		sample.Motion.SimulationSeconds = 1.25;
		const auto bytes = game::EncodePlayerMotion(sample);
		REQUIRE_FALSE(bytes.empty());
		game::PlayerMotion decoded;
		REQUIRE(game::DecodePlayerMotion(bytes, decoded));
		CHECK(decoded.Player == sample.Player);
		CHECK(decoded.Root == sample.Root);
		CHECK(decoded.Motion.InputTick == 9000);
		CHECK(decoded.Motion.DestinationTick == 50);
		CHECK(decoded.Motion.SimulationSeconds == 1.25);
		CHECK(decoded.Motion.DestinationIncarnation == 202);
		CHECK(decoded.Motion.Frame.Position == sample.Motion.Frame.Position);
		CHECK(decoded.Motion.Linear == sample.Motion.Linear);
		CHECK(decoded.Motion.Grounded);
		bool prefixesRefused = true;
		for (size_t count = 0; count < bytes.size(); ++count)
			prefixesRefused &= !game::DecodePlayerMotion(std::span(bytes).first(count), decoded);
		CHECK(prefixesRefused);
		CHECK(decoded.Motion.InputTick == 9000);
		auto extra = bytes;
		extra.push_back(std::byte{0});
		CHECK_FALSE(game::DecodePlayerMotion(extra, decoded));
		for (const double invalid :
			 {-1.0, std::numeric_limits<double>::infinity(), std::numeric_limits<double>::quiet_NaN()}) {
			sample.Motion.SimulationSeconds = invalid;
			CHECK(game::EncodePlayerMotion(sample).empty());
			core::ByteWriter badTime;
			badTime.WriteDouble(invalid);
			auto malformed = bytes;
			std::copy(badTime.Bytes().begin(), badTime.Bytes().end(), malformed.end() - sizeof(double));
			CHECK_FALSE(game::DecodePlayerMotion(malformed, decoded));
			CHECK(decoded.Motion.SimulationSeconds == 1.25);
		}
		sample.Motion.SimulationSeconds = 1.25;
		sample.Root = sample.Player;
		CHECK(game::EncodePlayerMotion(sample).empty());
		sample.Root = ecs::Entity{2};
		sample.Motion.InputTick = 0;
		CHECK(game::EncodePlayerMotion(sample).empty());
	}

	using engine::game::DecodeTeleportRequest;
	using engine::game::DecodeTeleportResult;
	using engine::game::EncodeTeleportRequest;
	using engine::game::EncodeTeleportResult;
	using engine::game::PlayMessage;
	using engine::game::TeleportRequest;
	using engine::game::TeleportRequestDecision;
	using engine::game::TeleportRequestResult;

	TEST_CASE("teleport request and result preserve their typed contract", "[game][play]") {
		TeleportRequest request;
		request.Id = 42;
		request.Place = "Arena";
		request.Data = {std::byte{0x01}, std::byte{0x02}};

		TeleportRequest decodedRequest;
		REQUIRE(DecodeTeleportRequest(EncodeTeleportRequest(request), decodedRequest));
		CHECK(decodedRequest.Id == request.Id);
		CHECK(decodedRequest.Place == request.Place);
		CHECK(decodedRequest.Data == request.Data);

		TeleportRequestResult result;
		result.Id = request.Id;
		result.Decision = TeleportRequestDecision::Denied;
		result.Message = "The arena is full";

		TeleportRequestResult decodedResult;
		REQUIRE(DecodeTeleportResult(EncodeTeleportResult(result), decodedResult));
		CHECK(decodedResult.Id == result.Id);
		CHECK(decodedResult.Decision == TeleportRequestDecision::Denied);
		CHECK(decodedResult.Message == result.Message);
	}

	TEST_CASE("teleport decoders refuse foreign and incomplete messages", "[game][play]") {
		const std::vector<std::byte> foreign{std::byte{0xff}};
		TeleportRequest request;
		TeleportRequestResult result;
		CHECK_FALSE(DecodeTeleportRequest(foreign, request));
		CHECK_FALSE(DecodeTeleportResult(foreign, result));
	}

	TEST_CASE("teleport codecs keep their bounded wire contract", "[game][play]") {
		TeleportRequest request;
		request.Id = 1;
		request.Place = "Arena";
		request.Data.resize(64u * 1024u + 1);
		CHECK(EncodeTeleportRequest(request).empty());

		request.Data.clear();
		request.Id = 0;
		CHECK(EncodeTeleportRequest(request).empty());

		TeleportRequestResult result;
		result.Id = 1;
		result.Message.resize(1025, 'x');
		CHECK(EncodeTeleportResult(result).empty());
		result.Message.clear();
		result.Decision = static_cast<TeleportRequestDecision>(3);
		CHECK(EncodeTeleportResult(result).empty());

		engine::core::ByteWriter oversized;
		oversized.WriteUInt8(static_cast<uint8_t>(PlayMessage::TeleportRequest));
		oversized.WriteUInt64(1);
		oversized.WriteString("Arena");
		oversized.WriteUInt32(64u * 1024u + 1);
		CHECK_FALSE(DecodeTeleportRequest(oversized.Bytes(), request));

		engine::core::ByteWriter tooLong;
		tooLong.WriteUInt8(static_cast<uint8_t>(PlayMessage::TeleportResult));
		tooLong.WriteUInt64(1);
		tooLong.WriteUInt8(static_cast<uint8_t>(TeleportRequestDecision::Denied));
		tooLong.WriteString(std::string(1025, 'x'));
		CHECK_FALSE(DecodeTeleportResult(tooLong.Bytes(), result));
	}
}

TEST_CASE(
	"host movement follows a portal player until native destination input", "[game][play][portal-move]"
) {
	using namespace engine;
	scene::RegisterSceneClasses();
	world::Universe worlds;
	world::WorldSettings settings;
	settings.Name = core::Name("source");
	settings.TickRate = 60;
	const auto source = worlds.Create(settings);
	settings.Name = core::Name("destination");
	const auto destination = worlds.Create(settings);
	for (const auto world : {source, destination}) {
		worlds.Enter(world, [&](ecs::Store &store, ecs::Scheduler &scheduler) {
			scene::InstallServices(store);
			REQUIRE(script::ConfigurePortalTransfers(store, world == source ? 101 : 202));
			script::RegisterTeleportAdmission(scheduler);
		});
	}
	for (int tick = 0; tick < 3; ++tick)
		worlds.Tick(1.0f / 60);
	ecs::Entity oldPlayer;
	script::PortalTransferId id;
	worlds.Enter(source, [&](ecs::Store &store) {
		oldPlayer = scene::AddPlayer(store, "walker", false, 1);
		REQUIRE(scene::LoadCharacter(store, oldPlayer) != ecs::NULL_ENTITY);
		std::string failure;
		REQUIRE(script::BeginPortalTransfer(store, oldPlayer, "destination", {}, id, failure));
		REQUIRE(game::ApplyMoveInput(store, oldPlayer, {{1, 0, 0}, false}));
	});
	for (int tick = 0; tick < 8; ++tick)
		worlds.Tick(1.0f / 60);
	worlds.Enter(source, [&](ecs::Store &store) {
		REQUIRE_FALSE(store.Alive(oldPlayer));
		REQUIRE(game::ApplyMoveInput(store, oldPlayer, {{0, 0, -1}, true}));
	});
	for (int tick = 0; tick < 8; ++tick)
		worlds.Tick(1.0f / 60);
	ecs::Entity newHumanoid;
	worlds.Enter(destination, [&](ecs::Store &store) {
		const auto player = script::PortalTransferPlayer(store, id);
		REQUIRE(player != ecs::NULL_ENTITY);
		newHumanoid = store.Get<scene::Character>(scene::CharacterOf(store, player))->Humanoid;
		auto *humanoid = store.GetMutable<scene::Humanoid>(newHumanoid);
		REQUIRE(humanoid->MoveDirection == core::Vector3{0, 0, -1});
		REQUIRE(humanoid->JumpRequested);
		humanoid->JumpRequested = false;
		REQUIRE(game::ApplyMoveInput(store, player, {{-1, 0, 0}, false}));
	});
	worlds.Enter(source, [&](ecs::Store &store) { REQUIRE(game::ApplyMoveInput(store, oldPlayer, {})); });
	for (int tick = 0; tick < 8; ++tick)
		worlds.Tick(1.0f / 60);
	worlds.Enter(destination, [&](ecs::Store &store) {
		const auto *humanoid = store.Get<scene::Humanoid>(newHumanoid);
		REQUIRE(humanoid->MoveDirection == core::Vector3{-1, 0, 0});
		REQUIRE_FALSE(humanoid->JumpRequested);
	});
	worlds.Enter(source, [&](ecs::Store &store) {
		REQUIRE_FALSE(game::ApplyMoveInput(store, oldPlayer, {}));
	});
}

TEST_CASE(
	"native portal controls wait for their input time and acknowledge completed steps",
	"[game][play][portal-input-schedule]"
) {
	using namespace engine;
	const double worldRate = GENERATE(30.0, 60.0, 120.0);
	const bool localOverride = GENERATE(false, true);
	const float worldStep = static_cast<float>(1.0 / worldRate);
	const double inputStep = 1.0 / 120;
	scene::RegisterSceneClasses();
	world::Universe worlds;
	world::WorldSettings settings;
	settings.Name = core::Name("source");
	settings.TickRate = worldRate;
	const auto source = worlds.Create(settings);
	settings.Name = core::Name("destination");
	const auto destination = worlds.Create(settings);
	for (const auto world : {source, destination}) {
		worlds.Enter(world, [&](ecs::Store &store, ecs::Scheduler &scheduler) {
			scene::InstallServices(store);
			REQUIRE(script::ConfigurePortalTransfers(store, world == source ? 101 : 202));
			script::RegisterTeleportAdmission(scheduler);
		});
	}
	for (int tick = 0; tick < 3; ++tick)
		worlds.Tick(worldStep);
	ecs::Entity sourcePlayer, player;
	script::PortalTransferId id;
	worlds.Enter(source, [&](ecs::Store &store) {
		sourcePlayer = scene::AddPlayer(store, "timed", false, 1);
		REQUIRE(scene::LoadCharacter(store, sourcePlayer) != ecs::NULL_ENTITY);
		std::string error;
		REQUIRE(script::BeginPortalTransfer(store, sourcePlayer, "destination", {}, id, error));
		REQUIRE(game::ApplyMoveInput(store, sourcePlayer, {{1, 0, 0}, false, inputStep}, 100));
	});
	bool forwarded = false;
	for (int tick = 0; tick < 20 && !forwarded; ++tick) {
		worlds.Tick(worldStep);
		worlds.Enter(destination, [&](ecs::Store &store) {
			player = script::PortalTransferPlayer(store, id);
			const auto *rig = store.Get<scene::Character>(scene::CharacterOf(store, player));
			forwarded =
				rig && store.Get<scene::Humanoid>(rig->Humanoid)->MoveDirection == core::Vector3{1, 0, 0};
		});
	}
	REQUIRE(forwarded);
	worlds.Enter(destination, [&](ecs::Store &store) {
		const auto forwardedPose = game::CapturePlayerMotion(store, player, 99);
		REQUIRE(forwardedPose);
		CHECK(forwardedPose->Motion.InputTick == 100);
		CHECK(forwardedPose->Motion.SimulationSeconds == store.Time().Elapsed);
		REQUIRE(game::ApplyMoveInput(store, player, {{-1, 0, 0}, true, inputStep}, 107));
		REQUIRE(game::ApplyMoveInput(store, player, {{0, 0, 1}, false, inputStep}, 108));
		REQUIRE(game::ApplyMoveInput(store, player, {{-1, 0, 0}, false, inputStep}, 107));
		CHECK_FALSE(game::ApplyMoveInput(store, player, {{1, 0, 0}, false, inputStep * 2}, 109));
		CHECK_FALSE(
			game::ApplyMoveInput(
				store, player, {{1, 0, 0}, false, inputStep}, std::numeric_limits<uint64_t>::max()
			)
		);
		const auto sample = game::CapturePlayerMotion(store, player, 108);
		REQUIRE(sample);
		CHECK(sample->Motion.InputTick == 100);
		CHECK(sample->Motion.SimulationSeconds == store.Time().Elapsed);
	});
	core::ByteWriter saved;
	REQUIRE(worlds.Save(saved));
	world::Universe restored;
	core::ByteReader reader(saved.Bytes());
	REQUIRE(restored.Load(reader));
	for (const auto world : restored.Worlds())
		restored.Enter(world, [](ecs::Store &, ecs::Scheduler &scheduler) {
			script::RegisterTeleportAdmission(scheduler);
		});
	const int dueSteps = static_cast<int>(8 * worldRate / 120);
	for (int tick = 1; tick <= dueSteps; ++tick) {
		worlds.Tick(worldStep);
		restored.Tick(worldStep);
		worlds.Enter(destination, [&](ecs::Store &store) {
			const auto *rig = store.Get<scene::Character>(scene::CharacterOf(store, player));
			const auto *humanoid = store.Get<scene::Humanoid>(rig->Humanoid);
			const auto sample = game::CapturePlayerMotion(store, player, 108);
			REQUIRE(sample);
			if (tick == dueSteps) {
				CHECK(sample->Motion.InputTick == 108);
				CHECK(humanoid->MoveDirection == core::Vector3{0, 0, 1});
				CHECK(humanoid->JumpRequested);
			} else if (tick < dueSteps - 1 || worldRate < 120) {
				CHECK(sample->Motion.InputTick == 100);
				CHECK(sample->Motion.SimulationSeconds == store.Time().Elapsed);
				CHECK(humanoid->MoveDirection == core::Vector3{1, 0, 0});
			} else {
				CHECK(sample->Motion.InputTick == 107);
				CHECK(humanoid->MoveDirection == core::Vector3{-1, 0, 0});
				CHECK(humanoid->JumpRequested);
			}
		});
	}
	core::ByteWriter expected, actual;
	REQUIRE(worlds.Save(expected));
	REQUIRE(restored.Save(actual));
	CHECK(std::ranges::equal(expected.Bytes(), actual.Bytes()));
	worlds.Enter(destination, [&](ecs::Store &store) {
		const auto *rig = store.Get<scene::Character>(scene::CharacterOf(store, player));
		store.GetMutable<scene::Humanoid>(rig->Humanoid)->JumpRequested = false;
		if (worldRate == 120) {
			for (uint64_t input = 109; input <= 172; ++input)
				REQUIRE(game::ApplyMoveInput(store, player, {{1, 0, 0}, input == 109, inputStep}, input));
			CHECK_FALSE(game::ApplyMoveInput(store, player, {{1, 0, 0}, true, inputStep}, 173));
			CHECK(script::AppliedPortalPlayerInput(store, player) == 108);
		} else {
			REQUIRE(game::ApplyMoveInput(store, player, {{1, 0, 0}, true, inputStep}, 112));
		}
		if (localOverride) {
			REQUIRE(game::ApplyMoveInput(store, player, {{0, 0, -1}, false}));
			CHECK_FALSE(script::AppliedPortalPlayerInput(store, player));
		} else {
			REQUIRE(scene::LoadCharacter(store, player) != ecs::NULL_ENTITY);
			CHECK_FALSE(script::AppliedPortalPlayerInput(store, player));
			REQUIRE(game::ApplyMoveInput(store, player, {{0, 0, -1}, false, inputStep}, 200));
		}
	});
	for (int tick = 0; tick < 4; ++tick)
		worlds.Tick(worldStep);
	worlds.Enter(destination, [&](ecs::Store &store) {
		const auto *rig = store.Get<scene::Character>(scene::CharacterOf(store, player));
		const auto *humanoid = store.Get<scene::Humanoid>(rig->Humanoid);
		CHECK(humanoid->MoveDirection == core::Vector3{0, 0, -1});
		CHECK_FALSE(humanoid->JumpRequested);
	});
	script::PortalTransferId returning;
	worlds.Enter(destination, [&](ecs::Store &store) {
		std::string error;
		REQUIRE(script::BeginPortalTransfer(store, player, "source", {}, returning, error));
	});
	for (int tick = 0; tick < 8; ++tick)
		worlds.Tick(worldStep);
	worlds.Enter(source, [&](ecs::Store &store) {
		const auto returned = script::PortalTransferPlayer(store, returning);
		REQUIRE(returned != ecs::NULL_ENTITY);
		CHECK_FALSE(script::AppliedPortalPlayerInput(store, returned));
	});
}
