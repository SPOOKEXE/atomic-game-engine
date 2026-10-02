#include "../src/ParticleCodec.hpp"
#include "../src/ValuePayload.hpp"
#include "NodeHarness.hpp"

#include <engine/imagegraph/Particle.hpp>
#include <engine/imagegraph/SimulationReplay.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <limits>
#include <sstream>

TEST_SUITE_ID("engine.imagegraph.particle")
using namespace engine::imagegraph;
namespace {
	ParticleValue Particle() {
		ParticleValue value;
		auto &data = value.Data.emplace();
		data.OriginNodeId = "pool \\\"quoted";
		data.OriginProcessorRow = 3;
		data.State.Active = true;
		data.State.Position = {1.25, -2.5};
		data.State.Scale = {2, .5};
		data.State.RotationDegrees = 45;
		data.State.Alpha = .75;
		data.State.Blend = 0x00030201;
		data.State.SourceSlot = 2;
		data.State.XHistory = {std::nullopt, 1.25};
		data.State.YHistory = {-2.5, std::nullopt};
		data.State.SpriteSlot = 0;
		data.Sprites.push_back(Image{1, 1, {1, 2, 3, 4}, 55});
		return value;
	}
	Document ParticleGraph() {
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
			{"step", "pc.flip_update"},
			{"vfx", "pc.flip_to_vfx"},
			{"select", "pc.array_get", "", {}, {{"index", int64_t{1}}}},
			{"length", "pc.array_length"}
		};
		document.Links = {
			{"domain", "domain", "fill", "domain"},
			{"fill", "domain", "step", "domain"},
			{"step", "domain", "vfx", "domain"},
			{"vfx", "particles", "select", "array"},
			{"vfx", "particles", "length", "array"}
		};
		document.Outputs = {
			{"particles", "vfx", "particles"}, {"selected", "select", "value"}, {"count", "length", "size"}
		};
		return document;
	}
}
TEST_CASE(
	"Source particle snapshots own histories and sprites without widening value layout", "[imagegraph]"
) {
	static_assert(std::variant_size_v<Value> == 35);
	static_assert(std::is_same_v<std::variant_alternative_t<34, Value>, ParticleValue>);
	static_assert(sizeof(Value) == 88 && sizeof(ParticleValue) == 8);
	auto original = Particle(), copy = original;
	copy.Data->State.XHistory[1] = 9;
	copy.Data->Sprites[0].Pixels[0] = 88;
	CHECK(original.Data->State.XHistory[1] == 1.25);
	CHECK(original.Data->Sprites[0].Pixels[0] == 1);
	CHECK(detail::PayloadType(Value{original}) == ValueType::Particle);
	CHECK(
		detail::ParticleStorageBytes<true>(original) >=
		sizeof(ParticleData2D) + 4 * sizeof(std::optional<double>)
	);
	REQUIRE(detail::ValidParticlePayload(original));
	original.Data->State.Alpha = std::numeric_limits<double>::quiet_NaN();
	CHECK_FALSE(detail::ValidParticlePayload(original));
	original = Particle();
	original.Data->State.SpriteSlot = 1;
	CHECK_FALSE(detail::ValidParticlePayload(original));
	original = Particle();
	original.Data->OriginProcessorRow = Limits::MaximumArrayElements;
	CHECK_FALSE(detail::ValidParticlePayload(original));
	original = Particle();
	original.Data->State.YHistory.clear();
	CHECK_FALSE(detail::ValidParticlePayload(original));
}
TEST_CASE(
	"Particle native codec roundtrips every owned surface format and sparse history atomically",
	"[imagegraph]"
) {
	auto original = Particle();
	original.Data->Sprites.clear();
	const std::array formats{
		SurfaceFormat::RGBA8Unorm,
		SurfaceFormat::RGBA4Unorm,
		SurfaceFormat::RGBA16Float,
		SurfaceFormat::RGBA32Float,
		SurfaceFormat::R8Unorm,
		SurfaceFormat::R16Float,
		SurfaceFormat::R32Float
	};
	for (const auto format : formats) {
		Image sprite;
		sprite.Width = sprite.Height = 1;
		sprite.Format = format;
		sprite.Pixels.resize(DescribeSurfaceFormat(format)->BytesPerPixel);
		REQUIRE(StoreSurfacePixel(sprite, 0, 0, {.25, .5, .75, 1}));
		sprite.Hash = SurfaceHash(sprite);
		original.Data->Sprites.push_back(std::move(sprite));
	}
	std::stringstream stream;
	stream.precision(17);
	detail::WriteParticleValue(stream, original);
	ParticleValue restored;
	uint64_t charged = 0;
	REQUIRE(detail::ReadParticleValue(stream, restored, [&](uint64_t bytes) {
		charged += bytes;
		return charged <= Limits::MaximumEvaluationBytes;
	}));
	CHECK(restored == original);
	const auto previous = restored;
	std::stringstream malformed("1 0 0 \"\" 65537");
	CHECK_FALSE(detail::ReadParticleValue(malformed, restored, [](uint64_t) { return true; }));
	CHECK(restored == previous);
	std::stringstream invalidRow;
	auto badRow = original;
	badRow.Data->OriginProcessorRow = Limits::MaximumArrayElements;
	detail::WriteParticleValue(invalidRow, badRow);
	CHECK_FALSE(detail::ReadParticleValue(invalidRow, restored, [](uint64_t) { return true; }));
	CHECK(restored == previous);
	std::stringstream denied;
	detail::WriteParticleValue(denied, original);
	CHECK_FALSE(detail::ReadParticleValue(denied, restored, [](uint64_t) { return false; }));
	CHECK(restored == previous);
}
TEST_CASE(
	"Particle arrays retain individual object semantics through native document and generic selection",
	"[imagegraph]"
) {
	ArrayValue particles{ValueType::Particle, {Particle(), Particle()}};
	std::get<ParticleValue>(particles.Elements[1]).Data->State.Position[0] = 9;
	Document document;
	document.FormatVersion = 9;
	document.Nodes = {{"snapshot", "vendor.particle_snapshot", "", {}, {{"particles", particles}}}};

	Diagnostic diagnostic;
	Document restored;
	REQUIRE(Read(Write(document), restored, diagnostic) == Status::Ok);
	CHECK(restored.Nodes[0].Values == document.Nodes[0].Values);
	const auto run = imagegraph_test::RunNode(
		"pc.array_get", {}, {{"array", restored.Nodes[0].Values[0].Data}, {"index", int64_t{1}}}
	);
	INFO(run.Message);
	REQUIRE(run.Ok);
	CHECK(detail::PayloadType(*run.OutputValue("value")) == ValueType::Particle);
	CHECK(std::get<ParticleValue>(*run.OutputValue("value")).Data->State.Position[0] == 9);
	ArrayValue nested{ValueType::Any, {}};
	nested.Items = {{std::vector<SourceArrayItem>{{ElementValue{Particle()}}}}};
	const auto selected =
		imagegraph_test::RunNode("pc.array_get", {}, {{"array", nested}, {"index", int64_t{0}}});
	INFO(selected.Message);
	REQUIRE(selected.Ok);
	const auto &row = std::get<ArrayValue>(*selected.OutputValue("value"));
	CHECK(row.ElementType == ValueType::Particle);
	CHECK(row.Items.empty());
	CHECK(row.Nested.empty());
	REQUIRE(row.Elements.size() == 1);
	CHECK(std::get<ParticleValue>(row.Elements[0]) == Particle());
}
TEST_CASE(
	"Actual FLIP-to-VFX graph uses source readbacks and transports individual particles to array nodes",
	"[imagegraph]"
) {
	const auto document = ParticleGraph();
	Plan plan;
	Diagnostic diagnostic;
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	EvaluationRequest request;
	request.SimulationAuthoringRevision = 5;
	SimulationEvaluationResult particles;
	const auto status = EvaluateSimulation(document, plan, "particles", request, particles, diagnostic);
	INFO(diagnostic.Message);
	REQUIRE(status == Status::Ok);
	const auto &array = std::get<ArrayValue>(std::get<EvaluatedValue>(particles.Output).Data);
	CHECK(array.ElementType == ValueType::Particle);
	REQUIRE(array.Elements.size() == 4);
	const auto &first = std::get<ParticleValue>(array.Elements[0]);
	CHECK(first.Data->State.Position[0] == 6);
	CHECK(first.Data->State.Position[1] == Catch::Approx(6.15));
	CHECK(first.Data->State.Active);
	CHECK(first.Data->State.SourceSlot == 0);
	CHECK(first.Data->OriginNodeId == "vfx");
	SimulationEvaluationResult selected;
	REQUIRE(EvaluateSimulation(document, plan, "selected", request, selected, diagnostic) == Status::Ok);
	const auto &particle = std::get<ParticleValue>(std::get<EvaluatedValue>(selected.Output).Data);
	CHECK(particle.Data->State.Position[0] == 14);
	CHECK(particle.Data->State.SourceSlot == 1);
	SimulationEvaluationResult length;
	REQUIRE(EvaluateSimulation(document, plan, "count", request, length, diagnostic) == Status::Ok);
	CHECK(std::get<int64_t>(std::get<EvaluatedValue>(length.Output).Data) == 4);
}
