#include "../src/MeshPayload.hpp"
#include "../src/ProcessorBatch.hpp"
#include "NodeHarness.hpp"

#include <engine/testing/Suite.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <numbers>
TEST_SUITE_ID("engine.imagegraph.mesh_cylinder")
using namespace engine::imagegraph;
using imagegraph_test::RunNode;
namespace {
	constexpr std::string_view CYLINDER = "pc.3_d_mesh_cylinder";
	MeshValue3D Mesh(std::vector<AuthoredValue> values = {}) {
		const auto *entry = FindCatalogueEntry(CYLINDER);
		REQUIRE(entry);
		const Node node{"cylinder", std::string(CYLINDER), "", {}, {}};
		const EvaluationRequest request;
		detail::NodeContext context(node, *entry, request);
		context.ByteBudget = Limits::MaximumEvaluationBytes;
		for (const auto &input : entry->Inputs) {
			const auto found =
				std::find_if(values.begin(), values.end(), [&](const auto &v) { return v.Port == input.Id; });
			if (found != values.end())
				context.Values.emplace_back(found->Port, found->Data);
			else if (auto value = CatalogueDefault(input))
				context.Values.emplace_back(input.Id, std::move(*value));
		}
		const bool okay = detail::RunProcessorBatch(context, detail::FindExecutor(CYLINDER));
		INFO(context.FailureMessage);
		REQUIRE(okay);
		REQUIRE(context.OutputValues.size() == 1);
		return std::get<MeshValue3D>(context.OutputValues[0].Data);
	}
	Curve Profile() {
		Curve curve;
		curve.Header = {0, 1, 0, 0, 1, 0};
		curve.Anchors = {{{0, 0, 0, 1, 0, 0}}, {{0, 0, .5, 2, 0, 0}}, {{0, 0, 1, .5, 0, 0}}};
		return curve;
	}
	Value Replay(const Document &doc) {
		Document restored;
		Diagnostic d;
		REQUIRE(Read(Write(doc), restored, d) == Status::Ok);
		CHECK(restored == doc);
		Plan plan;
		const auto code = Compile(restored, plan, d);
		INFO(d.Message);
		REQUIRE(code == Status::Ok);
		EvaluatedValue v;
		const auto evaluated = EvaluateValue(restored, plan, "out", {}, v, d);
		INFO(d.NodeId << ':' << d.Port << ' ' << d.Message);
		REQUIRE(evaluated == Status::Ok);
		return std::move(v.Data);
	}
}
TEST_CASE(
	"Cylinder default owns ordered side top bottom geometry and source cap edges",
	"[imagegraph][mesh_cylinder]"
) {
	const auto mesh = Mesh();
	const auto &d = *mesh.Data;
	REQUIRE(d.Parts.size() == 3);
	CHECK(d.Parts[0].Vertices.size() == 48);
	CHECK(d.Parts[1].Vertices.size() == 24);
	CHECK(d.Parts[2].Vertices.size() == 24);
	CHECK(d.Edges.size() == 32);
	CHECK(d.Materials.size() == 3);
	CHECK(d.LocalTransforms == std::vector<MeshTransform3D>{MeshTransform3D{}});
	const auto &side = d.Parts[0].Vertices;
	CHECK((side[0].Position == Vector3{.5, 0, .5}));
	CHECK((side[1].Position == Vector3{.5, 0, -.5}));
	CHECK((side[0].UV == Vector2{0, 1}));
	CHECK((side[1].UV == Vector2{0, 0}));
	CHECK(side[2].Position.X == Catch::Approx(std::sqrt(.125)));
	CHECK(side[2].Position.Y == Catch::Approx(-std::sqrt(.125)));
	CHECK(side[0].Normal.X == Catch::Approx(std::cos(std::numbers::pi / 8)));
	CHECK(side[0].Normal.Y == Catch::Approx(-std::sin(std::numbers::pi / 8)));
	CHECK(side[0].Normal.Z == 0);
	CHECK((d.Parts[1].Vertices[0] == MeshVertex3D{{0, 0, .5}, {0, 0, 1}, {.5, .5}}));
	CHECK((d.Parts[1].Vertices[1].UV == Vector2{1, .5}));
	CHECK((d.Parts[2].Vertices[2].Position == Vector3{.5, 0, -.5}));
	CHECK(d.Parts[2].Vertices[1].Normal.Z == -1);
	CHECK((d.Edges[0].From == Vector3{.5, 0, .5}));
	CHECK((d.Edges[1].From == Vector3{.5, 0, -.5}));
	CHECK((d.Edges[16] == MeshEdge3D{{.5, 0, -.5}, {.5, 0, .5}}));
}
TEST_CASE(
	"Cylinder caps toggle hides triangle parts while preserving unconditional source edges",
	"[imagegraph][mesh_cylinder]"
) {
	const auto full = Mesh({{"side", int64_t{4}}, {"segments", int64_t{3}}}),
			   open = Mesh({{"side", int64_t{4}}, {"segments", int64_t{3}}, {"end_caps", false}});
	CHECK(full.Data->Parts.size() == 3);
	REQUIRE(open.Data->Parts.size() == 1);
	CHECK(open.Data->Parts[0] == full.Data->Parts[0]);
	CHECK(open.Data->Edges == full.Data->Edges);
	CHECK(open.Data->Materials == full.Data->Materials);
	CHECK(open.Data->Parts[0].Vertices.size() == 72);
	CHECK(open.Data->Edges.size() == 32);
}
TEST_CASE(
	"Cylinder ordinary Int getters preserve min validators and real half even capture semantics",
	"[imagegraph][mesh_cylinder]"
) {
	CHECK(Mesh({{"side", 3.5}, {"segments", 2.5}}).Data->Parts[0].Vertices.size() == 48);
	CHECK(Mesh({{"side", 4.5}, {"segments", 3.5}}).Data->Parts[0].Vertices.size() == 96);
	CHECK(Mesh({{"side", -100.5}, {"segments", -.5}}).Data->Parts[0].Vertices.size() == 18);
	const auto cap = RunNode(CYLINDER, {}, {{"side", int64_t{4096}}, {"segments", int64_t{4096}}});
	CHECK_FALSE(cap.Ok);
	CHECK(cap.Code == Status::LimitExceeded);
}
TEST_CASE(
	"Cylinder profile sampling and smooth normals retain exact source component mixing",
	"[imagegraph][mesh_cylinder]"
) {
	const auto smooth = Mesh(
				   {{"side", int64_t{4}},
					{"segments", int64_t{2}},
					{"profile", Profile()},
					{"smooth_side", true}}
			   ),
			   flat = Mesh({{"side", int64_t{4}}, {"segments", int64_t{2}}, {"profile", Profile()}});
	const auto &v = smooth.Data->Parts[0].Vertices;
	CHECK((v[0].Position == Vector3{1, 0, 0}));
	CHECK((v[1].Position == Vector3{.5, 0, -.5}));
	CHECK((v[6].Position == Vector3{.25, 0, .5}));
	CHECK(smooth.Data->Parts[1].Vertices[1].Position.X == .25);
	CHECK(smooth.Data->Parts[2].Vertices[2].Position.X == .5);
	CHECK(v[0].Normal.X == Catch::Approx(1 / std::sqrt(2)));
	CHECK(v[0].Normal.Z == Catch::Approx(-1 / std::sqrt(17)));
	CHECK(v[1].Normal.Z == Catch::Approx(1 / std::sqrt(2)));
	CHECK(v[4].Normal.Y == Catch::Approx(-4 / std::sqrt(17)));
	CHECK(v[4].Normal.Z == Catch::Approx(1 / std::sqrt(2)));
	CHECK(flat.Data->Parts[0].Vertices[0].Normal.X == Catch::Approx(.5));
	CHECK(flat.Data->Parts[0].Vertices[0].Normal.Y == Catch::Approx(-.5));
	CHECK(flat.Data->Parts[0].Vertices[0].Normal.Z == Catch::Approx(1 / std::sqrt(2)));
	Curve invalid = Profile();
	invalid.Header[1] = std::numeric_limits<double>::quiet_NaN();
	const auto bad = RunNode(CYLINDER, {}, {{"profile", invalid}});
	CHECK_FALSE(bad.Ok);
	CHECK(bad.Code == Status::InvalidValue);
	CHECK(bad.Port == "profile");
}
TEST_CASE(
	"Cylinder three materials clone format preserving owned float samples independently",
	"[imagegraph][mesh_cylinder]"
) {
	const std::array<std::string_view, 3> ports{"material_side", "material_top", "material_bottom"};
	std::array<MaterialValue3D, 3> materials;
	std::vector<AuthoredValue> values;
	for (size_t i = 0; i < 3; ++i) {
		auto &d = materials[i].Edit();
		d.Diffuse = double(i + 1);
		d.Surface = Image{1, 1, std::vector<uint8_t>(16), 0, SurfaceFormat::RGBA32Float};
		REQUIRE(StoreSurfacePixel(*d.Surface, 0, 0, SurfacePixel{-2, double(i) + 3, .25, 1}));
		d.Surface->Pixels.reserve(256);
		values.push_back({std::string(ports[i]), materials[i]});
	}
	const auto mesh = Mesh(values);
	REQUIRE(mesh.Data->Materials.size() == 3);
	for (size_t i = 0; i < 3; ++i) {
		CHECK(mesh.Data->Parts[i].MaterialIndex == i);
		CHECK(mesh.Data->Materials[i] == materials[i]);
		CHECK(
			mesh.Data->Materials[i].Get().Surface->Pixels.data() != materials[i].Get().Surface->Pixels.data()
		);
	}
}
TEST_CASE(
	"Persisted explicit Material Cylinder Transform GetData route preserves every local control",
	"[imagegraph][mesh_cylinder]"
) {
	Document doc;
	doc.FormatVersion = 9;
	doc.Nodes = {
		{"material", "pc.3_d_material", "", {}, {{"diffuse", .25}}},
		{"cylinder",
		 std::string(CYLINDER),
		 "",
		 {},
		 {{"side", int64_t{4}},
		  {"segments", int64_t{2}},
		  {"profile", Profile()},
		  {"smooth_side", true},
		  {"end_caps", false},
		  {"position", Vector3{2, 3, 4}},
		  {"anchor", Vector3{.1, .2, .3}},
		  {"rotation", Quaternion{.1, .2, .3, .4}},
		  {"scale", Vector3{2, -3, .5}}}},
		{"transform", "pc.3_d_transform", "", {}, {{"position", Vector3{-1, 2, 3}}}},
		{"get", "pc.3_d_get_data", "", {}, {}}
	};
	doc.Links = {
		{"material", "material", "cylinder", "material_side"},
		{"cylinder", "mesh", "transform", "mesh"},
		{"transform", "mesh", "get", "mesh"}
	};
	doc.Outputs = {{"out", "transform", "mesh"}};
	const auto mesh = std::get<MeshValue3D>(Replay(doc));
	REQUIRE(mesh.Data->LocalTransforms.size() == 2);
	CHECK((mesh.Data->LocalTransforms[0].Position == Vector3{-1, 2, 3}));
	CHECK((
		mesh.Data->LocalTransforms[1] ==
		MeshTransform3D{Vector3{2, 3, 4}, Vector3{.1, .2, .3}, Quaternion{.1, .2, .3, .4}, Vector3{2, -3, .5}}
	));
	CHECK(mesh.Data->Parts.size() == 1);
	CHECK(mesh.Data->Materials[0].Get().Diffuse == .25);
	doc.Outputs = {{"out", "get", "position"}};
	CHECK((Replay(doc) == Value{Vector3{-1, 2, 3}}));
}
TEST_CASE(
	"Persisted Cylinder arrays use source twelve slot schedules including inverse suffix quirks",
	"[imagegraph][mesh_cylinder]"
) {
	const std::array<std::array<size_t, 6>, 4> sideRows{
		{{0, 1, 0, 0, 0, 0}, {0, 1, 1, 0, 0, 0}, {0, 0, 0, 1, 1, 1}, {0, 0, 0, 1, 1, 1}}
	},
		segmentRows{{{0, 1, 2, 0, 0, 0}, {0, 1, 2, 0, 0, 0}, {0, 1, 2, 0, 1, 2}, {0, 0, 0, 0, 0, 0}}};
	for (int64_t mode : {0, 1, 2, 3}) {
		Document doc;
		doc.FormatVersion = 9;
		doc.Nodes = {
			{"cylinder",
			 std::string(CYLINDER),
			 "",
			 {},
			 {{"side", ArrayValue{ValueType::Integer, {int64_t{3}, int64_t{4}}}},
			  {"segments", ArrayValue{ValueType::Integer, {int64_t{1}, int64_t{2}, int64_t{3}}}},
			  {"attribute_array_process", EnumValue{mode}}}}
		};
		doc.Outputs = {{"out", "cylinder", "mesh"}};
		const auto rows = std::get<ArrayValue>(Replay(doc));
		const size_t count = mode < 2 ? 3 : 6;
		REQUIRE(rows.Elements.size() == count);
		for (size_t i = 0; i < count; ++i) {
			INFO("mode=" << mode << " row=" << i);
			const auto &mesh = std::get<MeshValue3D>(rows.Elements[i]);
			const size_t sides = 3 + sideRows[size_t(mode)][i], segments = 1 + segmentRows[size_t(mode)][i];
			CHECK(mesh.Data->Parts[0].Vertices.size() == 6 * sides * segments);
			CHECK(mesh.Data->Parts[1].Vertices.size() == 3 * sides);
			CHECK(mesh.Data->Edges.size() == 2 * sides * (segments + 1));
		}
	}
}
TEST_CASE(
	"Cylinder shared live ledger admits profile scratch and retained material clone overlap atomically",
	"[imagegraph][mesh_cylinder]"
) {
	MaterialValue3D material;
	material.Edit().Surface = Image{1, 1, {1, 2, 3, 255}};
	material.Edit().Surface->Pixels.reserve(512);
	const Value input = material;
	const auto *entry = FindCatalogueEntry(CYLINDER);
	REQUIRE(entry);
	const Node node{"cylinder", std::string(CYLINDER), "", {}, {}};
	const EvaluationRequest request;
	uint64_t peak = 0;
	const auto run = [&](uint64_t maximum, bool accepted) {
		detail::EvaluationBudget budget(maximum);
		auto prior = budget.Reserve(detail::RetainedPayloadBytes(input));
		REQUIRE(prior);
		{
			detail::NodeContext context(node, *entry, request, budget);
			context.ByteBudget = maximum;
			context.Values = {
				{"attribute_array_process", EnumValue{0}}, {"side", int64_t{4}}, {"segments", int64_t{2}}
			};
			for (const auto port : {"material_side", "material_top", "material_bottom"})
				context.ValueViews.emplace_back(port, &input);
			const bool okay = detail::RunProcessorBatch(context, detail::FindExecutor(CYLINDER));
			INFO(context.FailureMessage);
			CHECK(okay == accepted);
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
	"Persisted Cylinder profile arrays reach public Compile through the exact source Curve port",
	"[imagegraph][mesh_cylinder]"
) {
	const std::array<std::array<size_t, 6>, 4> curveRows{
		{{0, 1, 0, 0, 0, 0}, {0, 1, 1, 0, 0, 0}, {0, 1, 0, 1, 0, 1}, {0, 0, 0, 0, 0, 0}}
	},
		capsRows{{{0, 1, 2, 0, 0, 0}, {0, 1, 2, 0, 0, 0}, {0, 0, 1, 1, 2, 2}, {0, 0, 0, 0, 0, 0}}};
	for (int64_t mode : {0, 1, 2, 3}) {
		Document doc;
		doc.FormatVersion = 9;
		Node profiles{"profiles", "pc.array", "", {}, {{"type", EnumValue{0}}, {"spread_array", true}}};
		Curve constant = Profile();
		constant.Anchors[0][3] = constant.Anchors[1][3] = constant.Anchors[2][3] = 1;
		profiles.DynamicInputs = {
			{"input_0", ValueType::Curve, constant}, {"input_1", ValueType::Curve, Profile()}
		};
		doc.Nodes = {
			profiles,
			{"cylinder",
			 std::string(CYLINDER),
			 "",
			 {},
			 {{"side", int64_t{4}},
			  {"segments", int64_t{2}},
			  {"end_caps", ArrayValue{ValueType::Boolean, {true, false, true}}},
			  {"attribute_array_process", EnumValue{mode}}}}
		};
		doc.Links = {{"profiles", "array", "cylinder", "profile"}};
		doc.Outputs = {{"out", "cylinder", "mesh"}};
		const auto rows = std::get<ArrayValue>(Replay(doc));
		const size_t count = mode < 2 ? 3 : 6;
		REQUIRE(rows.Elements.size() == count);
		for (size_t i = 0; i < count; ++i) {
			const auto &mesh = std::get<MeshValue3D>(rows.Elements[i]);
			CHECK(mesh.Data->Parts.size() == (capsRows[size_t(mode)][i] == 1 ? 1 : 3));
			CHECK(mesh.Data->Parts[0].Vertices[0].Position.X == (curveRows[size_t(mode)][i] == 0 ? .5 : 1));
		}
	}
}
TEST_CASE(
	"Persisted Cylinder material rows retain source scheduling and independent owned descriptors",
	"[imagegraph][mesh_cylinder]"
) {
	const std::array<std::array<size_t, 6>, 4> sideRows{
		{{0, 1, 0, 0, 0, 0}, {0, 1, 1, 0, 0, 0}, {0, 0, 0, 1, 1, 1}, {0, 1, 0, 1, 0, 1}}
	},
		materialRows{{{0, 1, 2, 0, 0, 0}, {0, 1, 2, 0, 0, 0}, {0, 1, 2, 0, 1, 2}, {0, 1, 2, 0, 1, 2}}};
	for (int64_t mode : {0, 1, 2, 3}) {
		Document doc;
		doc.FormatVersion = 9;
		doc.Nodes = {
			{"material",
			 "pc.3_d_material",
			 "",
			 {},
			 {{"metalic", ArrayValue{ValueType::Scalar, {.1, .5, .9}}}}},
			{"cylinder",
			 std::string(CYLINDER),
			 "",
			 {},
			 {{"side", ArrayValue{ValueType::Integer, {int64_t{3}, int64_t{4}}}},
			  {"attribute_array_process", EnumValue{mode}}}}
		};
		doc.Links = {{"material", "material", "cylinder", "material_top"}};
		doc.Outputs = {{"out", "cylinder", "mesh"}};
		const auto rows = std::get<ArrayValue>(Replay(doc));
		const size_t count = mode < 2 ? 3 : 6;
		REQUIRE(rows.Elements.size() == count);
		const std::array<double, 3> metallic{.1, .5, .9};
		for (size_t i = 0; i < count; ++i) {
			const auto &mesh = std::get<MeshValue3D>(rows.Elements[i]);
			CHECK(mesh.Data->Parts[0].Vertices.size() == 6 * (3 + sideRows[size_t(mode)][i]));
			CHECK(mesh.Data->Materials[1].Get().MetallicRange.X == metallic[materialRows[size_t(mode)][i]]);
			CHECK(mesh.Data->Materials[1].Get().MetallicRange.Y == metallic[materialRows[size_t(mode)][i]]);
		}
	}
}
