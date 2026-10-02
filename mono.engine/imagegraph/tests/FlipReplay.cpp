#include "FluidPayload.hpp"
#include "NodeHarness.hpp"

#include <engine/imagegraph/FlipReplay.hpp>
#include <engine/imagegraph/SimulationReplay.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <limits>

TEST_SUITE_ID("engine.imagegraph.flip_replay")
using namespace engine::imagegraph;
namespace {
	FluidDomainValue Reset(FluidDomainSettings settings = {}) {
		FluidDomainValue state;
		Diagnostic diagnostic;
		REQUIRE(
			ResetFlipReplay(settings, 0, 17, Limits::MaximumEvaluationBytes, state, diagnostic) == Status::Ok
		);
		return state;
	}
	Status Step(
		FluidDomainValue &state,
		uint64_t tick,
		Diagnostic &diagnostic,
		uint64_t work = FluidDomainLimits::MaximumWork
	) {
		return StepFlipReplay(state, tick, 17, Limits::MaximumEvaluationBytes, work, state, diagnostic);
	}
}
TEST_CASE("FLIP reset owns zeroed source grid and particle buffers", "[imagegraph]") {
	const auto state = Reset();
	REQUIRE(state.Data);
	CHECK(detail::ValidFluidPayload(state));
	CHECK(state.Data->ParticleCount == 0);
	CHECK(state.Data->ParticleRestDensity == 0);
	CHECK(state.Data->Buffers[size_t(FluidBuffer::U)].size() == 81);
	CHECK(state.Data->Buffers[size_t(FluidBuffer::ParticlePosition)].size() == 8192);
	for (const auto &buffer : state.Data->Buffers)
		for (const auto value : buffer)
			REQUIRE(value == 0);
}
TEST_CASE("FLIP source spawning truncates capacity without advancing tick", "[imagegraph]") {
	FluidDomainSettings settings;
	settings.MaximumParticles = 2;
	auto state = Reset(settings);
	const std::array particles{
		FluidSpawnParticle{{8, 9}, {1, 2}},
		FluidSpawnParticle{{12, 13}, {3, 4}},
		FluidSpawnParticle{{16, 17}, {5, 6}}
	};
	Diagnostic diagnostic;
	REQUIRE(
		SpawnFlipReplay(state, particles, Limits::MaximumEvaluationBytes, state, diagnostic) == Status::Ok
	);
	CHECK(state.Data->ParticleCount == 2);
	CHECK(state.Data->Tick == 0);
	CHECK(state.Data->Buffers[size_t(FluidBuffer::ParticlePosition)] == std::vector<double>{8, 9, 12, 13});
	CHECK(state.Data->Buffers[size_t(FluidBuffer::ParticleVelocity)] == std::vector<double>{1, 2, 3, 4});
	const auto full = state;
	REQUIRE(
		SpawnFlipReplay(state, particles, Limits::MaximumEvaluationBytes, state, diagnostic) == Status::Ok
	);
	CHECK(*state.Data == *full.Data);
}
TEST_CASE("FLIP source integration applies full timestep on each global iteration", "[imagegraph]") {
	FluidDomainSettings settings;
	settings.SkipIncompressible = true;
	settings.GlobalIterations = 2;
	settings.ParticleIterations = 0;
	settings.TimeStep = .1;
	settings.Gravity = 5;
	auto state = Reset(settings);
	const std::array particles{FluidSpawnParticle{{8, 8}, {}}};
	Diagnostic diagnostic;
	REQUIRE(
		SpawnFlipReplay(state, particles, Limits::MaximumEvaluationBytes, state, diagnostic) == Status::Ok
	);
	const auto previous = state;
	REQUIRE(Step(state, 1, diagnostic) == Status::Ok);
	CHECK(state.Data->Buffers[size_t(FluidBuffer::ParticlePosition)][0] == Catch::Approx(8));
	CHECK(state.Data->Buffers[size_t(FluidBuffer::ParticlePosition)][1] == Catch::Approx(8.15));
	CHECK(state.Data->Buffers[size_t(FluidBuffer::ParticleVelocity)][1] == Catch::Approx(1));
	CHECK(state.Data->Buffers[size_t(FluidBuffer::ParticleLife)][0] == Catch::Approx(1));
	CHECK(previous.Data->Buffers[size_t(FluidBuffer::ParticlePosition)][1] == 8);
	CHECK(state.Data->Tick == 1);
}
TEST_CASE("FLIP fixed reset and full pressure solver replay have identical owned snapshots", "[imagegraph]") {
	FluidDomainSettings settings;
	settings.MaximumParticles = 32;
	settings.GlobalIterations = 2;
	settings.PressureIterations = 3;
	settings.Gravity = 1;
	const std::array particles{
		FluidSpawnParticle{{10, 10}, {}},
		FluidSpawnParticle{{14, 10}, {}},
		FluidSpawnParticle{{10, 14}, {}},
		FluidSpawnParticle{{14, 14}, {}}
	};
	auto first = Reset(settings), second = Reset(settings);
	Diagnostic diagnostic;
	REQUIRE(
		SpawnFlipReplay(first, particles, Limits::MaximumEvaluationBytes, first, diagnostic) == Status::Ok
	);
	REQUIRE(
		SpawnFlipReplay(second, particles, Limits::MaximumEvaluationBytes, second, diagnostic) == Status::Ok
	);
	for (uint64_t tick = 1; tick <= 128; ++tick) {
		REQUIRE(Step(first, tick, diagnostic) == Status::Ok);
		REQUIRE(Step(second, tick, diagnostic) == Status::Ok);
		if (tick == 1) {
			// Captured from the pinned native FLIP C++ source after its explicit reset.
			CHECK(first.Data->Buffers[size_t(FluidBuffer::ParticlePosition)][1] == Catch::Approx(10.005));
			CHECK(first.Data->Buffers[size_t(FluidBuffer::ParticleVelocity)][1] == 0);
		}
	}
	CHECK(first.Data->Buffers[size_t(FluidBuffer::ParticlePosition)][1] == Catch::Approx(10.639999999999873));
	CHECK(first.Data->Buffers[size_t(FluidBuffer::ParticlePosition)][5] == Catch::Approx(14.639999999999873));
	CHECK(first.Data->ParticleRestDensity == 0);
	CHECK(*first.Data == *second.Data);
	second = Reset(settings);
	REQUIRE(
		SpawnFlipReplay(second, particles, Limits::MaximumEvaluationBytes, second, diagnostic) == Status::Ok
	);
	for (uint64_t tick = 1; tick <= 128; ++tick)
		REQUIRE(Step(second, tick, diagnostic) == Status::Ok);
	CHECK(*first.Data == *second.Data);
}
TEST_CASE("FLIP failures preserve both prior and published caller state", "[imagegraph]") {
	auto state = Reset();
	const auto original = state;
	Diagnostic diagnostic;
	CHECK(Step(state, 2, diagnostic) == Status::InvalidValue);
	CHECK(*state.Data == *original.Data);
	CHECK(
		StepFlipReplay(
			state, 1, 18, Limits::MaximumEvaluationBytes, FluidDomainLimits::MaximumWork, state, diagnostic
		) == Status::InvalidValue
	);
	CHECK(*state.Data == *original.Data);
	CHECK(Step(state, 1, diagnostic, 1) == Status::LimitExceeded);
	CHECK(*state.Data == *original.Data);
	const std::array bad{FluidSpawnParticle{{std::numeric_limits<double>::infinity(), 8}, {}}};
	CHECK(
		SpawnFlipReplay(state, bad, Limits::MaximumEvaluationBytes, state, diagnostic) == Status::InvalidValue
	);
	CHECK(*state.Data == *original.Data);
	CHECK(ResetFlipReplay({}, 0, 17, 1, state, diagnostic) == Status::LimitExceeded);
	CHECK(*state.Data == *original.Data);
	state.Data->Buffers[size_t(FluidBuffer::U)].pop_back();
	const auto malformed = state;
	CHECK(Step(state, 1, diagnostic) == Status::InvalidValue);
	CHECK(*state.Data == *malformed.Data);
}

TEST_CASE("FLIP graph constructors fill and update capture final domain aliases atomically", "[imagegraph]") {
	Document document;
	document.FormatVersion = 6;
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
		{"step", "pc.flip_update", "", {}, {}},
	};
	document.Links = {{"domain", "domain", "fill", "domain"}, {"fill", "domain", "step", "domain"}};
	document.Outputs = {{"fluid", "step", "domain"}, {"origin", "domain", "domain"}};
	Plan plan;
	Diagnostic diagnostic;
	const auto compiled = Compile(document, plan, diagnostic);
	INFO(diagnostic.Message);
	REQUIRE(compiled == Status::Ok);
	EvaluationRequest request;
	request.SimulationAuthoringRevision = 17;
	SimulationEvaluationResult result;
	const auto status = EvaluateSimulation(document, plan, "fluid", request, result, diagnostic);
	INFO(diagnostic.Message);
	REQUIRE(status == Status::Ok);
	REQUIRE(result.Replay.Entries.size() == 1);
	REQUIRE(result.Replay.Entries[0].Fluid.Data);
	CHECK(result.Replay.Entries[0].NodeId == "domain");
	CHECK(result.Replay.Entries[0].Fluid.Data->ParticleCount == 4);
	CHECK(
		result.Replay.Entries[0].Fluid.Data->Buffers[size_t(FluidBuffer::ParticlePosition)][1] ==
		Catch::Approx(6.15)
	);
	CHECK(ValidateSimulationReplay(result.Replay, Limits::MaximumEvaluationBytes, diagnostic) == Status::Ok);
	const auto &output = std::get<FluidDomainValue>(std::get<EvaluatedValue>(result.Output).Data);
	CHECK(*output.Data == *result.Replay.Entries[0].Fluid.Data);
	request.SimulationReplay = &result.Replay;
	request.Tick = 1;
	REQUIRE(EvaluateSimulation(document, plan, "fluid", request, result, diagnostic) == Status::Ok);
	CHECK(result.Replay.Entries[0].Fluid.Data->ParticleCount == 4);
	CHECK(
		result.Replay.Entries[0].Fluid.Data->Buffers[size_t(FluidBuffer::ParticlePosition)][1] ==
		Catch::Approx(6.5)
	);
	const auto previous = result.Replay;
	request.Tick = 3;
	CHECK(EvaluateSimulation(document, plan, "fluid", request, result, diagnostic) == Status::InvalidValue);
	CHECK(result.Replay == previous);
}

TEST_CASE(
	"FLIP source affectors skip particle zero and preserve strict source shape bounds", "[imagegraph]"
) {
	auto state = Reset();
	const std::array particles{
		FluidSpawnParticle{{8, 8}, {}}, FluidSpawnParticle{{10, 8}, {}}, FluidSpawnParticle{{12, 8}, {}}
	};
	Diagnostic diagnostic;
	REQUIRE(
		SpawnFlipReplay(state, particles, Limits::MaximumEvaluationBytes, state, diagnostic) == Status::Ok
	);
	const auto circle = imagegraph_test::RunNode(
		"pc.flip_apply_velocity",
		{},
		{{"domain", state},
		 {"position", Vector2{8, 8}},
		 {"position_unit", EnumValue{0}},
		 {"radius", 4.},
		 {"velocity", Vector2{3, 4}}}
	);
	INFO(circle.Message);
	REQUIRE(circle.Ok);
	const auto *circleValue = circle.OutputValue("domain");
	REQUIRE(circleValue);
	const auto &velocities =
		std::get<FluidDomainValue>(*circleValue).Data->Buffers[size_t(FluidBuffer::ParticleVelocity)];
	CHECK(velocities[0] == 0);
	CHECK(velocities[1] == 0);
	CHECK(velocities[2] == 3);
	CHECK(velocities[3] == 4);
	CHECK(velocities[4] == 0);
	CHECK(velocities[5] == 0);
	const auto rectangle = imagegraph_test::RunNode(
		"pc.flip_apply_velocity",
		{},
		{{"domain", state},
		 {"position", Vector2{12, 12}},
		 {"position_unit", EnumValue{0}},
		 {"shape", EnumValue{1}},
		 {"size", Vector2{2, 4}},
		 {"velocity", Vector2{3, 4}}}
	);
	INFO(rectangle.Message);
	REQUIRE(rectangle.Ok);
	const auto &rectangleVel = std::get<FluidDomainValue>(*rectangle.OutputValue("domain"))
								   .Data->Buffers[size_t(FluidBuffer::ParticleVelocity)];
	CHECK(rectangleVel[2] == 3);
	CHECK(rectangleVel[3] == 4);
	CHECK(rectangleVel[4] == 0);
	const auto repel = imagegraph_test::RunNode(
		"pc.flip_repel",
		{},
		{{"domain", state},
		 {"position", Vector2{8, 8}},
		 {"position_unit", EnumValue{0}},
		 {"radius", 10.},
		 {"strength", 1.}}
	);
	INFO(repel.Message);
	REQUIRE(repel.Ok);
	const auto &repelVel = std::get<FluidDomainValue>(*repel.OutputValue("domain"))
							   .Data->Buffers[size_t(FluidBuffer::ParticleVelocity)];
	CHECK(repelVel[0] == 0);
	CHECK(repelVel[2] == Catch::Approx(6.4));
	CHECK(repelVel[4] == Catch::Approx(4.8));
	const auto vortex = imagegraph_test::RunNode(
		"pc.flip_vortex",
		{},
		{{"domain", state},
		 {"position", Vector2{8, 8}},
		 {"position_unit", EnumValue{0}},
		 {"radius", 10.},
		 {"strength", 4.},
		 {"attraction", 2.}}
	);
	INFO(vortex.Message);
	REQUIRE(vortex.Ok);
	const auto &vortexVel = std::get<FluidDomainValue>(*vortex.OutputValue("domain"))
								.Data->Buffers[size_t(FluidBuffer::ParticleVelocity)];
	CHECK(vortexVel[0] == 0);
	CHECK(vortexVel[2] == Catch::Approx(-2));
	CHECK(vortexVel[3] == Catch::Approx(3.2));
}

TEST_CASE("FLIP zero timestep skips pressure safely and still ages source particles", "[imagegraph]") {
	FluidDomainSettings settings;
	settings.TimeStep = 0;
	settings.SkipIncompressible = true;
	auto state = Reset(settings);
	Diagnostic diagnostic;
	const std::array particles{FluidSpawnParticle{{8, 8}, {1, 2}}};
	REQUIRE(
		SpawnFlipReplay(state, particles, Limits::MaximumEvaluationBytes, state, diagnostic) == Status::Ok
	);
	REQUIRE(Step(state, 1, diagnostic) == Status::Ok);
	CHECK(state.Data->Buffers[size_t(FluidBuffer::ParticlePosition)][0] == 8);
	CHECK(state.Data->Buffers[size_t(FluidBuffer::ParticleLife)][0] == Catch::Approx(1));
	state.Data->Settings.SkipIncompressible = false;
	const auto previous = state;
	CHECK(Step(state, 2, diagnostic) == Status::UnsupportedExecution);
	CHECK(*state.Data == *previous.Data);
}
