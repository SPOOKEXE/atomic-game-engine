#include <engine/imagegraph/SimulationReplay.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
TEST_SUITE_ID("engine.imagegraph.flip_render_extended")
using namespace engine::imagegraph;
namespace {
	Document FlipRenderScene(int64_t updateSteps = 1) {
		Document document;
		document.FormatVersion = 9;
		document.Nodes = {
			{"domain",
			 "pc.flip_domain",
			 "",
			 {},
			 {{"dimension_unit", EnumValue{0}},
			  {"dimension", Vector2{16, 16}},
			  {"particle_size", int64_t{2}},
			  {"attribute_max_particles", 16.},
			  {"attribute_iteration", 2.},
			  {"attribute_iteration_particle", 0.},
			  {"attribute_skip_incompressible", true},
			  {"gravity", 5.},
			  {"time_step", .1}}},
			{"fill",
			 "pc.flip_fill",
			 "",
			 {},
			 {{"spawn_area_unit", EnumValue{0}}, {"spawn_area", Area{8, 8, 4, 4}}, {"density", .5}}},
			{"render",
			 "pc.flip_render",
			 "",
			 {},
			 {{"update_step", updateSteps},
			  {"particle_size", 2.},
			  {"draw_obstracles", false},
			  {"threshold", false},
			  {"alpha", Vector2{1, 1}},
			  {"lifespan", Vector2{0, 0}}}}
		};
		document.Links = {{"domain", "domain", "fill", "domain"}, {"fill", "domain", "render", "domain"}};
		document.Outputs = {{"image", "render", "rendered"}};
		return document;
	}
	void CompileGraph(const Document &document, Plan &plan) {
		Diagnostic diagnostic;
		const auto status = Compile(document, plan, diagnostic);
		INFO(diagnostic.Message);
		REQUIRE(status == Status::Ok);
	}
	void
	Run(const Document &document,
		const Plan &plan,
		const EvaluationRequest &request,
		SimulationEvaluationResult &result) {
		Diagnostic diagnostic;
		const auto status = EvaluateSimulation(document, plan, "image", request, result, diagnostic);
		INFO(diagnostic.Message);
		REQUIRE(status == Status::Ok);
	}
}
TEST_CASE(
	"Actual FLIP line graph uses owned previous readback histories and repeats reset pixels", "[imagegraph]"
) {
	auto document = FlipRenderScene();
	for (auto &value : document.Nodes[0].Values) {
		if (value.Port == "gravity") value.Data = 10.;
		if (value.Port == "time_step") value.Data = .2;
	}
	document.Nodes[2].Values.push_back({"render_type", EnumValue{1}});
	document.Nodes[2].Values.push_back({"segments", int64_t{3}});
	document.Nodes[2].Values.push_back({"thickness", 3.});
	Plan plan;
	CompileGraph(document, plan);
	EvaluationRequest request;
	SimulationEvaluationResult result;
	Run(document, plan, request, result);
	const auto initial = std::get<Image>(result.Output);
	CHECK(std::all_of(initial.Pixels.begin(), initial.Pixels.end(), [](uint8_t byte) { return byte == 0; }));
	request.Tick = 1;
	request.SimulationReplay = &result.Replay;
	Run(document, plan, request, result);
	const auto moving = std::get<Image>(result.Output);
	REQUIRE(result.Replay.Entries.size() == 1);
	REQUIRE(result.Replay.Entries[0].Fluid.Data);
	const auto &fluid = *result.Replay.Entries[0].Fluid.Data;
	REQUIRE(fluid.ReadbackPositions.size() == 8);
	CHECK(std::abs(fluid.ReadbackPositions[1] - 10.) < 1e-9);
	const auto history = std::find_if(fluid.History.begin(), fluid.History.end(), [](const auto &frame) {
		return frame.Tick == 1;
	});
	REQUIRE(history != fluid.History.end());
	REQUIRE(history->Positions.size() == 8);
	CHECK(std::abs(history->Positions[1] - 7.2) < 1e-9);
	CHECK(std::any_of(moving.Pixels.begin(), moving.Pixels.end(), [](uint8_t byte) { return byte != 0; }));
	request.Tick = 0;
	request.SimulationReplay = nullptr;
	Run(document, plan, request, result);
	CHECK(std::get<Image>(result.Output) == initial);
	request.Tick = 1;
	request.SimulationReplay = &result.Replay;
	Run(document, plan, request, result);
	CHECK(std::get<Image>(result.Output) == moving);
	const auto captured = result.Replay;
	request.ReuseSimulationFrame = true;
	Run(document, plan, request, result);
	CHECK(result.Replay == captured);
	CHECK(std::get<Image>(result.Output) == moving);
	request.ReuseSimulationFrame = false;
	for (auto &value : document.Nodes[2].Values)
		if (value.Port == "alpha") value.Data = Vector2{0, 0};
	document.Nodes[2].Values.push_back({"color_over_velocity", Gradient{0, {{0, {255, 255, 255, 0}}}}});
	CompileGraph(document, plan);
	request.Tick = 0;
	request.SimulationReplay = nullptr;
	Run(document, plan, request, result);
	request.Tick = 1;
	request.SimulationReplay = &result.Replay;
	Run(document, plan, request, result);
	CHECK(std::get<Image>(result.Output) == moving);
	const auto prior = result.Replay;
	Diagnostic diagnostic;
	request.Tick = 2;
	request.RequireSourceGpuRasterCoverage = true;
	CHECK(
		EvaluateSimulation(document, plan, "image", request, result, diagnostic) ==
		Status::UnsupportedExecution
	);
	CHECK(result.Replay == prior);
	CHECK(std::get<Image>(result.Output) == moving);
}
TEST_CASE(
	"Actual FLIP sprite image and frame-array graphs use packed alpha and first-frame origins", "[imagegraph]"
) {
	for (const bool sequence : {false, true}) {
		auto document = FlipRenderScene();
		for (auto &value : document.Nodes[2].Values) {
			if (value.Port == "alpha") value.Data = Vector2{.5, .5};
		}
		document.Nodes[2].Values.push_back({"additive", false});
		document.Nodes.push_back(
			{"red",
			 "image.solid",
			 "",
			 {},
			 {{"width", int64_t{2}}, {"height", int64_t{2}}, {"colour", Colour{255, 0, 0, 255}}}}
		);
		if (sequence) {
			document.Nodes.push_back(
				{"blue",
				 "image.solid",
				 "",
				 {},
				 {{"width", int64_t{4}}, {"height", int64_t{1}}, {"colour", Colour{0, 0, 255, 255}}}}
			);
			Node frames{"frames", "pc.array", "", {}, {{"type", EnumValue{1}}}};
			frames.DynamicInputs = {{"input_0", ValueType::Image, {}}, {"input_1", ValueType::Image, {}}};
			document.Nodes.push_back(std::move(frames));
			document.Links.push_back({"red", "image", "frames", "input_0"});
			document.Links.push_back({"blue", "image", "frames", "input_1"});
			document.Links.push_back({"frames", "array", "render", "fluid_particle"});
		} else
			document.Links.push_back({"red", "image", "render", "fluid_particle"});
		Plan plan;
		CompileGraph(document, plan);
		EvaluationRequest request;
		SimulationEvaluationResult result;
		Run(document, plan, request, result);
		const auto image = std::get<Image>(result.Output);
		const size_t redPixel = (3 * image.Width + 3) * 4;
		CHECK(image.Pixels[redPixel] == 63);
		CHECK(image.Pixels[redPixel + 1] == 0);
		CHECK(image.Pixels[redPixel + 2] == 0);
		CHECK(image.Pixels[redPixel + 3] == 63);
		if (sequence) {
			const size_t bluePixel = (3 * image.Width + 14) * 4;
			CHECK(image.Pixels[bluePixel] == 0);
			CHECK(image.Pixels[bluePixel + 1] == 0);
			CHECK(image.Pixels[bluePixel + 2] == 63);
			CHECK(image.Pixels[bluePixel + 3] == 63);
		}
		SimulationEvaluationResult reset;
		Run(document, plan, request, reset);
		CHECK(std::get<Image>(reset.Output) == image);
	}
}
