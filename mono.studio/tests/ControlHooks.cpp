#include <engine/control/Features.hpp>
#include <engine/control/HookRegistry.hpp>
#include <engine/control/Surface.hpp>
#include <engine/control/features/DataScene.hpp>
#include <engine/control/features/Universe.hpp>
#include <engine/testing/Suite.hpp>
#include <engine/world/Universe.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>

TEST_SUITE_ID("studio.controlhooks")

TEST_CASE("Studio product hook owns its engine info row", "[studio][control]") {
	engine::world::Universe worlds;
	engine::control::Surface surface("studio", "studio hook test");
	surface.Enable(
		std::array{
			engine::control::features::Universe(worlds, true, false),
			engine::control::features::Diagnostics(false)
		}
	);

	std::string failure;
	auto lease = surface.ActivateHook(
		{
			.Id = "studio.product",
			.Revision = "v1",
			.Purpose = "Studio product controls.",
			.Dependencies = {},
			.Limits = {},
		},
		[&surface](engine::control::HookRegistration &) {
			surface.Add(
				engine::control::Tool{
					"engine_info",
					"Studio control state.",
					[] { return nlohmann::json{{"type", "object"}}; },
					[](const nlohmann::json &, std::string &) { return nlohmann::json{{"studio", true}}; },
				}
			);
			surface.Add(
				engine::control::Tool{
					"log_tail",
					"Studio output-panel lines.",
					[] { return nlohmann::json{{"type", "object"}}; },
					[](const nlohmann::json &, std::string &) { return nlohmann::json{{"studio", true}}; },
				}
			);
		},
		failure
	);

	REQUIRE(failure.empty());
	REQUIRE(lease.IsValid());
	const auto info =
		std::find_if(surface.Registered().begin(), surface.Registered().end(), [](const auto &tool) {
			return tool.Name == "engine_info";
		});
	REQUIRE(info != surface.Registered().end());
	CHECK(info->Call(nlohmann::json::object(), failure) == nlohmann::json{{"studio", true}});
	CHECK(failure.empty());
	CHECK(std::any_of(surface.Registered().begin(), surface.Registered().end(), [](const auto &tool) {
		return tool.Name == "log_tail";
	}));

	lease.Close();
	CHECK(std::none_of(surface.Registered().begin(), surface.Registered().end(), [](const auto &tool) {
		return tool.Name == "engine_info";
	}));
	CHECK(std::none_of(surface.Registered().begin(), surface.Registered().end(), [](const auto &tool) {
		return tool.Name == "log_tail";
	}));
}

TEST_CASE("Studio scene-rendering hook owns its camera calibration row", "[studio][control]") {
	engine::world::Universe worlds;
	engine::control::Surface surface("studio", "studio hook test");

	std::string failure;
	auto lease = surface.ActivateHook(
		{
			.Id = "studio.scene-rendering",
			.Revision = "v1",
			.Purpose = "Studio rendered-scene reads.",
			.Dependencies = {},
			.Limits = {},
		},
		[&worlds](engine::control::HookRegistration &registration) {
			registration.Add(engine::control::features::CameraRenderingDataTool(worlds));
		},
		failure
	);

	REQUIRE(failure.empty());
	REQUIRE(lease.IsValid());
	CHECK(std::any_of(surface.Registered().begin(), surface.Registered().end(), [](const auto &tool) {
		return tool.Name == "get_camera_rendering_data";
	}));

	lease.Close();
	CHECK(std::none_of(surface.Registered().begin(), surface.Registered().end(), [](const auto &tool) {
		return tool.Name == "get_camera_rendering_data";
	}));
}
