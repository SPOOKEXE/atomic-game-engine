#include <engine/imagegraph/SimulationReplay.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>
TEST_SUITE_ID("engine.imagegraph.flip_spawner")
using namespace engine::imagegraph;
namespace {
	Document Spawner(bool splash = false) {
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
			  {"attribute_max_particles", 16.}}},
			{"spawn",
			 "pc.flip_spawner",
			 "",
			 {},
			 {{"spawn_position_unit", EnumValue{0}},
			  {"spawn_position", Vector2{6, 6}},
			  {"spawn_radius", 0.},
			  {"spawn_amount", .5},
			  {"spawn_type", EnumValue{splash ? 1 : 0}},
			  {"spawn_frame", int64_t{2}},
			  {"spawn_duration", int64_t{1}},
			  {"seed", 12345.},
			  {"spawn_direction", ArrayValue{ValueType::Scalar, {0.}}}}}
		};
		document.Links = {{"domain", "domain", "spawn", "domain"}};
		document.Outputs = {{"domain", "spawn", "domain"}};
		return document;
	}
	const FluidDomainData &Data(const SimulationEvaluationResult &result) {
		return *std::get<FluidDomainValue>(std::get<EvaluatedValue>(result.Output).Data).Data;
	}
} // namespace
TEST_CASE(
	"Source FLIP fractional stream and delayed splash retain exact "
	"accumulator and owned reset state",
	"[imagegraph]"
) {
	for (bool splash : {false, true}) {
		const auto document = Spawner(splash);
		Plan plan;
		Diagnostic diagnostic;
		const auto compiled = Compile(document, plan, diagnostic);
		INFO(diagnostic.Message);
		REQUIRE(compiled == Status::Ok);
		EvaluationRequest request;
		request.SimulationAuthoringRevision = 7;
		SimulationEvaluationResult result;
		REQUIRE(EvaluateSimulation(document, plan, "domain", request, result, diagnostic) == Status::Ok);
		REQUIRE(Data(result).Spawners.size() == 1);
		CHECK(Data(result).ParticleCount == 0);
		CHECK(Data(result).Spawners[0].Accumulator == .5);
		request.Tick = 1;
		request.SimulationReplay = &result.Replay;
		REQUIRE(EvaluateSimulation(document, plan, "domain", request, result, diagnostic) == Status::Ok);
		CHECK(Data(result).ParticleCount == (splash ? 0 : 1));
		CHECK(Data(result).Spawners[0].Accumulator == (splash ? 1 : 0));
		request.Tick = 2;
		REQUIRE(EvaluateSimulation(document, plan, "domain", request, result, diagnostic) == Status::Ok);
		CHECK(Data(result).ParticleCount == 1);
		CHECK(Data(result).Spawners[0].Accumulator == .5);
		CHECK(Data(result).Buffers[size_t(FluidBuffer::ParticlePosition)][0] == 8);
		CHECK(Data(result).Buffers[size_t(FluidBuffer::ParticlePosition)][1] == 8);
		CHECK(Data(result).ReadbackPositions.empty());
		request.Tick = 0;
		request.SimulationReplay = nullptr;
		SimulationEvaluationResult reset;
		REQUIRE(EvaluateSimulation(document, plan, "domain", request, reset, diagnostic) == Status::Ok);
		CHECK(Data(reset).ParticleCount == 0);
		CHECK(Data(reset).Spawners[0].Accumulator == .5);
	}
}
TEST_CASE(
	"Actual FLIP surface spawner uses bounded RGBA8 distribution "
	"readback with explicit GPU precision gate",
	"[imagegraph]"
) {
	for (const bool opaque : {false, true}) {
		auto document = Spawner();
		document.Nodes[1].Values.push_back({"spawn_shape", EnumValue{2}});
		for (auto &value : document.Nodes[1].Values)
			if (value.Port == "spawn_amount") value.Data = 2.;
		document.Nodes.push_back(
			{"mask",
			 "image.solid",
			 "",
			 {},
			 {{"width", int64_t{2}},
			  {"height", int64_t{2}},
			  {"colour", Colour{255, 255, 255, uint8_t(opaque ? 255 : 0)}}}}
		);
		document.Links.push_back({"mask", "image", "spawn", "spawn_surface"});
		Plan plan;
		Diagnostic diagnostic;
		const auto compiled = Compile(document, plan, diagnostic);
		INFO(diagnostic.Message);
		REQUIRE(compiled == Status::Ok);
		EvaluationRequest request;
		SimulationEvaluationResult result;
		const auto evaluated = EvaluateSimulation(document, plan, "domain", request, result, diagnostic);
		INFO(diagnostic.Message);
		REQUIRE(evaluated == Status::Ok);
		CHECK(Data(result).ParticleCount == (opaque ? 2 : 0));
		CHECK(Data(result).Spawners[0].Accumulator == 0);
		for (size_t index = 0; index < Data(result).ParticleCount; ++index) {
			const auto &points = Data(result).Buffers[size_t(FluidBuffer::ParticlePosition)];
			CHECK(points[index * 2] >= 7);
			CHECK(points[index * 2] <= 9);
			CHECK(points[index * 2 + 1] >= 7);
			CHECK(points[index * 2 + 1] <= 9);
		}
		const auto previous = result.Replay;
		request.RequireSourceGpuRasterCoverage = true;
		CHECK(
			EvaluateSimulation(document, plan, "domain", request, result, diagnostic) ==
			Status::UnsupportedExecution
		);
		CHECK(result.Replay == previous);
	}
}
