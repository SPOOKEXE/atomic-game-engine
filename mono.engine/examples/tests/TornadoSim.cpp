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
	CHECK(document.find("Instance.new(\"GpuParticleField\")") != std::string::npos);
	CHECK(document.find("visualField.Layers = 7") != std::string::npos);
	CHECK(document.find("TORNADO FIELD LAB") != std::string::npos);
	CHECK(document.find("Storm.Sample") != std::string::npos);
	CHECK(document.find("Storm.Visibility") != std::string::npos);
	CHECK(document.find("Storm.Damage") != std::string::npos);
	CHECK(document.find("StormInteraction") != std::string::npos);
	CHECK(document.find("WoodenSign") != std::string::npos);
	CHECK(document.find("SteelPanel") != std::string::npos);
	CHECK(document.find("WindBentTree") != std::string::npos);
	CHECK(document.find("StormExposedArea") != std::string::npos);
	CHECK(document.find("StormBreakForce") != std::string::npos);
	CHECK(document.find("StormMaterialStrength") != std::string::npos);
	CHECK(document.find("StormIntegrity") != std::string::npos);
	CHECK(document.find("StormDamageRate") != std::string::npos);
	CHECK(document.find("StormRestFrame") != std::string::npos);
	CHECK(document.find("retireWhenBroken") != std::string::npos);
	CHECK(document.find("Debris:AddItem") != std::string::npos);
	CHECK(document.find("<Item class=\"RemoteEvent\" name=\"TornadoControl\"") != std::string::npos);
	CHECK(document.find("controlEvent:FireServer(HttpService:JSONEncode(change))") != std::string::npos);
	CHECK(document.find("controlEvent.OnServerEvent:Connect(") != std::string::npos);
	CHECK(document.find("Controls.parameters") != std::string::npos);
	CHECK(document.find("request.kind == \"parameters\"") != std::string::npos);
	CHECK(document.find("request.kind == \"layers\"") != std::string::npos);
	CHECK(document.find("\"PAUSE\"") != std::string::npos);
	CHECK(document.find("\"RESET\"") != std::string::npos);
	CHECK(document.find("GpuParticleField") != std::string::npos);
	CHECK(document.find("50000000") != std::string::npos);
	CHECK(document.find("Farmhouse") != std::string::npos);
	CHECK(document.find("Barn") != std::string::npos);
	CHECK(document.find("Workshop") != std::string::npos);
	CHECK(document.find("Storm Structure Assemblies") != std::string::npos);
	CHECK(document.find("Timber Farmhouse") != std::string::npos);
	CHECK(document.find("Masonry Depot") != std::string::npos);
	CHECK(document.find("Steel Workshop") != std::string::npos);
	CHECK(document.find("material.glass") != std::string::npos);
	CHECK(document.find("CFrame.Angles(0, yaw, 0)") != std::string::npos);
	CHECK(document.find("row == 0") != std::string::npos);
	CHECK(document.find("stormJoint") != std::string::npos);
	CHECK(document.find("Tornado Storm Sky") != std::string::npos);
	CHECK(document.find("Tornado Anvil Cloud Deck") != std::string::npos);
	CHECK(document.find("Funnel Ground Spray") != std::string::npos);
	CHECK(document.find("Funnel Upper Condensation") != std::string::npos);
	CHECK(document.find("Funnel Anvil Feed") != std::string::npos);
	CHECK(document.find("Rain Curtain") != std::string::npos);
	CHECK(document.find("Debris Skirt") != std::string::npos);
	CHECK(document.find("Condensation Streamers") != std::string::npos);
	CHECK(document.find("Storm Prairie") != std::string::npos);
	CHECK(document.find("Prairie Grid NS") != std::string::npos);
	CHECK(document.find("Prairie Grid EW") != std::string::npos);
	CHECK(document.find("Tree Crown High") != std::string::npos);
	CHECK(document.find("if index ~= 57 then") != std::string::npos);
	CHECK(document.find("bindStormDamageScenery") != std::string::npos);
	CHECK(document.find("WindBentTree Crown Low") != std::string::npos);
	CHECK(document.find("Wood Sign Face") != std::string::npos);
	CHECK(document.find("Steel Panel Stripe") != std::string::npos);
	CHECK(document.find("scenery.Source.CFrame * scenery.LocalFrame") != std::string::npos);
	CHECK(document.find("Fence Rail") != std::string::npos);
	CHECK(document.find("Ground Debris Inflow") != std::string::npos);
	CHECK(document.find("Wall Cloud Near") != std::string::npos);
	CHECK(document.find("Wall Cloud East") != std::string::npos);
	CHECK(document.find("Anvil Back Shelf") != std::string::npos);
	CHECK(document.find("Tornado Wind Bed") != std::string::npos);
	CHECK(document.find("audio/tornado-wind.wav") != std::string::npos);
	CHECK(document.find("Tornado Low Circulation") != std::string::npos);
	CHECK(document.find("audio/tornado-circulation.wav") != std::string::npos);
	CHECK(document.find("Tornado Rain Sheet") != std::string::npos);
	CHECK(document.find("audio/tornado-rain.wav") != std::string::npos);
	CHECK(document.find("Tornado Debris Rattle") != std::string::npos);
	CHECK(document.find("audio/tornado-debris.wav") != std::string::npos);
	CHECK(document.find("Tornado Delayed Thunder") != std::string::npos);
	CHECK(document.find("audio/tornado-thunder.wav") != std::string::npos);
	CHECK(document.find("scheduleThunder") != std::string::npos);
	CHECK(document.find("local lightningSeed = 0x544F524E") != std::string::npos);
	CHECK(document.find("lightningSeed = (lightningSeed * 16807) % 2147483647") != std::string::npos);
	CHECK(document.find("parameters.Humidity * parameters.Energy") != std::string::npos);
	CHECK(document.find("/ math.max(0.25 + moisture, 0.25)") != std::string::npos);
	CHECK(document.find("Tornado Lightning Segment") != std::string::npos);
	CHECK(document.find("Tornado Lightning Light") != std::string::npos);
	CHECK(document.find("Tornado Lightning Sparks") != std::string::npos);
	CHECK(document.find("/ 343") != std::string::npos);
	CHECK(document.find("FIELD AND WEATHER") != std::string::npos);
	CHECK(document.find("CAM ORBIT") != std::string::npos);
	CHECK(document.find("SHAKE") != std::string::npos);
	CHECK(document.find("FieldVectorInset") != std::string::npos);
	CHECK(document.find("cameraTarget = Vector3.new(0, 170, 95)") != std::string::npos);
	CHECK(document.find("cameraHome = Vector3.new(-275, 135, -405)") != std::string::npos);
	CHECK(document.find("Instance.new(\"ScrollingFrame\")") != std::string::npos);
	CHECK(document.find("panel.CanvasSize = UDim2.new(0, 352, 0, 704)") != std::string::npos);
	CHECK(document.find("panel.CanvasPosition = Vector2.new(0, 0)") != std::string::npos);
	CHECK(document.find("panel.ScrollBarThickness = 8") != std::string::npos);

	const std::filesystem::path audio =
		world.parent_path().parent_path().parent_path() / "audio" / "tornado-wind.wav";
	CHECK(std::filesystem::is_regular_file(audio));
	CHECK(std::filesystem::file_size(audio) > 1024U);
	for (const char *name :
		 {"tornado-circulation.wav", "tornado-rain.wav", "tornado-debris.wav", "tornado-thunder.wav"}) {
		const std::filesystem::path layer = audio.parent_path() / name;
		CHECK(std::filesystem::is_regular_file(layer));
		CHECK(std::filesystem::file_size(layer) > 1024U);
	}
}
