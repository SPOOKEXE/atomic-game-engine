// The authored TornadoSim world is a self-contained game asset. These checks
// keep its embedded server, client and shared control paths together.

#include <engine/core/Paths.hpp>
#include <engine/examples/DemosLoader.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

TEST_SUITE_ID("engine.examples.tornado-sim")
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

TEST_CASE("the TornadoSim world carries its storm service and in-game controls", "[examples][tornado]") {
	const StagedAssets assets;
	const engine::examples::DemosLoader demos;
	const std::filesystem::path world = demos.Resolve(engine::examples::DemoKind::World, "TornadoSim.aworld");
	REQUIRE_FALSE(world.empty());

	std::ifstream input(world, std::ios::binary);
	REQUIRE(input);
	const std::string document(std::istreambuf_iterator<char>(input), {});

	CHECK(document.find("scripts/server/TornadoServer.server.luau") != std::string::npos);
	CHECK(document.find("scripts/client/TornadoClient.client.luau") != std::string::npos);
	CHECK(document.find("scripts/shared/TornadoSim/StormControls.luau") != std::string::npos);
	CHECK(document.find("Storm.Preset(\"EF3\")") != std::string::npos);
	CHECK(document.find("EF0-EF5  Q0-Q5  CUSTOM") != std::string::npos);
	CHECK(document.find("Storm.Sample") != std::string::npos);
	CHECK(document.find("Storm.Visibility") != std::string::npos);
	CHECK(document.find("Storm.Damage") != std::string::npos);
	CHECK(document.find("MessagingService:PublishAsync(\"tornado.sim.control\"") != std::string::npos);
	CHECK(document.find("MessagingService:SubscribeAsync(\"tornado.sim.control\"") != std::string::npos);
	CHECK(document.find("\"PAUSE\"") != std::string::npos);
	CHECK(document.find("\"RESET\"") != std::string::npos);
	CHECK(document.find("Farmhouse") != std::string::npos);
	CHECK(document.find("Barn") != std::string::npos);
	CHECK(document.find("Workshop") != std::string::npos);
}
