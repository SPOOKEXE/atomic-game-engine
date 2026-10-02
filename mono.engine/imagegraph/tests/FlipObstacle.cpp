#include "FluidPayload.hpp"

#include <engine/imagegraph/SimulationReplay.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>
TEST_SUITE_ID("engine.imagegraph.flip_obstacle")
using namespace engine::imagegraph;
namespace {
	Document Obstacles(int64_t shape) {
		Document document;
		document.FormatVersion = 9;
		document.Nodes = {
			{"a",
			 "pc.flip_domain",
			 "",
			 {},
			 {{"dimension_unit", EnumValue{0}},
			  {"dimension", Vector2{16, 16}},
			  {"particle_size", int64_t{2}},
			  {"attribute_max_particles", 4.},
			  {"time_step", .05}}},
			{"b",
			 "pc.flip_domain",
			 "",
			 {},
			 {{"dimension_unit", EnumValue{0}},
			  {"dimension", Vector2{16, 16}},
			  {"particle_size", int64_t{2}},
			  {"attribute_max_particles", 4.}}},
			{"shape",
			 "pc.flip_apply_force",
			 "",
			 {},
			 {{"shape", EnumValue{shape}},
			  {"position_unit", EnumValue{0}},
			  {"position", Vector2{4, 4}},
			  {"radius", 3.},
			  {"size", Vector2{3, 3}}}},
			{"texture",
			 "image.solid",
			 "",
			 {},
			 {{"width", int64_t{2}}, {"height", int64_t{2}}, {"colour", Colour{255, 255, 255, 255}}}},
			{"render", "pc.flip_render", "", {}, {{"update_step", int64_t{0}}}}
		};
		document.Links = {
			{"a", "domain", "shape", "domain"},
			{"texture", "image", "shape", "texture"},
			{"shape", "domain", "render", "domain"}
		};
		document.Outputs = {
			{"domain", "shape", "domain"}, {"old", "a", "domain"}, {"image", "render", "rendered"}
		};
		return document;
	}
	const FluidDomainData &Data(const SimulationEvaluationResult &result) {
		return *std::get<FluidDomainValue>(std::get<EvaluatedValue>(result.Output).Data).Data;
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
		const std::string &output,
		const EvaluationRequest &request,
		SimulationEvaluationResult &result) {
		Diagnostic diagnostic;
		const auto status = EvaluateSimulation(document, plan, output, request, result, diagnostic);
		INFO(diagnostic.Message);
		REQUIRE(status == Status::Ok);
	}
}
TEST_CASE(
	"Source FLIP shape collider owns persistent native index and exact moving grid velocities", "[imagegraph]"
) {
	for (const int64_t shape : {int64_t{0}, int64_t{1}}) {
		auto document = Obstacles(shape);
		Plan plan;
		CompileGraph(document, plan);
		EvaluationRequest request;
		SimulationEvaluationResult result;
		Run(document, plan, "domain", request, result);
		REQUIRE(Data(result).Obstacles.size() == 1);
		REQUIRE(Data(result).ObstacleControls.size() == 1);
		REQUIRE(Data(result).ObstacleVisuals.size() == 1);
		CHECK(Data(result).Obstacles[0].VelocityX == 1);
		CHECK(Data(result).Obstacles[0].VelocityY == 1);
		const auto layout = detail::FluidDomainLayout(Data(result).Settings);
		REQUIRE(layout);
		CHECK(Data(result).Buffers[size_t(FluidBuffer::Open)][2 * layout->Rows + 2] == 0);
		CHECK(Data(result).Buffers[size_t(FluidBuffer::U)][2 * layout->Rows + 2] == 1);
		const auto initial = Data(result);
		for (auto &value : document.Nodes[2].Values)
			if (value.Port == "position") value.Data = Vector2{6, 5};
		CompileGraph(document, plan);
		request.Tick = 1;
		request.SimulationReplay = &result.Replay;
		Run(document, plan, "domain", request, result);
		CHECK(Data(result).Obstacles.size() == 1);
		CHECK(Data(result).ObstacleVisuals.size() == 1);
		CHECK(Data(result).Obstacles[0].VelocityX == .5);
		CHECK(Data(result).Obstacles[0].VelocityY == .25);
		CHECK(Data(result).ObstacleControls[0].Serial == 2);
		CHECK(Data(result).ParticleCount == 0);
		for (auto &value : document.Nodes[2].Values)
			if (value.Port == "position") value.Data = Vector2{4, 4};
		CompileGraph(document, plan);
		request.Tick = 0;
		request.SimulationReplay = nullptr;
		Run(document, plan, "domain", request, result);
		CHECK(Data(result) == initial);
	}
}
TEST_CASE(
	"Source FLIP changed domain reuses old index while original visual observes latest node controls",
	"[imagegraph]"
) {
	auto document = Obstacles(0);
	Plan plan;
	CompileGraph(document, plan);
	EvaluationRequest request;
	SimulationEvaluationResult result;
	Run(document, plan, "domain", request, result);
	request.Tick = 1;
	request.SimulationReplay = &result.Replay;
	document.Links[0].FromNode = "b";
	for (auto &value : document.Nodes[2].Values)
		if (value.Port == "position") value.Data = Vector2{8, 8};
	CompileGraph(document, plan);
	Run(document, plan, "domain", request, result);
	CHECK(Data(result).Obstacles.empty());
	CHECK(Data(result).ObstacleVisuals.empty());
	REQUIRE(Data(result).ObstacleControls.size() == 1);
	CHECK(Data(result).ObstacleControls[0].Index == 0);
	CHECK(Data(result).ObstacleControls[0].Serial == 2);
	Run(document, plan, "old", request, result);
	REQUIRE(Data(result).Obstacles.size() == 1);
	CHECK(Data(result).Obstacles[0].X == 4);
	REQUIRE(Data(result).ObstacleVisuals.size() == 1);
	CHECK(Data(result).ObstacleControls[0].X == 8);
	CHECK(Data(result).ObstacleControls[0].Y == 8);
	CHECK(Data(result).ObstacleControls[0].Serial == 2);
	document.Links[2].FromNode = "a";
	CompileGraph(document, plan);
	Run(document, plan, "image", request, result);
	const auto &image = std::get<Image>(result.Output);
	CHECK(image.Pixels[(7 * image.Width + 7) * 4] == 255);
	CHECK(image.Pixels[(3 * image.Width + 3) * 4] == 0);
	const auto previous = result.Replay;
	Diagnostic diagnostic;
	request.RequireSourceGpuRasterCoverage = true;
	CHECK(
		EvaluateSimulation(document, plan, "image", request, result, diagnostic) ==
		Status::UnsupportedExecution
	);
	CHECK(result.Replay == previous);
}
TEST_CASE("Source FLIP obstacle texture uses centered point sampling and normal RGBA blend", "[imagegraph]") {
	auto document = Obstacles(0);
	for (auto &value : document.Nodes[3].Values)
		if (value.Port == "colour") value.Data = Colour{255, 255, 255, 128};
	Plan plan;
	CompileGraph(document, plan);
	EvaluationRequest request;
	SimulationEvaluationResult result;
	Run(document, plan, "image", request, result);
	const auto &image = std::get<Image>(result.Output);
	const size_t pixel = (3 * image.Width + 3) * 4;
	CHECK(image.Pixels[pixel] == 128);
	CHECK(image.Pixels[pixel + 1] == 128);
	CHECK(image.Pixels[pixel + 2] == 128);
	CHECK(image.Pixels[pixel + 3] == 64);
	const auto previous = result.Replay;
	const auto previousImage = image;
	Diagnostic diagnostic;
	CHECK(
		EvaluateSimulation(document, plan, "image", request, result, diagnostic, 1) == Status::LimitExceeded
	);
	CHECK(result.Replay == previous);
	CHECK(std::get<Image>(result.Output) == previousImage);
}
