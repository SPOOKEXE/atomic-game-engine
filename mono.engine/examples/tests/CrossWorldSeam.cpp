#include <engine/core/Paths.hpp>
#include <engine/examples/Scene.hpp>
#include <engine/scene/SurfaceCameras.hpp>
#include <engine/testing/Suite.hpp>
#include <engine/world/Universe.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <filesystem>
#include <string>
#include <vector>

TEST_SUITE_ID("engine.examples.crossworldseam")
TEST_DEPENDS("engine.examples.scene")

namespace {
	struct StagedAssets {
		std::filesystem::path Previous = engine::core::Paths::Assets();
		StagedAssets() {
			engine::core::Paths::SetAssetsOverride(engine::core::Paths::Base().parent_path() / "assets");
		}
		~StagedAssets() {
			engine::core::Paths::SetAssetsOverride(Previous);
		}
	};
}

TEST_CASE("cross-world demo joins the real mouths from either camera side", "[examples][cross-world-seam]") {
	using namespace engine;
	const StagedAssets assets;
	world::Universe worlds;
	const std::array names{core::Name("client.world"), core::Name("client.world.1")};
	std::array<scene::PortalSeam, 2> seams;
	for (size_t index = 0; index < names.size(); ++index) {
		const auto id = worlds.Create({.Name = names[index]});
		REQUIRE(id.IsValid());
		worlds.Enter(id, [&](ecs::Store &store, ecs::Scheduler &systems) {
			std::string failure;
			const bool loaded =
				examples::LoadScene(store, systems, examples::ExamplePath("CrossWorldSeam.luau"), failure);
			INFO(failure);
			REQUIRE(loaded);
			std::vector<scene::PortalSeam> authored;
			REQUIRE(scene::GatherPortalSeams(store, authored) == 1);
			seams[index] = authored.front();
		});
	}

	for (size_t index = 0; index < seams.size(); ++index) {
		CAPTURE(index);
		const auto &source = seams[index];
		const auto &destination = seams[1 - index];
		REQUIRE(source.Crosses);
		REQUIRE(source.Bidirectional);
		CHECK(source.DestinationWorld == names[1 - index]);
		const auto through = scene::SeamMapping(source);
		CHECK((through.Point(source.Centre) - destination.Centre).Magnitude() < .001f);
		for (const auto axis : {core::Vector3::XAxis, core::Vector3::YAxis, core::Vector3::ZAxis}) {
			CHECK((through.Rotate(axis) - axis).Magnitude() < .001f);
		}
		for (const float side : {-1.0f, 1.0f}) {
			CAPTURE(side);
			const auto eye = core::CFrame::LookAt({1.5f, 5, side * 16}, source.Centre);
			const auto placed = through.Place(eye);
			CHECK((placed.Position - eye.Position).Magnitude() < .001f);
			CHECK((placed.LookVector() - eye.LookVector()).Magnitude() < .001f);
			const auto returned = scene::SeamMapping(destination).Place(placed);
			CHECK((returned.Position - eye.Position).Magnitude() < .001f);
			CHECK((returned.LookVector() - eye.LookVector()).Magnitude() < .001f);
			CHECK((returned.UpVector() - eye.UpVector()).Magnitude() < .001f);
		}
	}
}
