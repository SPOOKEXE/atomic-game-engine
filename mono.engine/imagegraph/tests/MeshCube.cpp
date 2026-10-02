#include "../src/MeshPayload.hpp"
#include "../src/ProcessorBatch.hpp"
#include "NodeHarness.hpp"

#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <limits>
TEST_SUITE_ID("engine.imagegraph.mesh_cube")
using namespace engine::imagegraph;
using imagegraph_test::RunNode;
namespace {
	constexpr std::string_view CUBE = "pc.3_d_mesh_cube";
	MeshValue3D Mesh(std::vector<AuthoredValue> values = {}) {
		const auto *entry = FindCatalogueEntry(CUBE);
		REQUIRE(entry);
		const Node node{"cube", std::string(CUBE), "", {}, {}};
		const EvaluationRequest request;
		detail::NodeContext context(node, *entry, request);
		context.ByteBudget = Limits::MaximumEvaluationBytes;
		for (const auto &input : entry->Inputs) {
			const auto found =
				std::find_if(values.begin(), values.end(), [&](const auto &v) { return v.Port == input.Id; });
			if (found != values.end())
				context.Values.emplace_back(found->Port, found->Data);
			else if (auto value = CatalogueDefault(input))
				context.Values.push_back({std::string(input.Id), std::move(*value)});
		}
		INFO(context.FailureMessage);
		REQUIRE(detail::RunProcessorBatch(context, detail::FindExecutor(CUBE)));
		REQUIRE(context.OutputValues.size() == 1);
		return std::get<MeshValue3D>(context.OutputValues[0].Data);
	}
	Value Replay(const Document &doc) {
		Document restored;
		Diagnostic d;
		REQUIRE(Read(Write(doc), restored, d) == Status::Ok);
		CHECK(restored == doc);
		Plan plan;
		INFO(d.Message);
		REQUIRE(Compile(restored, plan, d) == Status::Ok);
		EvaluatedValue v;
		REQUIRE(EvaluateValue(restored, plan, "out", {}, v, d) == Status::Ok);
		return std::move(v.Data);
	}
}
TEST_CASE(
	"Cube default preserves source face concatenation winding UVs and twelve edges", "[imagegraph][mesh_cube]"
) {
	const auto mesh = Mesh();
	REQUIRE(mesh.Data);
	const auto &data = *mesh.Data;
	REQUIRE(data.Parts.size() == 1);
	REQUIRE(data.Parts[0].Vertices.size() == 36);
	REQUIRE(data.Edges.size() == 12);
	CHECK(data.Materials.size() == 6);
	CHECK(data.LocalTransforms == std::vector<MeshTransform3D>{MeshTransform3D{}});
	const std::array<Vector3, 6> normals{
		{{-1, 0, 0}, {1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}}
	};
	const std::array<Vector3, 6> first{
		{{-.5, -.5, .5}, {.5, -.5, .5}, {-.5, .5, .5}, {-.5, -.5, .5}, {-.5, -.5, .5}, {-.5, -.5, -.5}}
	};
	const std::array<Vector2, 6> uv{{{1, 0}, {0, 0}, {1, 0}, {0, 0}, {1, 1}, {1, 1}}};
	for (size_t face = 0; face < 6; ++face) {
		CHECK(data.Parts[0].Vertices[face * 6].Position == first[face]);
		CHECK(data.Parts[0].Vertices[face * 6].UV == uv[face]);
		for (size_t i = 0; i < 6; ++i) {
			const auto &v = data.Parts[0].Vertices[face * 6 + i];
			CHECK(v.Normal == normals[face]);
			CHECK((v.Tint == Colour{255, 255, 255, 255}));
		}
	}
	const std::array<Vector3, 8> corners{
		{{-.5, -.5, -.5},
		 {.5, -.5, -.5},
		 {-.5, .5, -.5},
		 {.5, .5, -.5},
		 {-.5, -.5, .5},
		 {.5, -.5, .5},
		 {-.5, .5, .5},
		 {.5, .5, .5}}
	};
	const std::array<size_t, 36> positions{4, 2, 6, 4, 0, 2, 5, 7, 3, 5, 3, 1, 6, 3, 7, 6, 2, 3,
										   4, 5, 1, 4, 1, 0, 4, 7, 5, 4, 6, 7, 0, 1, 3, 0, 3, 2};
	for (size_t i = 0; i < positions.size(); ++i)
		CHECK(data.Parts[0].Vertices[i].Position == corners[positions[i]]);
	CHECK((data.Edges[0] == MeshEdge3D{{-.5, -.5, -.5}, {.5, -.5, -.5}}));
	CHECK((data.Edges[11] == MeshEdge3D{{.5, .5, -.5}, {.5, .5, .5}}));
}
TEST_CASE(
	"Cube asymmetric and fractional subdivisions retain raw denominators and minimum one",
	"[imagegraph][mesh_cube]"
) {
	const auto mesh = Mesh({{"subdivision", Vector3{2, 3, 4}}, {"material_mode", EnumValue{1}}});
	const std::array<size_t, 6> expected{36, 36, 72, 72, 48, 48};
	REQUIRE(mesh.Data->Parts.size() == 6);
	for (size_t i = 0; i < 6; ++i)
		CHECK(mesh.Data->Parts[i].Vertices.size() == expected[i]);
	CHECK(Mesh({{"subdivision", Vector3{0, -2, 1}}}).Data->Parts[0].Vertices.size() == 36);
	const auto fractional = Mesh({{"subdivision", Vector3{1.5, 2.5, 1}}, {"material_mode", EnumValue{1}}});
	CHECK(fractional.Data->Parts[0].Vertices.size() == 36);
	CHECK(fractional.Data->Parts[2].Vertices.size() == 18);
	CHECK(fractional.Data->Parts[4].Vertices.size() == 12);
	// Z+ face's final cell ends at raw u=2/1.5 and v=3/2.5, not one.
	CHECK((fractional.Data->Parts[0].Vertices[30].UV == Vector2{2.0 / 1.5, 3.0 / 2.5}));
	CHECK((fractional.Data->Parts[0].Vertices[30].Position == Vector3{.5 - 2.0 / 1.5, .5 - 3.0 / 2.5, .5}));
	const auto nonRounded = Mesh({{"subdivision", Vector3{1.25, 2.5, 3.5}}});
	CHECK(nonRounded.Data->Parts[0].Vertices.size() == 312);
	const auto cap = RunNode(CUBE, {}, {{"subdivision", Vector3{4096, 4096, 4096}}});
	CHECK_FALSE(cap.Ok);
	CHECK(cap.Code == Status::LimitExceeded);
}
TEST_CASE(
	"Cube taper changes perpendicular corners on all axes while source normals remain axial",
	"[imagegraph][mesh_cube]"
) {
	for (int64_t axis = 0; axis < 3; ++axis)
		for (double amount : {-1.0, -.5, .5, 1.0}) {
			const auto mesh = Mesh({{"taper", amount}, {"taper_axis", EnumValue{axis}}});
			std::array<double, 3> lower{-.5, -.5, -.5};
			for (size_t j = 0; j < 3; ++j)
				if (j != size_t(axis)) lower[j] = (-1 + amount) / 2;
			CHECK((mesh.Data->Edges[0].From == Vector3{lower[0], lower[1], lower[2]}));
			CHECK((mesh.Data->Parts[0].Vertices[0].Normal == Vector3{-1, 0, 0}));
		}
}
TEST_CASE(
	"Cube material modes preserve source six descriptors and deep owned float textures",
	"[imagegraph][mesh_cube]"
) {
	const std::array<std::string_view, 6> names{
		"material", "material_bottom", "material_left", "material_right", "material_back", "material_front"
	};
	std::array<MaterialValue3D, 6> materials;
	for (size_t i = 0; i < 6; ++i) {
		auto &d = materials[i].Edit();
		d.Diffuse = double(i + 1);
		d.Surface = Image{1, 1, std::vector<uint8_t>(16), 0, SurfaceFormat::RGBA32Float};
		REQUIRE(StoreSurfacePixel(*d.Surface, 0, 0, SurfacePixel{-2, double(i) + 2, .25, 1}));
		d.Surface->Pixels.reserve(128);
	}
	for (int64_t mode : {0, 1, 2}) {
		std::vector<AuthoredValue> values{{"material_mode", EnumValue{mode}}};
		for (size_t i = 0; i < 6; ++i)
			values.push_back({std::string(names[i]), materials[i]});
		const auto mesh = Mesh(std::move(values));
		CHECK(mesh.Data->Parts.size() == (mode == 0 ? 1 : 6));
		REQUIRE(mesh.Data->Materials.size() == 6);
		for (size_t i = 0; i < 6; ++i) {
			const size_t source = mode == 2 ? (i < 2 ? 0 : 1) : i;
			CHECK(mesh.Data->Materials[i] == materials[source]);
			CHECK(
				mesh.Data->Materials[i].Data->Surface->Pixels.data() !=
				materials[source].Data->Surface->Pixels.data()
			);
		}
		for (size_t i = 0; i < mesh.Data->Parts.size(); ++i)
			CHECK(mesh.Data->Parts[i].MaterialIndex == i);
	}
}
TEST_CASE(
	"Persisted Material cube Transform and GetData recompute nonidentity local chain",
	"[imagegraph][mesh_cube]"
) {
	Document doc;
	doc.FormatVersion = 9;
	doc.Nodes = {
		{"material", "pc.3_d_material", "", {}, {{"diffuse", .25}}},
		{"cube",
		 std::string(CUBE),
		 "",
		 {},
		 {{"position", Vector3{2, 3, 4}},
		  {"anchor", Vector3{.1, .2, .3}},
		  {"rotation", Quaternion{.1, .2, .3, .4}},
		  {"scale", Vector3{2, -3, .5}},
		  {"subdivision", Vector3{2, 1, 3}},
		  {"taper", .5},
		  {"taper_axis", EnumValue{1}}}},
		{"transform", "pc.3_d_transform", "", {}, {{"position", Vector3{-1, 2, 3}}}},
		{"get", "pc.3_d_get_data", "", {}, {}}
	};
	doc.Links = {
		{"material", "material", "cube", "material"},
		{"cube", "mesh", "transform", "mesh"},
		{"transform", "mesh", "get", "mesh"}
	};
	doc.Outputs = {{"out", "transform", "mesh"}};
	const auto mesh = std::get<MeshValue3D>(Replay(doc));
	REQUIRE(mesh.Data->LocalTransforms.size() == 2);
	CHECK((mesh.Data->LocalTransforms[0].Position == Vector3{-1, 2, 3}));
	CHECK((mesh.Data->LocalTransforms[1].Position == Vector3{2, 3, 4}));
	CHECK(mesh.Data->Materials[0].Get().Diffuse == .25);
	doc.Outputs = {{"out", "get", "position"}};
	CHECK((Replay(doc) == Value{Vector3{-1, 2, 3}}));
}
TEST_CASE(
	"Persisted cube schedules linked vector rows in all four processor modes", "[imagegraph][mesh_cube]"
) {
	// The source inverse reverses all 14 constructor slots, including scalar slots.
	// Taper 11 and Subdivision 13 both read a suffix product of 6 after that reversal.
	const std::array<std::array<size_t, 6>, 4> subdivisionRows{
		{{0, 1, 2, 0, 0, 0}, {0, 1, 2, 0, 0, 0}, {0, 1, 2, 0, 1, 2}, {0, 0, 0, 0, 0, 0}}
	};
	const std::array<std::array<size_t, 6>, 4> taperRows{
		{{0, 1, 0, 0, 0, 0}, {0, 1, 1, 0, 0, 0}, {0, 0, 0, 1, 1, 1}, {0, 0, 0, 0, 0, 0}}
	};
	for (int64_t mode : {0, 1, 2, 3}) {
		Document doc;
		doc.FormatVersion = 9;
		doc.Nodes = {
			{"dimensions",
			 "pc.vector3",
			 "",
			 {},
			 {{"x", ArrayValue{ValueType::Scalar, {1.0, 2.0, 3.0}}}, {"y", 1.0}, {"z", 1.0}}},
			{"cube",
			 std::string(CUBE),
			 "",
			 {},
			 {{"attribute_array_process", EnumValue{mode}},
			  {"taper", ArrayValue{ValueType::Scalar, {-.5, .5}}}}}
		};
		doc.Links = {{"dimensions", "vector", "cube", "subdivision"}};
		doc.Outputs = {{"out", "cube", "mesh"}};
		const auto rows = std::get<ArrayValue>(Replay(doc));
		const size_t size = mode < 2 ? 3 : 6;
		REQUIRE(rows.Elements.size() == size);
		for (size_t i = 0; i < size; ++i) {
			INFO("mode=" << mode << " row=" << i);
			const size_t d = subdivisionRows[size_t(mode)][i];
			const size_t t = taperRows[size_t(mode)][i];
			const auto &mesh = std::get<MeshValue3D>(rows.Elements[i]);
			CHECK(mesh.Data->Parts[0].Vertices.size() == 36 + 24 * d);
			const double taper = t == 0 ? -.5 : .5;
			CHECK((mesh.Data->Edges[0].From == Vector3{-.5, (-1 + taper) / 2, (-1 + taper) / 2}));
		}
	}
}
TEST_CASE(
	"Cube admits retained material clones atomically beside prior live payload", "[imagegraph][mesh_cube]"
) {
	MaterialValue3D material;
	material.Edit().Surface = Image{1, 1, {1, 2, 3, 255}};
	material.Edit().Surface->Pixels.reserve(512);
	const Value input = material;
	const auto *entry = FindCatalogueEntry(CUBE);
	REQUIRE(entry);
	const Node node{"cube", std::string(CUBE), "", {}, {}};
	const EvaluationRequest request;
	uint64_t peak = 0;
	const auto run = [&](uint64_t maximum, bool accepted) {
		detail::EvaluationBudget budget(maximum);
		auto prior = budget.Reserve(detail::RetainedPayloadBytes(input));
		REQUIRE(prior);
		{
			detail::NodeContext context(node, *entry, request, budget);
			context.ByteBudget = maximum;
			context.Values = {{"attribute_array_process", EnumValue{0}}, {"material_mode", EnumValue{2}}};
			context.ValueViews.emplace_back("material", &input);
			context.ValueViews.emplace_back("material_bottom", &input);
			CHECK(detail::RunProcessorBatch(context, detail::FindExecutor(CUBE)) == accepted);
			if (accepted) {
				REQUIRE(context.OutputValues.size() == 1);
				CHECK(detail::ValidMeshPayload(std::get<MeshValue3D>(context.OutputValues[0].Data)));
				peak = budget.Peak();
			} else {
				CHECK(context.FailureCode == Status::LimitExceeded);
				CHECK(context.OutputValues.empty());
			}
		}
		CHECK(budget.Used() == prior->Bytes());
	};
	run(Limits::MaximumEvaluationBytes, true);
	const auto exact = peak;
	run(exact, true);
	run(exact - 1, false);
}

TEST_CASE(
	"Cube selector switch fallthrough retains source descriptors and explicit native truth policy",
	"[imagegraph][mesh_cube]"
) {
	for (double mode : {.25, .5, .75, 1.25, 1.75}) {
		const auto mesh = Mesh({{"material_mode", mode}});
		CHECK(mesh.Data->Parts.size() == (mode > .5 ? 6 : 1));
		CHECK(mesh.Data->Materials.size() == 6);
	}
	MaterialValue3D top, bottom;
	top.Edit().Diffuse = 2;
	bottom.Edit().Diffuse = 9;
	const auto fallthrough = Mesh({{"material_mode", 1.25}, {"material", top}, {"material_bottom", bottom}});
	CHECK(fallthrough.Data->Materials[0].Get().Diffuse == 2);
	CHECK(fallthrough.Data->Materials[1].Get().Diffuse == 9);
	CHECK(fallthrough.Data->Materials[2].Get().Diffuse == 1);
	const auto invalidSubdivision =
		RunNode(CUBE, {}, {{"subdivision", Vector3{1, std::numeric_limits<double>::quiet_NaN(), 1}}});
	CHECK_FALSE(invalidSubdivision.Ok);
	CHECK(invalidSubdivision.Code == Status::InvalidValue);
	CHECK(invalidSubdivision.Port == "subdivision");
	CHECK(Mesh({{"taper_axis", .5}, {"taper", 0.0}}) == Mesh());
	const auto fractionalAxis = RunNode(CUBE, {}, {{"taper_axis", .5}, {"taper", .5}});
	CHECK_FALSE(fractionalAxis.Ok);
	CHECK(fractionalAxis.Code == Status::UnsupportedExecution);
	CHECK(fractionalAxis.Port == "taper_axis");
	const auto nonfinite = RunNode(CUBE, {}, {{"taper", std::numeric_limits<double>::infinity()}});
	CHECK_FALSE(nonfinite.Ok);
	CHECK(nonfinite.Code == Status::InvalidValue);
}
