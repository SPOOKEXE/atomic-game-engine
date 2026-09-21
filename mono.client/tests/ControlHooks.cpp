#include "ControlHooks.hpp"

#include <engine/control/Surface.hpp>
#include <engine/control/features/DataScene.hpp>
#include <engine/testing/Suite.hpp>
#include <engine/world/DataFactory.hpp>
#include <engine/world/Universe.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <client/DataAudioObservation.hpp>

TEST_SUITE_ID("client.controlhooks")

TEST_CASE("client visibility observation hook owns its row for the lease lifetime", "[client][control]") {
	engine::control::Surface surface("client", "client hook test");
	std::string failure;
	auto lease = client::ActivateVisibilityObservationHook(
		surface.Hooks(),
		[] {
			return engine::control::features::VisibilitySnapshotReply{
				.Frame = 13,
				.World = {},
				.Valid = true,
				.Observations = {},
			};
		},
		failure
	);

	REQUIRE(failure.empty());
	REQUIRE(lease.IsValid());
	const auto visible =
		std::find_if(surface.Registered().begin(), surface.Registered().end(), [](const auto &tool) {
			return tool.Name == "visibility_observations";
		});
	REQUIRE(visible != surface.Registered().end());
	const auto reply = visible->Call(nlohmann::json::object(), failure);
	REQUIRE(failure.empty());
	CHECK(reply["frame"] == 13);
	CHECK(reply["valid"] == true);

	lease.Close();
	CHECK(surface.Hooks().Active().empty());
	CHECK(std::none_of(surface.Registered().begin(), surface.Registered().end(), [](const auto &tool) {
		return tool.Name == "visibility_observations";
	}));
}

TEST_CASE("client visibility observation hook refuses a missing renderer provider", "[client][control]") {
	engine::control::Surface surface("client", "client hook test");
	std::string failure;
	const auto lease = client::ActivateVisibilityObservationHook(surface.Hooks(), {}, failure);

	CHECK_FALSE(lease.IsValid());
	CHECK(failure == "visibility snapshot provider is required");
	CHECK(surface.Registered().empty());
}

TEST_CASE("client temporal and rig hooks own their session-fenced readers", "[client][control]") {
	engine::world::Universe worlds;
	engine::world::DataFactorySession session(worlds);
	engine::control::Surface surface("client", "client hook test");
	std::string failure;
	auto temporal = client::ActivateTemporalSampleHook(surface.Hooks(), worlds, session, failure);
	REQUIRE(failure.empty());
	REQUIRE(temporal.IsValid());
	auto rig = client::ActivateRigExportHook(surface.Hooks(), worlds, session, failure);
	REQUIRE(failure.empty());
	REQUIRE(rig.IsValid());

	const auto temporalTool =
		std::find_if(surface.Registered().begin(), surface.Registered().end(), [](const auto &tool) {
			return tool.Name == "get_temporal_sample";
		});
	REQUIRE(temporalTool != surface.Registered().end());
	CHECK(temporalTool->Call(nlohmann::json::object(), failure).is_null());
	CHECK_FALSE(failure.empty());
	const auto rigTool =
		std::find_if(surface.Registered().begin(), surface.Registered().end(), [](const auto &tool) {
			return tool.Name == "get_rig_export";
		});
	REQUIRE(rigTool != surface.Registered().end());
	CHECK(rigTool->Call(nlohmann::json::object(), failure).is_null());
	CHECK_FALSE(failure.empty());

	temporal.Close();
	rig.Close();
	CHECK(surface.Hooks().Active().empty());
	CHECK(std::none_of(surface.Registered().begin(), surface.Registered().end(), [](const auto &tool) {
		return tool.Name == "get_temporal_sample" || tool.Name == "get_rig_export";
	}));
}

TEST_CASE("client audio observation hook publishes and removes its coordinated rows", "[client][control]") {
	engine::world::Universe worlds;
	engine::world::DataFactorySession session(worlds);
	engine::control::Surface surface("client", "client hook test");
	std::string failure;
	auto bridge = std::make_shared<client::DataAudioObservationHost>();
	auto lease = client::ActivateDataAudioObservationHook(surface.Hooks(), worlds, bridge, session, failure);

	REQUIRE(failure.empty());
	REQUIRE(lease.IsValid());
	CHECK(surface.Readable().size() == 1);
	CHECK(surface.Registered().size() == 2);
	const auto observation =
		std::find_if(surface.Registered().begin(), surface.Registered().end(), [](const auto &tool) {
			return tool.Name == "get_audio_observation";
		});
	REQUIRE(observation != surface.Registered().end());
	CHECK(observation->Call(nlohmann::json::object(), failure).is_null());
	CHECK_FALSE(failure.empty());

	lease.Close();
	CHECK(surface.Hooks().Active().empty());
	CHECK(surface.Readable().empty());
	CHECK(surface.Registered().empty());
}

TEST_CASE("client audio observation hook refuses a missing bridge", "[client][control]") {
	engine::world::Universe worlds;
	engine::world::DataFactorySession session(worlds);
	engine::control::Surface surface("client", "client hook test");
	std::string failure;
	const auto lease =
		client::ActivateDataAudioObservationHook(surface.Hooks(), worlds, {}, session, failure);

	CHECK_FALSE(lease.IsValid());
	CHECK(failure == "audio observation bridge is required");
	CHECK(surface.Readable().empty());
	CHECK(surface.Registered().empty());
}

TEST_CASE("client scene-rendering hook owns camera calibration row", "[client][control]") {
	engine::world::Universe worlds;
	engine::world::DataFactorySession session(worlds);
	engine::control::Surface surface("client", "client hook test");
	std::string failure;
	auto lifecycle = surface.ActivateHook(
		{.Id = "client.data-factory-lifecycle",
		 .Revision = "v1",
		 .Purpose = "test lifecycle",
		 .Dependencies = {},
		 .Limits = {}},
		[&surface, &session](engine::control::HookRegistration &) { surface.AddDataFactoryTools(session); },
		failure
	);
	REQUIRE(lifecycle.IsValid());

	auto sceneRendering = surface.ActivateHook(
		{.Id = "client.scene-rendering",
		 .Revision = "v1",
		 .Purpose = "test camera calibration",
		 .Dependencies = {"client.data-factory-lifecycle"},
		 .Limits = {}},
		[&worlds, &session](engine::control::HookRegistration &registration) {
			registration.Add(engine::control::features::CameraRenderingDataTool(worlds, &session));
		},
		failure
	);
	REQUIRE(failure.empty());
	REQUIRE(sceneRendering.IsValid());
	CHECK(std::any_of(surface.Registered().begin(), surface.Registered().end(), [](const auto &tool) {
		return tool.Name == "get_camera_rendering_data";
	}));

	sceneRendering.Close();
	CHECK(std::none_of(surface.Registered().begin(), surface.Registered().end(), [](const auto &tool) {
		return tool.Name == "get_camera_rendering_data";
	}));
	lifecycle.Close();
}

TEST_CASE("scoped client factory hooks retain lifecycle rows until dependants close", "[client][control]") {
	engine::world::Universe worlds;
	engine::world::DataFactorySession session(worlds);
	engine::control::Surface surface("client", "client hook test");
	std::string failure;
	auto lifecycle = surface.ActivateHook(
		{.Id = "client.lifecycle", .Revision = "v1", .Purpose = "test lifecycle", .Dependencies = {}, .Limits = {}},
		[&surface, &session](engine::control::HookRegistration &) { surface.AddDataFactoryTools(session); },
		failure
	);
	REQUIRE(lifecycle.IsValid());
	auto scene = surface.ActivateHook(
		{.Id = "client.scene",
		 .Revision = "v1",
		 .Purpose = "test scene",
		 .Dependencies = {"client.lifecycle"},
		 .Limits = {}},
		[&surface](engine::control::HookRegistration &) {
			surface.Add(
				engine::control::Tool{
					"scene_probe", "test", nullptr, [](const nlohmann::json &, std::string &) {
						return nlohmann::json{};
					}
				}
			);
		},
		failure
	);
	REQUIRE(scene.IsValid());
	lifecycle.Close();
	CHECK(surface.Hooks().Active().size() == 2);
	scene.Close();
	CHECK(surface.Hooks().Active().empty());
	CHECK(surface.Registered().empty());
}
