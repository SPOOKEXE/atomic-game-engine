#include "../src/nodes/FlipNodes.hpp"
#include "SourceBuiltinRandomContext.hpp"

#include <engine/imagegraph/FlipReplay.hpp>
#include <engine/imagegraph/SimulationReplay.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>
TEST_SUITE_ID("engine.imagegraph.flip_destroy")
namespace engine::imagegraph::detail {
	bool FlipDestroy(NodeContext &);
}
using namespace engine::imagegraph;
TEST_CASE(
	"FLIP destroy consumes captured C rand in solver slot order and "
	"preserves stale mirrors",
	"[imagegraph]"
) {
	FluidDomainSettings settings;
	settings.Width = settings.Height = 20;
	settings.Spacing = 2;
	settings.MaximumParticles = 4;
	FluidDomainValue reset, domain;
	Diagnostic diagnostic;
	REQUIRE(ResetFlipReplay(settings, 0, 0, Limits::MaximumEvaluationBytes, reset, diagnostic) == Status::Ok);
	const std::array particles{
		FluidSpawnParticle{{5, 5}, {1, 2}},
		FluidSpawnParticle{{9, 5}, {3, 4}},
		FluidSpawnParticle{{5, 5}, {5, 6}}
	};
	REQUIRE(
		SpawnFlipReplay(reset, particles, Limits::MaximumEvaluationBytes, domain, diagnostic) == Status::Ok
	);
	domain.Data->OriginNodeId = "domain";
	Node node{"destroy", "pc.flip_destroy"};
	const auto *entry = FindCatalogueEntry(node.Type);
	REQUIRE(entry);
	EvaluationRequest request;
	SourceBuiltinRandomCapture capture;
	capture.Authored = node;
	capture.Draws = {
		{SourceBuiltinRandomOperation::CRand, 0, 0, 100},
		{SourceBuiltinRandomOperation::CRand, 0, 0, 0},
		{SourceBuiltinRandomOperation::CRand, 0, 0, 1}
	};
	request.BuiltinRandomCaptures = {&capture, 1};
	detail::NodeContext context(node, *entry, request);
	context.ByteBudget = Limits::MaximumEvaluationBytes;
	for (const auto &input : entry->Inputs)
		if (auto fallback = CatalogueDefault(input)) context.Values.emplace_back(input.Id, *fallback);
	context.Values.emplace_back("domain", domain);
	for (auto &[port, value] : context.Values) {
		if (port == "position") value = Vector2{5, 5};
		if (port == "position_unit") value = EnumValue{0};
		if (port == "radius") value = 4.;
		if (port == "chance") value = 0.;
	}
	for (const auto &[port, value] : context.Values)
		capture.Inputs.push_back({std::string(port), value});
	const bool destroyed = detail::FlipDestroy(context);
	INFO(context.FailureMessage);
	REQUIRE(destroyed);
	const auto &output = std::get<FluidDomainValue>(context.OutputValues[0].Data);
	const auto &points = output.Data->Buffers[size_t(FluidBuffer::ParticlePosition)];
	CHECK(points[0] == 0);
	CHECK(points[1] == 0);
	CHECK(points[2] == 9);
	CHECK(points[3] == 5);
	CHECK(points[4] == 5);
	CHECK(points[5] == 5);
	CHECK(output.Data->ParticleCount == 3);
	CHECK(output.Data->SourceParticleCount == domain.Data->SourceParticleCount);
	CHECK(output.Data->ReadbackPositions == domain.Data->ReadbackPositions);
	CHECK(
		output.Data->Buffers[size_t(FluidBuffer::ParticleVelocity)] ==
		domain.Data->Buffers[size_t(FluidBuffer::ParticleVelocity)]
	);
	CHECK(domain.Data->Buffers[size_t(FluidBuffer::ParticlePosition)][0] == 5);
}
TEST_CASE("Actual FLIP destroy graph binds captured C rand and commits geometry atomically", "[imagegraph]") {
	for (const int64_t shape : {int64_t{0}, int64_t{1}}) {
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
			{"fill",
			 "pc.flip_fill",
			 "",
			 {},
			 {{"spawn_area_unit", EnumValue{0}}, {"spawn_area", Area{8, 8, 4, 4}}, {"density", .5}}},
			{"destroy",
			 "pc.flip_destroy",
			 "",
			 {},
			 {{"shape", EnumValue{shape}},
			  {"position_unit", EnumValue{0}},
			  {"position", Vector2{6, 6}},
			  {"radius", 8.},
			  {"size", Vector2{8, 8}},
			  {"chance", 0.}}}
		};
		document.Links = {{"domain", "domain", "fill", "domain"}, {"fill", "domain", "destroy", "domain"}};
		document.Outputs = {{"domain", "destroy", "domain"}};
		Plan plan;
		Diagnostic diagnostic;
		const auto compiled = Compile(document, plan, diagnostic);
		INFO(diagnostic.Message);
		REQUIRE(compiled == Status::Ok);
		EvaluationRequest request;
		SourceBuiltinRandomCapture capture;
		const auto prepared =
			PrepareSourceBuiltinRandomCapture(document, plan, "destroy", request, capture, diagnostic);
		INFO(diagnostic.Message);
		REQUIRE(prepared == Status::Ok);
		capture.Draws.assign(4, {SourceBuiltinRandomOperation::CRand, 0, 0, 0});
		capture.Draws[0].Result = 100;
		request.BuiltinRandomCaptures = {&capture, 1};
		SimulationEvaluationResult result;
		const auto evaluated = EvaluateSimulation(document, plan, "domain", request, result, diagnostic);
		INFO(diagnostic.Message);
		REQUIRE(evaluated == Status::Ok);
		const auto &fluid = std::get<FluidDomainValue>(std::get<EvaluatedValue>(result.Output).Data);
		CHECK(fluid.Data->Buffers[size_t(FluidBuffer::ParticlePosition)][0] == 0);
		CHECK(fluid.Data->Buffers[size_t(FluidBuffer::ParticlePosition)][1] == 0);
		CHECK(fluid.Data->Buffers[size_t(FluidBuffer::ParticlePosition)][2] == 14);
		CHECK(fluid.Data->Buffers[size_t(FluidBuffer::ParticlePosition)][3] == 6);
		CHECK(fluid.Data->Buffers[size_t(FluidBuffer::ParticlePosition)][4] == 6);
		CHECK(fluid.Data->Buffers[size_t(FluidBuffer::ParticlePosition)][5] == 14);
		CHECK(fluid.Data->ParticleCount == 4);
		CHECK(fluid.Data->SourceParticleCount == std::variant<int64_t, double>{4.});
		CHECK(fluid.Data->ReadbackPositions.empty());
		CHECK(fluid.Data->ReadbackVelocities.empty());
		CHECK(fluid.Data->ReadbackLife.empty());
		const auto previous = result.Replay;
		capture.Draws.pop_back();
		CHECK(
			EvaluateSimulation(document, plan, "domain", request, result, diagnostic) == Status::InvalidValue
		);
		CHECK(result.Replay == previous);
	}
}
