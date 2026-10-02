#include "SourceVfxParticles.hpp"
#include "SourceVfxRaster.hpp"

#include <engine/imagegraph/SimulationReplay.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>
TEST_SUITE_ID("engine.imagegraph.vfx_renderer")
using namespace engine::imagegraph;
TEST_CASE("VFX base particles preserve point alpha and signed rotated surface semantics", "[imagegraph]") {
	Node node{"renderer", "pc.vfx_renderer", "", {}, {}};
	const auto *entry = FindCatalogueEntry(node.Type);
	REQUIRE(entry);
	EvaluationRequest request;
	detail::NodeContext context(node, *entry, request);
	context.ByteBudget = Limits::MaximumEvaluationBytes;
	Image output{8, 8, std::vector<uint8_t>(8 * 8 * 4)};
	ParticleData2D point;
	point.State.Active = true;
	point.State.Position = {2, 3};
	point.State.Blend = 0x0000ff;
	point.State.Alpha = 0;
	uint64_t work = 0;
	REQUIRE(detail::DrawSourceVfxBaseParticle(context, output, point, 0, false, work));
	CHECK(detail::ReadPixel(output, 2, 2) == std::array<double, 4>{1, 0, 0, 1});
	point.State.Active = false;
	REQUIRE(detail::DrawSourceVfxBaseParticle(context, output, point, 0, false, work));
	CHECK(work == 1);
	REQUIRE(detail::DrawSourceVfxBaseParticle(context, output, point, 0, false, work, true));
	CHECK(work == 2);
	ParticleData2D surface;
	surface.State.Active = true;
	surface.State.Position = {4, 4};
	surface.State.SpriteSlot = 0;
	surface.State.Scale = {-1, 1};
	surface.State.RotationDegrees = 90;
	surface.State.Alpha = .5;
	surface.Sprites = {{2, 1, {255, 0, 0, 255, 0, 0, 255, 255}}};
	output.Pixels.assign(output.Pixels.size(), 0);
	REQUIRE(detail::DrawSourceVfxBaseParticle(context, output, surface, 1, false, work));
	CHECK(output.Pixels[(3 * 8 + 3) * 4] == 255);
	CHECK(output.Pixels[(4 * 8 + 3) * 4 + 2] == 255);
	CHECK(output.Pixels[(3 * 8 + 3) * 4 + 3] == 127);
}
TEST_CASE(
	"Actual FLIP to VFX renderer graph repeats seeded pixels and captures exact solver refresh",
	"[imagegraph]"
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
		  {"attribute_max_particles", 16.},
		  {"attribute_iteration", 2.},
		  {"attribute_iteration_particle", 0.},
		  {"attribute_skip_incompressible", true},
		  {"gravity", 0.}}},
		{"fill",
		 "pc.flip_fill",
		 "",
		 {},
		 {{"spawn_area_unit", EnumValue{0}}, {"spawn_area", Area{8, 8, 4, 4}}, {"density", .5}}},
		{"step", "pc.flip_update", "", {}, {}},
		{"vfx", "pc.flip_to_vfx", "", {}, {{"attribute_part_amount", 4.}}},
		{"scope",
		 "pc.vfx_group_inline",
		 "",
		 {},
		 {{"dimension_unit", EnumValue{0}}, {"dimension", Vector2{16, 16}}}},
		{"renderer", "pc.vfx_renderer", "scope/inline", {}, {}}
	};
	document.Nodes.back().DynamicInputs = {
		{"blend_mode_0", ValueType::Enum, Value{EnumValue{0}}}, {"input_1_0", ValueType::Particle, {}}
	};
	Group group;
	group.Id = "scope/inline";
	group.OwnerNodeId = "scope";
	document.Groups = {group};
	document.Links = {
		{"domain", "domain", "fill", "domain"},
		{"fill", "domain", "step", "domain"},
		{"step", "domain", "vfx", "domain"},
		{"vfx", "particles", "renderer", "input_1_0"}
	};
	document.Outputs = {{"image", "renderer", "surface_out"}};
	Plan plan;
	Diagnostic diagnostic;
	auto status = Compile(document, plan, diagnostic);
	INFO(diagnostic.Message);
	REQUIRE(status == Status::Ok);
	EvaluationRequest request;
	request.Seed = 7;
	request.SimulationAuthoringRevision = 9;
	SimulationEvaluationResult result;
	status = EvaluateSimulation(document, plan, "image", request, result, diagnostic);
	INFO(diagnostic.Message);
	REQUIRE(status == Status::Ok);
	const auto image = std::get<Image>(result.Output);
	CHECK(detail::ReadPixel(image, 6, 5) == std::array<double, 4>{1, 1, 1, 1});
	const auto captured = result.Replay;
	request.SimulationReplay = &result.Replay;
	request.ReuseSimulationFrame = true;
	REQUIRE(EvaluateSimulation(document, plan, "image", request, result, diagnostic) == Status::Ok);
	CHECK(result.Replay == captured);
	CHECK(std::get<Image>(result.Output) == image);
	request.ReuseSimulationFrame = false;
	request.SimulationReplay = nullptr;
	SimulationEvaluationResult reset;
	REQUIRE(EvaluateSimulation(document, plan, "image", request, reset, diagnostic) == Status::Ok);
	CHECK(std::get<Image>(reset.Output) == image);
	request.RequireSourceGpuRasterCoverage = true;
	CHECK(
		EvaluateSimulation(document, plan, "image", request, result, diagnostic) ==
		Status::UnsupportedExecution
	);
	CHECK(std::get<Image>(result.Output) == image);
}
TEST_CASE(
	"VFX renderer visits source generic pool rows in order and refuses deeper objects", "[imagegraph]"
) {
	Node node{"renderer", "pc.vfx_renderer", "", {}, {}};
	const auto *entry = FindCatalogueEntry(node.Type);
	REQUIRE(entry);
	EvaluationRequest request;
	detail::NodeContext context(node, *entry, request);
	context.ByteBudget = Limits::MaximumEvaluationBytes;
	ParticleValue first, second;
	first.Data.emplace().State.Position = {2, 3};
	second.Data.emplace().State.Position = {5, 7};
	ArrayValue pools;
	pools.ElementType = ValueType::Particle;
	pools.Items = {
		{std::vector<SourceArrayItem>{{ElementValue{first}}}},
		{std::vector<SourceArrayItem>{{ElementValue{second}}}}
	};
	Value value{pools};
	std::vector<std::array<double, 2>> positions;
	REQUIRE(detail::VisitSourceVfxParticles(context, &value, [&](const ParticleData2D &particle) {
		positions.push_back(particle.State.Position);
		return true;
	}));
	REQUIRE(positions.size() == 2);
	CHECK(positions[0] == std::array<double, 2>{2, 3});
	CHECK(positions[1] == std::array<double, 2>{5, 7});
	std::get<std::vector<SourceArrayItem>>(pools.Items[0].Data)[0].Data =
		std::vector<SourceArrayItem>{{ElementValue{first}}};
	value = pools;
	positions.clear();
	CHECK_FALSE(detail::VisitSourceVfxParticles(context, &value, [&](const ParticleData2D &particle) {
		positions.push_back(particle.State.Position);
		return true;
	}));
	CHECK(context.FailureCode == Status::InvalidValue);
	CHECK(positions.empty());
}
