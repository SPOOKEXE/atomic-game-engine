#include "FluidPayload.hpp"

#include <engine/imagegraph/SimulationReplay.hpp>
#include <engine/imagegraph/SourceBuiltinRandom.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>
TEST_SUITE_ID("engine.imagegraph.flip_surface")
using namespace engine::imagegraph;
namespace {
	Document SurfaceGraph(
		std::string type, Colour colour, uint32_t width = 6, uint32_t height = 1, double maximum = 16.
	) {
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
			  {"attribute_max_particles", maximum}}},
			{"mask",
			 "image.solid",
			 "",
			 {},
			 {{"width", int64_t(width)}, {"height", int64_t(height)}, {"colour", colour}}},
			{"effect", std::move(type), "", {}, {}}
		};
		const bool fill = document.Nodes[2].Type == "pc.flip_fill";
		if (fill) document.Nodes[2].Values = {{"spawn_shape", EnumValue{1}}, {"density", .5}};
		document.Links = {
			{"domain", "domain", "effect", "domain"},
			{"mask", "image", "effect", fill ? "spawn_surface" : "collider"}
		};
		document.Outputs = {{"domain", "effect", "domain"}};
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
		const EvaluationRequest &request,
		SimulationEvaluationResult &result) {
		Diagnostic diagnostic;
		const auto status = EvaluateSimulation(document, plan, "domain", request, result, diagnostic);
		INFO(diagnostic.Message);
		REQUIRE(status == Status::Ok);
	}
} // namespace
TEST_CASE(
	"Source FLIP surface fill walks linear half-float pixels and "
	"preserves full count at capacity",
	"[imagegraph]"
) {
	for (const double maximum : {1., 16.}) {
		const auto document = SurfaceGraph("pc.flip_fill", {255, 255, 255, 255}, 6, 2, maximum);
		Plan plan;
		CompileGraph(document, plan);
		EvaluationRequest request;
		SimulationEvaluationResult result;
		Run(document, plan, request, result);
		const auto &data = Data(result);
		CHECK(data.ParticleCount == size_t(maximum == 1 ? 1 : 3));
		CHECK(data.SourceParticleCount == std::variant<int64_t, double>{int64_t{3}});
		const auto &position = data.Buffers[size_t(FluidBuffer::ParticlePosition)];
		CHECK(position[0] == 2);
		CHECK(position[1] == 2);
		if (maximum != 1) {
			CHECK(position[2] == 6);
			CHECK(position[3] == 2);
			CHECK(position[4] == 4);
			CHECK(position[5] == 3);
		}
		CHECK(data.ReadbackPositions.empty());
		const auto resetData = data;
		request.Tick = 1;
		request.SimulationReplay = &result.Replay;
		Run(document, plan, request, result);
		CHECK(Data(result).ParticleCount == resetData.ParticleCount);
		CHECK(Data(result).SourceParticleCount == resetData.SourceParticleCount);
		request.Tick = 0;
		request.SimulationReplay = nullptr;
		Run(document, plan, request, result);
		CHECK(Data(result) == resetData);
		request.RequireSourceGpuRasterCoverage = true;
		const auto previous = result.Replay;
		Diagnostic diagnostic;
		CHECK(
			EvaluateSimulation(document, plan, "domain", request, result, diagnostic) ==
			Status::UnsupportedExecution
		);
		CHECK(result.Replay == previous);
	}
}
TEST_CASE("Source FLIP surface fill uses luma alpha once and exact first frame", "[imagegraph]") {
	for (const auto colour :
		 {Colour{255, 0, 0, 255},
		  Colour{0, 255, 0, 255},
		  Colour{255, 255, 255, 127},
		  Colour{255, 255, 255, 128}}) {
		const auto document = SurfaceGraph("pc.flip_fill", colour, 1, 1);
		Plan plan;
		CompileGraph(document, plan);
		EvaluationRequest request;
		SimulationEvaluationResult result;
		Run(document, plan, request, result);
		CHECK(
			Data(result).ParticleCount ==
			size_t(colour == Colour{0, 255, 0, 255} || colour == Colour{255, 255, 255, 128})
		);
		request.Subframe = .5;
		Run(document, plan, request, result);
		CHECK(Data(result).ParticleCount == 0);
	}
}
TEST_CASE(
	"Source FLIP surface solid is sticky until reset and transposes mask "
	"addressing",
	"[imagegraph]"
) {
	auto document = SurfaceGraph("pc.flip_solid", {255, 255, 255, 255}, 1, 1);
	Plan plan;
	CompileGraph(document, plan);
	EvaluationRequest request;
	SimulationEvaluationResult result;
	Run(document, plan, request, result);
	const auto layout = detail::FluidDomainLayout(Data(result).Settings);
	REQUIRE(layout);
	const auto &solid = Data(result).Buffers[size_t(FluidBuffer::Solid)];
	for (uint32_t x = 0; x < layout->Columns; ++x)
		for (uint32_t y = 0; y < layout->Rows; ++y)
			CHECK(
				solid[size_t(x) * layout->Rows + y] ==
				double(x > 0 && y > 0 && x + 1 < layout->Columns && y + 1 < layout->Rows)
			);
	const auto original = solid;
	for (auto &value : document.Nodes[1].Values)
		if (value.Port == "colour") value.Data = Colour{0, 0, 0, 255};
	CompileGraph(document, plan);
	request.Tick = 1;
	request.SimulationReplay = &result.Replay;
	Run(document, plan, request, result);
	CHECK(Data(result).Buffers[size_t(FluidBuffer::Solid)] == original);
	request.Tick = 0;
	request.SimulationReplay = nullptr;
	Run(document, plan, request, result);
	for (const auto value : Data(result).Buffers[size_t(FluidBuffer::Solid)])
		CHECK(value == 0);
	document.Nodes[2].Values = {{"threshold", 0.}};
	CompileGraph(document, plan);
	Run(document, plan, request, result);
	CHECK(Data(result).Buffers[size_t(FluidBuffer::Solid)] == original);
	const auto previous = result.Replay;
	Diagnostic diagnostic;
	request.RequireSourceGpuRasterCoverage = true;
	CHECK(
		EvaluateSimulation(document, plan, "domain", request, result, diagnostic) ==
		Status::UnsupportedExecution
	);
	CHECK(result.Replay == previous);
}
TEST_CASE(
	"Actual source FLIP surface destroy freezes mask pixels and preserves count and velocity", "[imagegraph]"
) {
	auto document = SurfaceGraph("pc.flip_fill", {255, 255, 255, 255}, 6, 2);
	document.Nodes.push_back(
		{"destroy", "pc.flip_destroy", "", {}, {{"shape", EnumValue{2}}, {"chance", 0.}}}
	);
	document.Links.push_back({"effect", "domain", "destroy", "domain"});
	document.Links.push_back({"mask", "image", "destroy", "mask"});
	document.Outputs[0].NodeId = "destroy";
	Plan plan;
	CompileGraph(document, plan);
	EvaluationRequest request;
	SourceBuiltinRandomCapture capture;
	Diagnostic diagnostic;
	const auto prepared =
		PrepareSourceBuiltinRandomCapture(document, plan, "destroy", request, capture, diagnostic);
	INFO(diagnostic.Message);
	REQUIRE(prepared == Status::Ok);
	REQUIRE(capture.InputImages.size() == 1);
	capture.Draws = {
		{SourceBuiltinRandomOperation::CRand, 0, 0, 100},
		{SourceBuiltinRandomOperation::CRand, 0, 0, 1},
		{SourceBuiltinRandomOperation::CRand, 0, 0, 0}
	};
	request.BuiltinRandomCaptures = {&capture, 1};
	SimulationEvaluationResult result;
	Run(document, plan, request, result);
	const auto &positions = Data(result).Buffers[size_t(FluidBuffer::ParticlePosition)];
	CHECK(positions[0] == 0);
	CHECK(positions[1] == 0);
	CHECK(positions[2] == 6);
	CHECK(positions[3] == 2);
	CHECK(positions[4] == 0);
	CHECK(positions[5] == 0);
	CHECK(Data(result).ParticleCount == 3);
	CHECK(Data(result).SourceParticleCount == std::variant<int64_t, double>{int64_t{3}});
	for (const auto velocity : Data(result).Buffers[size_t(FluidBuffer::ParticleVelocity)])
		CHECK(velocity == 0);
	CHECK(Data(result).ReadbackPositions.empty());
	const auto previous = result.Replay;
	capture.InputImages[0].Data.Pixels[0] = 0;
	CHECK(EvaluateSimulation(document, plan, "domain", request, result, diagnostic) == Status::InvalidValue);
	CHECK(result.Replay == previous);
	document.Links.pop_back();
	CompileGraph(document, plan);
	request.BuiltinRandomCaptures = {};
	Run(document, plan, request, result);
	CHECK(Data(result).ParticleCount == 3);
	CHECK(Data(result).Buffers[size_t(FluidBuffer::ParticlePosition)][0] == 2);
}
