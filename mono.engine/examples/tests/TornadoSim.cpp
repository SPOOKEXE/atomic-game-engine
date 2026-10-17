// The authored TornadoSim world is a self-contained game asset. These checks
// keep its embedded server, client and shared control paths together.

#include <engine/core/Paths.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/examples/DemosLoader.hpp>
#include <engine/scene/Part.hpp>
#include <engine/scene/Registration.hpp>
#include <engine/scene/Services.hpp>
#include <engine/scripthost/Runtime.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <regex>
#include <string>
#include <string_view>

TEST_SUITE_ID("engine.examples.tornado-sim")
TEST_DEPENDS("engine.examples.demos-loader")
TEST_DEPENDS("engine.scriptluau.runtime")

namespace {
	using Vector3Components = std::array<float, 3>;

	std::optional<Vector3Components> LocalVector3(const std::string &document, const std::string &name) {
		const std::regex assignment(
			"local\\s+" + name +
			"\\s*=\\s*Vector3\\.new\\(\\s*(-?[0-9]+(?:\\.[0-9]+)?)\\s*,\\s*"
			"(-?[0-9]+(?:\\.[0-9]+)?)\\s*,\\s*(-?[0-9]+(?:\\.[0-9]+)?)\\s*\\)"
		);
		std::smatch match;
		if (!std::regex_search(document, match, assignment)) {
			return std::nullopt;
		}
		return Vector3Components{std::stof(match[1]), std::stof(match[2]), std::stof(match[3])};
	}

	std::optional<std::array<int, 2>> PanelCanvasPixels(const std::string &document) {
		const std::regex assignment(
			R"(panel\.CanvasSize\s*=\s*UDim2\.new\(\s*0\s*,\s*([0-9]+)\s*,\s*0\s*,\s*([0-9]+)\s*\))"
		);
		std::smatch match;
		if (!std::regex_search(document, match, assignment)) {
			return std::nullopt;
		}
		return std::array<int, 2>{std::stoi(match[1]), std::stoi(match[2])};
	}

	std::optional<std::string> Source(const std::string &document, std::string_view path) {
		const std::string marker = "<Source path=\"" + std::string(path) + "\"><![CDATA[";
		const size_t begin = document.find(marker);
		if (begin == std::string::npos) return std::nullopt;
		const size_t sourceBegin = begin + marker.size();
		const size_t end = document.find("]]></Source>", sourceBegin);
		if (end == std::string::npos) return std::nullopt;
		return document.substr(sourceBegin, end - sourceBegin);
	}

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
	CHECK(document.find("scripts/shared/TornadoSim/StormField.luau") != std::string::npos);
	CHECK(document.find("Field.cloudDensity") != std::string::npos);
	CHECK(document.find("Storm.Sample") == std::string::npos);
	CHECK(document.find("Storm.Preset") == std::string::npos);
	CHECK(document.find("Field.preset(state, request.name)") != std::string::npos);
	CHECK(
		document.find(
			"function Field.sample(parameters: Parameters, center: Vector3, position: Vector3, timeSeconds: "
			"number)"
		) != std::string::npos
	);
	CHECK(document.find("SetSpawnSamples") != std::string::npos);
	CHECK(document.find("SetLayer(index, colour, alpha, size, acceleration)") != std::string::npos);
	CHECK(
		document.find("World:SetComponentTags(\"Tornado.State\", { \"replicated\" })") != std::string::npos
	);
	CHECK(document.find("TORNADO FIELD LAB") != std::string::npos);
	CHECK(document.find("Field.sample(snapshot.Parameters, snapshot.Position") != std::string::npos);
	CHECK(document.find("Field.visibility(snapshot") != std::string::npos);
	CHECK(document.find("Field.damage(snapshot") != std::string::npos);
	CHECK(document.find("button(\"FUNNEL\"") != std::string::npos);
	CHECK(document.find("rainBank.Enabled = layerActive(activeLayers, 2)") != std::string::npos);
	CHECK(document.find("for _, volume in ipairs(funnelVolumes)") != std::string::npos);
	CHECK(document.find("for _, shelf in ipairs(cloudShelf)") != std::string::npos);
	CHECK(document.find("StormInteraction") != std::string::npos);
	CHECK(document.find("WoodenSign") != std::string::npos);
	CHECK(document.find("SteelPanel") != std::string::npos);
	CHECK(document.find("WindBentTree") != std::string::npos);
	CHECK(document.find("Tornado.Response") != std::string::npos);
	CHECK(document.find("Tornado.Link") != std::string::npos);
	CHECK(document.find("MaterialStrength = \"number\"") != std::string::npos);
	CHECK(document.find("Integrity = \"number\"") != std::string::npos);
	CHECK(document.find("DamageRate = \"number\"") != std::string::npos);
	CHECK(document.find("Tornado.Vegetation") != std::string::npos);
	CHECK(document.find("retireWhenBroken") != std::string::npos);
	CHECK(document.find("Debris:AddItem") != std::string::npos);
	CHECK(document.find("<Item class=\"RemoteEvent\" name=\"TornadoControl\"") != std::string::npos);
	CHECK(document.find("controlEvent:FireServer(HttpService:JSONEncode(change))") != std::string::npos);
	CHECK(document.find("controlEvent.OnServerEvent:Connect(") != std::string::npos);
	CHECK(document.find("Controls.parameters") != std::string::npos);
	CHECK(document.find("request.kind == \"parameters\"") != std::string::npos);
	CHECK(document.find("request.kind == \"layers\"") != std::string::npos);
	CHECK(document.find("request.kind == \"motion\"") != std::string::npos);
	CHECK(document.find("request.kind == \"rotation\"") != std::string::npos);
	CHECK(document.find("\"PAUSE\"") != std::string::npos);
	CHECK(document.find("\"RESET\"") != std::string::npos);
	CHECK(document.find("GpuParticleField") != std::string::npos);
	CHECK(document.find("524288") != std::string::npos);
	CHECK(document.find("2000000") != std::string::npos);
	CHECK(document.find("5000000") != std::string::npos);
	CHECK(document.find("10000000") != std::string::npos);
	CHECK(document.find("15000000") != std::string::npos);
	CHECK(document.find("20000000") != std::string::npos);
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
	CHECK(document.find("updateStormAudio(snapshot, observerPosition)") != std::string::npos);
	CHECK(document.find("sample.Influence") != std::string::npos);
	CHECK(document.find("condensationEmitters") != std::string::npos);
	CHECK(document.find("rainEmitters") != std::string::npos);
	CHECK(document.find("debrisEmitters") != std::string::npos);
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
	CHECK(document.find("WIND X-") != std::string::npos);
	CHECK(document.find("CCW") != std::string::npos);
	CHECK(document.find("RAIN OP-") != std::string::npos);
	CHECK(document.find("RAIN DEN-") != std::string::npos);
	CHECK(document.find("ENV") != std::string::npos);
	CHECK(document.find("LIGHT") != std::string::npos);
	CHECK(document.find("cameraYaw") != std::string::npos);
	CHECK(document.find("cameraPitch") != std::string::npos);
	CHECK(document.find("cameraZoom") != std::string::npos);
	CHECK(document.find("SHAKE") != std::string::npos);
	CHECK(document.find("FieldVectorInset") != std::string::npos);
	const std::optional<Vector3Components> cameraTarget = LocalVector3(document, "cameraTarget");
	const std::optional<Vector3Components> cameraHome = LocalVector3(document, "cameraHome");
	REQUIRE(cameraTarget.has_value());
	REQUIRE(cameraHome.has_value());
	const float targetFromStorm = std::hypot((*cameraTarget)[0], (*cameraTarget)[2] - 95.0f);
	const float eyeToTarget = std::sqrt(
		std::pow((*cameraTarget)[0] - (*cameraHome)[0], 2.0f) +
		std::pow((*cameraTarget)[1] - (*cameraHome)[1], 2.0f) +
		std::pow((*cameraTarget)[2] - (*cameraHome)[2], 2.0f)
	);
	CHECK(targetFromStorm < 300.0f);
	CHECK((*cameraTarget)[1] >= 40.0f);
	CHECK((*cameraTarget)[1] <= 100.0f);
	CHECK((*cameraHome)[0] < -80.0f);
	CHECK((*cameraHome)[2] < -50.0f);
	CHECK(2.0f * eyeToTarget * std::tan(68.0f * 0.5f * 0.0174532925f) >= 1.5f * 285.0f);
	CHECK(document.find("return CFrame.lookAt(eye, target)") != std::string::npos);
	CHECK(document.find("camera.CameraType = Enum.CameraType.Scriptable") != std::string::npos);
	CHECK(document.find("camera.CFrame = cameraFrame(cameraTarget)") != std::string::npos);
	CHECK(document.find("Instance.new(\"ScrollingFrame\")") != std::string::npos);
	const std::optional<std::array<int, 2>> panelCanvas = PanelCanvasPixels(document);
	REQUIRE(panelCanvas.has_value());
	CHECK((*panelCanvas)[0] == 352);
	CHECK((*panelCanvas)[1] > 884);
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

TEST_CASE("the authored Luau field preserves deterministic tornado fixtures", "[examples][tornado]") {
	const StagedAssets assets;
	const engine::examples::DemosLoader demos;
	const std::filesystem::path world = demos.Resolve(engine::examples::DemoKind::World, "TornadoSim.aworld");
	REQUIRE_FALSE(world.empty());
	std::ifstream input(world, std::ios::binary);
	REQUIRE(input);
	const std::string document(std::istreambuf_iterator<char>(input), {});
	const auto extracted = Source(document, "scripts/shared/TornadoSim/StormField.luau");
	REQUIRE(extracted.has_value());

	std::string source = *extracted;
	const size_t returnField = source.rfind("return Field");
	REQUIRE(returnField != std::string::npos);
	source.insert(returnField, R"TORNADO(
local defaults = Field.defaultParameters()
local fixtures = {
	{ Vector3.new(34, 5, 0), Vector3.new(-12.424633, 19.002569, 64.446610), 0.533769 },
	{ Vector3.new(90, 20, -30), Vector3.new(7.729156, -9.602772, 43.107094), 0.202896 },
	{ Vector3.new(0, 100, 0), Vector3.new(4.602900, -4.237411, -0.321400), 0.060480 },
}
for _, fixture in ipairs(fixtures) do
	local sample = Field.sample(defaults, Vector3.zero, fixture[1], 12.5)
	assert((sample.Velocity - fixture[2]).Magnitude < 0.00002)
	assert(math.abs(sample.DamagePotential - fixture[3]) < 0.00002)
end
local drag = Field.dragForce(Vector3.new(10, 0, 0), Vector3.zero, 2, 1, 1, 0.1, 10)
assert(
	math.abs(drag.X - 122.5) < 0.00001 and drag.Y == 0 and drag.Z == 0,
	string.format("dragForce returned (%.9g, %.9g, %.9g), expected (122.5, 0, 0)", drag.X, drag.Y, drag.Z)
)
local cappedDrag = Field.dragForce(Vector3.new(10, 0, 0), Vector3.zero, 2, 1, 1, 20, 10)
assert(math.abs(cappedDrag.X - 5) < 0.00001)
local integrity, broken = Field.linkDamage(1, 200, 100, 2, 1, 0.5, true)
assert(math.abs(integrity - 0.775) < 0.00001 and not broken)
local brokenIntegrity, broke = Field.linkDamage(1, 1000, 100, 2, 1, 0.5, true)
assert(brokenIntegrity == 0 and broke)
local bend, direction = Field.vegetationStep(0, 0, Vector3.new(36, 0, 0), 0.5, 5, 72, 0.02)
assert(math.abs(bend - 0.025) < 0.00001 and math.abs(direction) < 0.00001)
local cloudPoint = Vector3.new(defaults.CoreRadius * 1.8, defaults.TopHeight * 0.24, 0)
local cloudDensity = Field.cloudDensity(defaults, cloudPoint, 12.5)
assert(cloudDensity > 0 and cloudDensity <= 1)
assert(Field.cloudDensity(defaults, Vector3.new(0, defaults.TopHeight * 1.2, 0), 12.5) == 0)
local lifecycle = { { 0, 0.12 }, { 8.1, 0.435 }, { 16.2, 0.75 }, { 45, 0.912868857 }, { 61.2, 0.75 }, { 90, 0.12 } }
for _, fixture in ipairs(lifecycle) do
	assert(math.abs(Field.lifecycleEnergy(fixture[1]) - fixture[2]) < 0.000002)
end
local moving = Field.new(defaults, Vector3.zero)
Field.configure(moving, { LifecycleEnabled = true })
Field.advance(moving, 18)
assert((moving.Position - Vector3.new(72, 0, 27)).Magnitude < 0.00001)
assert(math.abs(moving.Parameters.Energy - Field.lifecycleEnergy(18)) < 0.000002)
assert(Field.preset(moving, "EF4") and math.abs(moving.Parameters.CoreRadius - 39) < 0.00001)
assert(not Field.preset(moving, "EF6"))
local near = Field.visibility(Field.new({ Energy = 1, CoreRadius = 34, InfluenceRadius = 260,
	PeakTangentialSpeed = 150, PeakInflowSpeed = 34, PeakUpdraftSpeed = 58, PeakDowndraftSpeed = 32,
	SurfaceOutflowSpeed = 28, UpperWind = Vector3.zero, PressureDrop = 120, Humidity = 1, RainRate = 1,
	Turbulence = 13, TranslationVelocity = Vector3.zero, GroundFriction = 0.28, DebrisDensity = 0.65,
	VortexTightness = 2.25, TopHeight = 360, CounterClockwise = true }, Vector3.zero), Vector3.new(34, 20, 0), 300)
assert(near.EffectiveDistance > 0 and near.EffectiveDistance < 300)
)TORNADO");

	engine::ecs::Store store("tornado_luau_field");
	engine::scene::EnsureClassTree();
	engine::scene::InstallServices(store);
	engine::script::RuntimeLimits limits;
	limits.Role = engine::script::HostRole::OfClient();
	const auto runtime = engine::script::MakeRuntime(store, engine::script::Language::Luau, limits);
	REQUIRE(runtime != nullptr);
	INFO(source);
	const bool ran = runtime->Run(source.c_str());
	INFO(runtime->LastError());
	CHECK(ran);
}
