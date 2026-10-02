#include <engine/core/Paths.hpp>
#include <engine/game/Game.hpp>
#include <engine/imagegraph/SourceSdf.hpp>
#include <engine/scene/Registration.hpp>
#include <engine/script/Instances.hpp>
#include <engine/script/SourceCache.hpp>
#include <engine/testing/Suite.hpp>
#include <engine/world/Universe.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <client/ImageGraphRuntime.hpp>
#include <filesystem>
#include <fstream>
#include <iterator>

TEST_SUITE_ID("client.composer-scenes")
TEST_DEPENDS("client.imagegraphruntime")
TEST_DEPENDS("engine.game.roundtrip")
namespace {
	std::filesystem::path Assets() {
		return engine::core::Paths::Base().parent_path() / "assets";
	}
	client::ImageGraphFrameResult
	Frame(std::string_view graph, std::string_view output, uint64_t tick = 0, uint64_t seed = 7291) {
		auto result = client::LoadImageGraphFrame(
			Assets(), engine::core::Name(graph), engine::core::Name(output), tick, seed
		);
		INFO(result.Diagnostic.Message);
		REQUIRE(result.Status == engine::imagegraph::Status::Ok);
		return result;
	}
}
TEST_CASE(
	"Packaged Composer worlds load their embedded server and camera programs", "[client][composer-scenes]"
) {
	const std::string scene = GENERATE(
		"Composer-Particle-Flipbook",
		"Composer-Material-Maps",
		"Composer-Gui-And-Shader",
		"Composer-Skybox",
		"Composer-Feedback-And-Fluid",
		"Composer-3D-Reference",
		"Composer-Reference-Studies",
		"Composer-Verlet-Braced-Disk"
	);
	std::ifstream file(Assets() / "examples/worlds" / (scene + ".aworld"));
	REQUIRE(file.good());
	const std::string text(std::istreambuf_iterator<char>(file), {});
	engine::scene::RegisterSceneClasses();
	engine::script::ScriptClass();
	engine::script::LocalScriptClass();
	engine::world::Universe universe;
	std::string error;
	const auto world = engine::game::ReadWorldDocument(universe, text, {}, error);
	INFO(error);
	REQUIRE(world.IsValid());
	universe.Enter(world, [&](engine::ecs::Store &store) {
		const auto *cache = store.Resource<engine::script::SourceCache>();
		REQUIRE(cache);
		CHECK(cache->Count() == 2);
		const auto *server = cache->Find(engine::core::Name("scripts/server/" + scene + ".server.luau"));
		const auto *camera = cache->Find(engine::core::Name("scripts/client/" + scene + ".client.luau"));
		REQUIRE(server);
		REQUIRE(camera);
		CHECK(server->find("ImageGraphOutput") != std::string::npos);
		CHECK(camera->find("workspace.CurrentCamera = camera") != std::string::npos);
	});
}
TEST_CASE(
	"Living particle sheets preserve four authored cells and exact playback timing",
	"[client][composer-scenes]"
) {
	for (const std::string_view output : {"fire", "spark"}) {
		const auto result = Frame("Composer-Particle-Flipbook", output);
		CHECK(result.FlipbookSide == 2);
		REQUIRE(result.FrameDurations.size() == 4);
		CHECK(result.Image.Width == 128);
		CHECK(result.Image.Height == 128);
		for (const float duration : result.FrameDurations)
			CHECK(duration == Catch::Approx(1.f / 12));
		engine::imagegraph::SurfacePixel first{}, last{};
		REQUIRE(LoadSurfacePixel(result.Image, 32, 32, first));
		REQUIRE(LoadSurfacePixel(result.Image, 96, 96, last));
		CHECK(first[3] > 0);
		CHECK(last[3] > 0);
		if (output == "fire") CHECK(first[1] < last[1]);
	}
}
TEST_CASE(
	"Living material maps update derived normals and independent packed channels", "[client][composer-scenes]"
) {
	const auto colour0 = Frame("Composer-Material-Maps", "colour"),
			   colour60 = Frame("Composer-Material-Maps", "colour", 60);
	const auto normal0 = Frame("Composer-Material-Maps", "normal"),
			   normal60 = Frame("Composer-Material-Maps", "normal", 60);
	CHECK(colour0.Image.Hash != colour60.Image.Hash);
	CHECK(normal0.Image.Hash != normal60.Image.Hash);
	engine::imagegraph::SurfacePixel a{}, b{};
	const auto pbr0 = Frame("Composer-Material-Maps", "pbr"),
			   pbr60 = Frame("Composer-Material-Maps", "pbr", 60);
	REQUIRE(LoadSurfacePixel(pbr0.Image, 0, 0, a));
	REQUIRE(LoadSurfacePixel(pbr60.Image, 0, 0, b));
	CHECK(a[0] == Catch::Approx(170. / 255));
	CHECK(a[1] == Catch::Approx(230. / 255));
	CHECK(a[2] == Catch::Approx(15. / 255));
	CHECK(b[0] < a[0]);
	CHECK(b[1] < a[1]);
	CHECK(b[2] > a[2]);
}
TEST_CASE(
	"Living skybox authors one nonsquare generation and recovers all six faces", "[client][composer-scenes]"
) {
	for (const std::string_view face : {"front", "back", "left", "right", "up", "down"}) {
		const auto first = Frame("Composer-Skybox", face), invalid = Frame("Composer-Skybox", face, 120),
				   recovered = Frame("Composer-Skybox", face, 180);
		CHECK(first.Image.Width == first.Image.Height);
		CHECK(recovered.Image.Width == recovered.Image.Height);
		if (face == "up")
			CHECK(invalid.Image.Width != invalid.Image.Height);
		else
			CHECK(invalid.Image.Width == invalid.Image.Height);
		CHECK(first.Image.Hash == recovered.Image.Hash);
	}
}
TEST_CASE(
	"Living GUI outputs and feedback seeks change actual pixels deterministically",
	"[client][composer-scenes]"
) {
	for (const std::string_view output : {"mask", "height", "colour"}) {
		const auto first = Frame("Composer-Gui-And-Shader", output),
				   expanded = Frame("Composer-Gui-And-Shader", output, 60);
		CHECK(first.Image.Hash != expanded.Image.Hash);
	}
	const auto reset = Frame("Composer-Feedback-And-Fluid", "accumulated"),
			   stepped = Frame("Composer-Feedback-And-Fluid", "accumulated", 30),
			   again = Frame("Composer-Feedback-And-Fluid", "accumulated");
	CHECK(reset.Image.Hash != stepped.Image.Hash);
	CHECK(reset.Image == again.Image);
}

TEST_CASE(
	"Supplemental source studies produce deterministic nonconstant images", "[client][composer-scenes]"
) {
	const std::string study =
		GENERATE("Black-Hole", "Fire-Tornado", "Glass-Block-Refraction", "Ornate-Trim", "Spark-Bolt");
	const auto first = Frame("Composer-Study-" + study, "study");
	const auto later = Frame("Composer-Study-" + study, "study", 60);
	CHECK(first.Image.Width == 64);
	CHECK(first.Image.Height == 64);
	CHECK(first.Image == later.Image);
	engine::imagegraph::SurfacePixel origin{}, pixel{};
	REQUIRE(LoadSurfacePixel(first.Image, 0, 0, origin));
	bool different = false, readable = true;
	for (uint32_t y = 0; y < first.Image.Height && !different; ++y)
		for (uint32_t x = 0; x < first.Image.Width && !different; ++x) {
			readable = readable && LoadSurfacePixel(first.Image, x, y, pixel);
			different = pixel != origin;
		}
	CHECK(readable);
	CHECK(different);
}

TEST_CASE(
	"Living raymarch scene preserves animated subtraction through the native typed graph",
	"[client][composer-scenes]"
) {
	using namespace engine::imagegraph;
	std::ifstream file(Assets() / "imagegraphs/Composer-3D-Reference.graph");
	REQUIRE(file.good());
	const std::string text(std::istreambuf_iterator<char>(file), {});
	Document document;
	Plan plan;
	Diagnostic diagnostic;
	REQUIRE(Read(text, document, diagnostic) == Status::Ok);
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	EvaluatedValue initial, moved;
	REQUIRE(EvaluateValue(document, plan, "sdf-scene", {}, initial, diagnostic) == Status::Ok);
	EvaluationRequest request;
	request.Tick = 120;
	REQUIRE(EvaluateValue(document, plan, "sdf-scene", request, moved, diagnostic) == Status::Ok);
	const auto &a = std::get<SdfValue>(initial.Data), &b = std::get<SdfValue>(moved.Data);
	REQUIRE(a.Data);
	REQUIRE(b.Data);
	CHECK(a.Data->Shapes.size() == 2);
	REQUIRE(a.Data->Operations.size() == 3);
	CHECK(a.Data->Operations.back().Code == 102);
	double before = 0, after = 0;
	REQUIRE(SampleSourceSdf(a, {.65, 0, .4}, before, diagnostic) == Status::Ok);
	REQUIRE(SampleSourceSdf(b, {.65, 0, .4}, after, diagnostic) == Status::Ok);
	CHECK(before > 0);
	CHECK(after < 0);
}

TEST_CASE("Packaged source projects preserve authored official output routes", "[client][composer-scenes]") {
	using namespace engine::imagegraph;
	struct ProjectOutput {
		std::string_view Project, Output, Node, Port;
	};
	const ProjectOutput selected = GENERATE(
		ProjectOutput{
			"Black-Hole",
			"gSKBQW2707447cZLPHO4qsosZuR6bD0L",
			"gSJ9el29901564gZuoIW19YnO4fepb59",
			"surface_out"
		},
		ProjectOutput{
			"Fire-Tornado", "gSKBO82563685pOhaulkBGjkN0ZahGhW", "gSGCdv273281U95ck93aKFVYaOVxXR3b", "image"
		},
		ProjectOutput{
			"Glass-Block-Refraction",
			"gSKAiP180467pJ0YpXbwrplRMgj671sM",
			"gSDFAi769794aqfifGPMnFF0oXfSSMt2",
			"image"
		},
		ProjectOutput{
			"Ornate-Trim",
			"gSKBQu2731320Bc3gLD9ntjq2djDzQT3",
			"gSJD0d1443507UtlPsWv1EIVc79N0NPn",
			"surface_out"
		},
		ProjectOutput{
			"Spark-Bolt", "gSKA1Q1237222AW3R5nxuMfz0TUM1wYS", "gSI8Kg5208654sdkZlea7b6nCbzqSFtm", "image"
		}
	);
	std::ifstream file(
		Assets() / "imagegraphs" / ("Composer-Source-" + std::string(selected.Project) + ".graph")
	);
	REQUIRE(file.good());
	const std::string text(std::istreambuf_iterator<char>(file), {});
	Document document;
	Plan plan;
	Diagnostic diagnostic;
	REQUIRE(Read(text, document, diagnostic) == Status::Ok);
	REQUIRE(document.Outputs.size() == 1);
	CHECK(document.Outputs.front().Id == selected.Output);
	CHECK(document.Outputs.front().NodeId == selected.Node);
	CHECK(document.Outputs.front().Port == selected.Port);
	const auto status = Compile(document, plan, diagnostic);
	INFO(diagnostic.Message);
	REQUIRE(status == Status::Ok);
}

TEST_CASE(
	"Living FLIP particle images advance gravity and repeat fixed-seed reset seeks",
	"[client][composer-scenes]"
) {
	const auto first = Frame("Composer-Fluid-Particles", "fluid", 0, 12345);
	const auto stepped = Frame("Composer-Fluid-Particles", "fluid", 8, 12345);
	const auto reset = Frame("Composer-Fluid-Particles", "fluid", 0, 12345);
	const auto repeated = Frame("Composer-Fluid-Particles", "fluid", 8, 12345);
	CHECK(first.Animated);
	CHECK(first.Image.Width == 16);
	CHECK(first.Image.Height == 16);
	CHECK(first.Image.Hash != stepped.Image.Hash);
	CHECK(first.Image == reset.Image);
	CHECK(stepped.Image == repeated.Image);
	bool visible = false, readable = true;
	engine::imagegraph::SurfacePixel pixel{};
	for (uint32_t y = 0; y < first.Image.Height; ++y)
		for (uint32_t x = 0; x < first.Image.Width; ++x) {
			readable = readable && LoadSurfacePixel(first.Image, x, y, pixel);
			visible = visible || pixel[3] > 0;
		}
	CHECK(readable);
	CHECK(visible);
}

TEST_CASE(
	"Braced Verlet study falls onto its floor and reproduces reset and seek pixels",
	"[client][composer-scenes]"
) {
	const auto initial = Frame("Composer-Verlet-Braced-Disk", "body-image"),
			   falling = Frame("Composer-Verlet-Braced-Disk", "body-image", 4),
			   floor = Frame("Composer-Verlet-Braced-Disk", "body-image", 16),
			   reset = Frame("Composer-Verlet-Braced-Disk", "body-image"),
			   seek = Frame("Composer-Verlet-Braced-Disk", "body-image", 4);
	CHECK(initial.Image == reset.Image);
	CHECK(falling.Image == seek.Image);
	CHECK(initial.Image.Hash != falling.Image.Hash);
	CHECK(falling.Image.Hash != floor.Image.Hash);
	auto coverage = [](const engine::imagegraph::Image &image) {
		uint32_t count = 0;
		double centreY = 0;
		bool readable = true;
		for (uint32_t y = 0; y < image.Height; ++y)
			for (uint32_t x = 0; x < image.Width; ++x) {
				engine::imagegraph::SurfacePixel pixel{};
				readable = LoadSurfacePixel(image, x, y, pixel) && readable;
				if (pixel[3] > 0) {
					++count;
					centreY += y;
				}
			}
		REQUIRE(readable);
		REQUIRE(count > 0);
		return centreY / count;
	};
	CHECK(initial.Image.Width == 64);
	CHECK(initial.Image.Height == 64);
	CHECK(coverage(initial.Image) < 20);
	CHECK(coverage(falling.Image) > 30);
	CHECK(coverage(floor.Image) > 50);
}
