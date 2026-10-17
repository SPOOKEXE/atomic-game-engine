// The authored Bladeborne world keeps an automatic camera script alongside its
// HUD so a client never falls back to an unowned, detached view.

#include <engine/core/Paths.hpp>
#include <engine/examples/DemosLoader.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

TEST_SUITE_ID("engine.examples.bladeborne-demo")
TEST_DEPENDS("engine.examples.demos-loader")

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

TEST_CASE("the Bladeborne world installs an automatic local camera", "[examples][bladeborne]") {
	const StagedAssets assets;

	const engine::examples::DemosLoader demos;
	const std::filesystem::path world =
		demos.Resolve(engine::examples::DemoKind::World, "BladeborneDemo.aworld");
	REQUIRE_FALSE(world.empty());

	std::ifstream input(world, std::ios::binary);
	REQUIRE(input);
	const std::string document(std::istreambuf_iterator<char>(input), {});

	CHECK(
		document.find(R"(<Source path="scripts/client/BladeborneCamera.client.luau">)") != std::string::npos
	);
	CHECK(document.find(R"(<Item class="LocalScript" name="BladeborneCamera")") != std::string::npos);
	CHECK(document.find("camera.CameraSubjectAutomatic = true") != std::string::npos);
	CHECK(document.find("Workspace.CurrentCamera = camera") != std::string::npos);
}
