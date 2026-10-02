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

TEST_SUITE_ID("engine.imagegraph.mesh_cone")
using namespace engine::imagegraph;
using imagegraph_test::RunNode;
namespace {
	constexpr std::string_view CONE = "pc.3_d_mesh_cone";
	MeshValue3D Mesh(std::vector<AuthoredValue> values = {}) {
		const auto *entry = FindCatalogueEntry(CONE);
		REQUIRE(entry);
		const Node node{"cone", std::string(CONE), "", {}, {}};
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
		const bool okay = detail::RunProcessorBatch(context, detail::FindExecutor(CONE));
		INFO(context.FailurePort << ':' << context.FailureMessage);
		REQUIRE(okay);
		REQUIRE(context.OutputValues.size() == 1);
		return std::get<MeshValue3D>(context.OutputValues.front().Data);
	}
	Value Replay(const Document &doc) {
		Document restored;
		Diagnostic diagnostic;
		const std::string serialized = Write(doc);
		const auto read = Read(serialized, restored, diagnostic);
		INFO(diagnostic.NodeId << ':' << diagnostic.Port << ' ' << diagnostic.Message);
		INFO(serialized);
		REQUIRE(read == Status::Ok);
		CHECK(restored == doc);
		Plan plan;
		const auto compiled = Compile(restored, plan, diagnostic);
		INFO(diagnostic.Port << ':' << diagnostic.Message);
		REQUIRE(compiled == Status::Ok);
		EvaluatedValue output;
		const auto evaluated = EvaluateValue(restored, plan, "out", {}, output, diagnostic);
		INFO(diagnostic.NodeId << ':' << diagnostic.Port << ' ' << diagnostic.Message);
		REQUIRE(evaluated == Status::Ok);
		return std::move(output.Data);
	}
	void Point(Vector3 actual, Vector3 expected) {
		CHECK(actual.X == Catch::Approx(expected.X).margin(1e-12));
		CHECK(actual.Y == Catch::Approx(expected.Y).margin(1e-12));
		CHECK(actual.Z == Catch::Approx(expected.Z).margin(1e-12));
	}
	void Vertex(const MeshVertex3D &actual, Vector3 p, Vector3 n, Vector2 uv) {
		Point(actual.Position, p);
		Point(actual.Normal, n);
		CHECK(actual.UV.X == Catch::Approx(uv.X).margin(1e-12));
		CHECK(actual.UV.Y == Catch::Approx(uv.Y).margin(1e-12));
		CHECK((actual.Tint == Colour{255, 255, 255, 255}));
	}
	void Geometry(const MeshValue3D &mesh, size_t sides, bool smooth) {
		REQUIRE(mesh.Data);
		const auto &data = *mesh.Data;
		REQUIRE(data.Parts.size() == 2);
		REQUIRE(data.Materials.size() == 2);
		REQUIRE(data.Edges.size() == 2 * sides);
		for (size_t part = 0; part < 2; ++part) {
			CHECK(data.Parts[part].MaterialIndex == part);
			REQUIRE(data.Parts[part].Vertices.size() == 3 * sides);
		}
		// Independent angular oracle: source uses clockwise XY, fixed radius and unnormalized nz.
		for (size_t i = 0; i < sides; ++i) {
			const double a = 2 * std::numbers::pi * double(i) / double(sides),
						 b = 2 * std::numbers::pi * double(i + 1) / double(sides), midpoint = (a + b) / 2;
			const Vector3 p{.5 * std::cos(a), -.5 * std::sin(a), -.5},
				q{.5 * std::cos(b), -.5 * std::sin(b), -.5};
			const Vector3 middle{std::cos(midpoint), -std::sin(midpoint), .2},
				na = smooth ? Vector3{std::cos(a), -std::sin(a), .2} : middle,
				nb = smooth ? Vector3{std::cos(b), -std::sin(b), .2} : middle;
			const auto &bottom = data.Parts[0].Vertices;
			Vertex(bottom[3 * i], {0, 0, -.5}, {0, 0, -1}, {.5, .5});
			Vertex(bottom[3 * i + 1], q, {0, 0, -1}, {.5 + q.X, .5 + q.Y});
			Vertex(bottom[3 * i + 2], p, {0, 0, -1}, {.5 + p.X, .5 + p.Y});
			const auto &side = data.Parts[1].Vertices;
			Vertex(side[3 * i], {0, 0, .5}, middle, {(double(i) + .5) / double(sides), 0});
			Vertex(side[3 * i + 1], p, na, {double(i) / double(sides), 1});
			Vertex(side[3 * i + 2], q, nb, {double(i + 1) / double(sides), 1});
			Point(data.Edges[i].From, p);
			Point(data.Edges[i].To, q);
			Point(data.Edges[sides + i].From, p);
			Point(data.Edges[sides + i].To, {0, 0, .5});
		}
	}
}

TEST_CASE(
	"Cone defaults own bottom then side triangles and ring then apex edges", "[imagegraph][mesh_cone]"
) {
	const auto mesh = Mesh();
	Geometry(mesh, 8, false);
	CHECK(mesh.Data->LocalTransforms == std::vector<MeshTransform3D>{MeshTransform3D{}});
	CHECK(mesh.Data->Materials == std::vector<MaterialValue3D>(2));
	CHECK((mesh.Data->Parts[0].Vertices[2].Position == Vector3{.5, 0, -.5}));
	CHECK(mesh.Data->Parts[1].Vertices[0].Normal.Z == .2);
	CHECK(mesh.Data->Parts[1].Vertices[0].Normal.X == Catch::Approx(std::cos(std::numbers::pi / 8)));
}
TEST_CASE(
	"Cone every ordered vertex normal UV tint and edge follows flat and smooth source geometry",
	"[imagegraph][mesh_cone]"
) {
	for (int64_t sides : {3, 4, 9})
		for (bool smooth : {false, true}) {
			INFO("sides=" << sides << " smooth=" << smooth);
			Geometry(Mesh({{"side", sides}, {"smooth_side", smooth}}), size_t(sides), smooth);
		}
	const auto flat = Mesh({{"side", int64_t{4}}}),
			   smooth = Mesh({{"side", int64_t{4}}, {"smooth_side", true}});
	CHECK(flat.Data->Parts[0] == smooth.Data->Parts[0]);
	CHECK(flat.Data->Edges == smooth.Data->Edges);
	CHECK(flat.Data->Parts[1].Vertices[0] == smooth.Data->Parts[1].Vertices[0]);
	CHECK((smooth.Data->Parts[1].Vertices[1].Normal == Vector3{1, 0, .2}));
	CHECK((smooth.Data->Parts[1].Vertices[2].Normal == Vector3{0, -1, .2}));
}
TEST_CASE(
	"Cone source Side validator precedes half even rounding and Bool uses the documented threshold",
	"[imagegraph][mesh_cone]"
) {
	CHECK(Mesh({{"side", 3.5}}).Data->Parts[0].Vertices.size() == 12);
	CHECK(Mesh({{"side", 4.5}}).Data->Parts[0].Vertices.size() == 12);
	CHECK(Mesh({{"side", 5.5}}).Data->Parts[0].Vertices.size() == 18);
	CHECK(Mesh({{"side", -100.5}}).Data->Parts[0].Vertices.size() == 9);
	CHECK(Mesh({{"smooth_side", .5}}) == Mesh({{"smooth_side", false}}));
	CHECK(Mesh({{"smooth_side", .5001}}) == Mesh({{"smooth_side", true}}));
	CHECK(Mesh({{"smooth_side", -2.0}}) == Mesh({{"smooth_side", false}}));
	for (int64_t sides : {int64_t{683}, int64_t{4096}, std::numeric_limits<int64_t>::max()}) {
		const auto refused = RunNode(CONE, {}, {{"side", sides}});
		CHECK_FALSE(refused.Ok);
		CHECK(refused.Code == Status::LimitExceeded);
		CHECK(refused.Values.empty());
	}
	CHECK(Mesh({{"side", int64_t{682}}}).Data->Parts[0].Vertices.size() == 2046);
}
TEST_CASE(
	"Cone material ports preserve complete independent owned float descriptors", "[imagegraph][mesh_cone]"
) {
	std::array<MaterialValue3D, 2> materials;
	std::vector<AuthoredValue> values;
	const std::array<std::string_view, 2> ports{"material_bottom", "material_side"};
	for (size_t i = 0; i < materials.size(); ++i) {
		auto &data = materials[i].Edit();
		data.TextureScale = {double(i) + 2, -3};
		data.TextureShift = {.25, -.5};
		data.TextureFilter = 2;
		data.Diffuse = double(i) + .25;
		data.Specular = .75;
		data.Shininess = 64;
		data.Reflectance = .4;
		data.NormalStrength = .7;
		data.Metal = true;
		data.MetallicMapped = true;
		data.RoughnessMapped = true;
		data.MetallicRange = {.1, .8};
		data.RoughnessRange = {.2, .9};
		data.Surface = Image{1, 1, std::vector<uint8_t>(16), 0, SurfaceFormat::RGBA32Float};
		REQUIRE(StoreSurfacePixel(*data.Surface, 0, 0, SurfacePixel{-2, double(i) + 4, .25, 1}));
		data.Surface->Pixels.reserve(512);
		data.Normal = Image{1, 1, {128, 128, 255, 255}};
		data.PropertiesMap = Image{1, 1, {42, 53, 0, 255}};
		values.push_back({std::string(ports[i]), materials[i]});
	}
	const auto mesh = Mesh(values);
	for (size_t i = 0; i < 2; ++i) {
		CHECK(mesh.Data->Materials[i] == materials[i]);
		CHECK(&mesh.Data->Materials[i].Get() != &materials[i].Get());
		for (const auto &pair :
			 {std::pair{&mesh.Data->Materials[i].Get().Surface, &materials[i].Get().Surface},
			  std::pair{&mesh.Data->Materials[i].Get().Normal, &materials[i].Get().Normal},
			  std::pair{&mesh.Data->Materials[i].Get().PropertiesMap, &materials[i].Get().PropertiesMap}}) {
			REQUIRE(*pair.first);
			REQUIRE(*pair.second);
			CHECK((*pair.first)->Pixels.data() != (*pair.second)->Pixels.data());
		}
	}
	for (const auto port : ports) {
		MaterialValue3D invalid;
		invalid.Edit().Diffuse = std::numeric_limits<double>::infinity();
		const auto run = RunNode(CONE, {}, {{port, invalid}});
		CHECK_FALSE(run.Ok);
		CHECK(run.Code == Status::InvalidValue);
		CHECK(run.Port == port);
		CHECK(run.Values.empty());
	}
}
TEST_CASE(
	"Persisted Material Cone Transform GetData retains both materials and every local transform control",
	"[imagegraph][mesh_cone]"
) {
	Document doc;
	doc.FormatVersion = 9;
	const MeshTransform3D local{{2, 3, 4}, {.1, .2, .3}, {.1, .2, .3, .4}, {2, -3, .5}},
		outer{{-1, 2, 3}, {4, 5, 6}, {0, 0, 1, 0}, {-1, 2, 4}};
	doc.Nodes = {
		{"bottom", "pc.3_d_material", "", {}, {{"diffuse", .25}}},
		{"side", "pc.3_d_material", "", {}, {{"diffuse", .75}}},
		{"cone",
		 std::string(CONE),
		 "",
		 {},
		 {{"side", int64_t{4}},
		  {"smooth_side", true},
		  {"position", local.Position},
		  {"anchor", local.Anchor},
		  {"rotation", local.Rotation},
		  {"scale", local.Scale}}},
		{"transform",
		 "pc.3_d_transform",
		 "",
		 {},
		 {{"position", outer.Position},
		  {"anchor", outer.Anchor},
		  {"rotation", outer.Rotation},
		  {"scale", outer.Scale}}},
		{"get", "pc.3_d_get_data", "", {}, {}}
	};
	doc.Links = {
		{"bottom", "material", "cone", "material_bottom"},
		{"side", "material", "cone", "material_side"},
		{"cone", "mesh", "transform", "mesh"},
		{"transform", "mesh", "get", "mesh"}
	};
	doc.Outputs = {{"out", "transform", "mesh"}};
	const auto mesh = std::get<MeshValue3D>(Replay(doc));
	Geometry(mesh, 4, true);
	CHECK((mesh.Data->LocalTransforms == std::vector<MeshTransform3D>{outer, local}));
	CHECK(mesh.Data->Materials[0].Get().Diffuse == .25);
	CHECK(mesh.Data->Materials[1].Get().Diffuse == .75);
	for (const auto &expected : std::vector<AuthoredValue>{
			 {"origin", outer.Anchor},
			 {"position", outer.Position},
			 {"rotation", Vector4{0, 0, 1, 0}},
			 {"scale", outer.Scale}
		 }) {
		doc.Outputs = {{"out", "get", expected.Port}};
		CHECK(Replay(doc) == expected.Data);
	}
}
TEST_CASE(
	"Persisted Cone Side and Smooth arrays follow every source eight slot processor mode",
	"[imagegraph][mesh_cone]"
) {
	const std::array<std::array<size_t, 6>, 4> sideRows{
		{{0, 1, 0, 0, 0, 0}, {0, 1, 1, 0, 0, 0}, {0, 0, 0, 1, 1, 1}, {0, 0, 0, 0, 0, 0}}
	},
		smoothRows{{{0, 1, 2, 0, 0, 0}, {0, 1, 2, 0, 0, 0}, {0, 1, 2, 0, 1, 2}, {0, 0, 0, 0, 0, 0}}};
	for (int64_t mode : {0, 1, 2, 3}) {
		Document doc;
		doc.FormatVersion = 9;
		doc.Nodes = {
			{"cone",
			 std::string(CONE),
			 "",
			 {},
			 {{"side", ArrayValue{ValueType::Integer, {int64_t{3}, int64_t{4}}}},
			  {"smooth_side", ArrayValue{ValueType::Boolean, {false, true, false}}},
			  {"attribute_array_process", EnumValue{mode}}}}
		};
		doc.Outputs = {{"out", "cone", "mesh"}};
		const auto array = std::get<ArrayValue>(Replay(doc));
		const size_t count = mode < 2 ? 3 : 6;
		REQUIRE(array.Elements.size() == count);
		for (size_t i = 0; i < count; ++i) {
			INFO("mode=" << mode << " row=" << i);
			Geometry(
				std::get<MeshValue3D>(array.Elements[i]),
				3 + sideRows[size_t(mode)][i],
				smoothRows[size_t(mode)][i] == 1
			);
		}
	}
}
TEST_CASE(
	"Persisted Cone material arrays retain source scheduling and independently owned clones",
	"[imagegraph][mesh_cone]"
) {
	for (int64_t mode : {0, 1, 2, 3}) {
		Document doc;
		doc.FormatVersion = 9;
		doc.Nodes = {
			{"material",
			 "pc.3_d_material",
			 "",
			 {},
			 {{"diffuse", ArrayValue{ValueType::Scalar, {.1, .5, .9}}}}},
			{"cone",
			 std::string(CONE),
			 "",
			 {},
			 {{"side", ArrayValue{ValueType::Integer, {int64_t{3}, int64_t{4}}}},
			  {"attribute_array_process", EnumValue{mode}}}}
		};
		doc.Links = {
			{"material", "material", "cone", "material_bottom"},
			{"material", "material", "cone", "material_side"}
		};
		doc.Outputs = {{"out", "cone", "mesh"}};
		const auto array = std::get<ArrayValue>(Replay(doc));
		// The same three rows at two material slots contribute independently in Expand.
		const size_t count = mode < 2 ? 3 : 18;
		REQUIRE(array.Elements.size() == count);
		const std::array<double, 3> diffuse{.1, .5, .9};
		for (size_t i = 0; i < count; ++i) {
			const auto &mesh = std::get<MeshValue3D>(array.Elements[i]);
			const size_t side = mode == 0	? i % 2
								: mode == 1 ? std::min(i, size_t{1})
								: mode == 2 ? i / 9
											: 0;
			const size_t bottom = mode < 2 ? i : mode == 2 ? (i / 3) % 3 : 0;
			const size_t sidemat = mode < 2 ? i : mode == 2 ? i % 3 : 0;
			CHECK(mesh.Data->Parts[0].Vertices.size() == 3 * (3 + side));
			CHECK(mesh.Data->Materials[0].Get().Diffuse == diffuse[bottom]);
			CHECK(mesh.Data->Materials[1].Get().Diffuse == diffuse[sidemat]);
			CHECK(&mesh.Data->Materials[0].Get() != &mesh.Data->Materials[1].Get());
		}
	}
}
TEST_CASE(
	"Persisted Cone linked transform arrays retain all source slot schedules without composition",
	"[imagegraph][mesh_cone]"
) {
	const std::array<Vector3, 2> positions{{{1, 2, 3}, {4, 5, 6}}}, anchors{{{.1, .2, .3}, {.4, .5, .6}}};
	const std::array<Vector3, 3> scales{{{1, 1, 1}, {-1, 2, 3}, {0, .5, 4}}};
	const std::array<Quaternion, 2> rotations{{{0, 0, 0, 1}, {.1, .2, .3, .4}}};
	for (int64_t mode : {0, 1, 2, 3}) {
		Document doc;
		doc.FormatVersion = 9;
		Node rotation{"rotations", "pc.array", "", {}, {{"type", EnumValue{0}}, {"spread_array", true}}};
		rotation.DynamicInputs = {
			{"input_0", ValueType::Quaternion, rotations[0]}, {"input_1", ValueType::Quaternion, rotations[1]}
		};
		// Authored carriers remain flat scalar arrays. Vector/Quaternion array results are runtime values.
		doc.Nodes = {
			{"positions",
			 "pc.vector3",
			 "",
			 {},
			 {{"x", ArrayValue{ValueType::Scalar, {1.0, 4.0}}},
			  {"y", ArrayValue{ValueType::Scalar, {2.0, 5.0}}},
			  {"z", ArrayValue{ValueType::Scalar, {3.0, 6.0}}}}},
			{"scales",
			 "pc.vector3",
			 "",
			 {},
			 {{"x", ArrayValue{ValueType::Scalar, {1.0, -1.0, 0.0}}},
			  {"y", ArrayValue{ValueType::Scalar, {1.0, 2.0, .5}}},
			  {"z", ArrayValue{ValueType::Scalar, {1.0, 3.0, 4.0}}}}},
			{"anchors",
			 "pc.vector3",
			 "",
			 {},
			 {{"x", ArrayValue{ValueType::Scalar, {.1, .4}}},
			  {"y", ArrayValue{ValueType::Scalar, {.2, .5}}},
			  {"z", ArrayValue{ValueType::Scalar, {.3, .6}}}}},
			rotation,
			{"cone", std::string(CONE), "", {}, {{"attribute_array_process", EnumValue{mode}}}}
		};
		doc.Links = {
			{"positions", "vector", "cone", "position"},
			{"scales", "vector", "cone", "scale"},
			{"anchors", "vector", "cone", "anchor"},
			{"rotations", "array", "cone", "rotation"}
		};
		doc.Outputs = {{"out", "cone", "mesh"}};
		const auto array = std::get<ArrayValue>(Replay(doc));
		const size_t count = mode < 2 ? 3 : 24;
		REQUIRE(array.Elements.size() == count);
		for (size_t i = 0; i < count; ++i) {
			INFO("mode=" << mode << " row=" << i);
			const auto &mesh = std::get<MeshValue3D>(array.Elements[i]);
			REQUIRE(mesh.Data);
			REQUIRE(mesh.Data->LocalTransforms.size() == 1);
			const size_t position = mode == 0 || mode == 3 ? i % 2
									: mode == 1			   ? std::min(i, size_t{1})
														   : i / 12;
			const size_t rotationIndex = mode == 0 || mode == 3 ? i % 2
										 : mode == 1			? std::min(i, size_t{1})
																: (i / 6) % 2;
			const size_t scale = mode < 2 ? i : mode == 2 ? (i / 2) % 3 : i % 3;
			const size_t anchor = mode == 1 ? std::min(i, size_t{1}) : i % 2;
			CHECK((
				mesh.Data->LocalTransforms.front() ==
				MeshTransform3D{positions[position], anchors[anchor], rotations[rotationIndex], scales[scale]}
			));
		}
	}
}
TEST_CASE(
	"Cone shared ledger admits retained material clones and source getter workspace before publication",
	"[imagegraph][mesh_cone]"
) {
	MaterialValue3D material;
	material.Edit().Surface = Image{1, 1, {1, 2, 3, 255}};
	material.Edit().Surface->Pixels.reserve(512);
	const Value input = std::move(material);
	const auto *entry = FindCatalogueEntry(CONE);
	REQUIRE(entry);
	const Node node{"cone", std::string(CONE), "", {}, {}};
	const EvaluationRequest request;
	uint64_t peak = 0;
	const auto run = [&](uint64_t maximum, bool accepted) {
		detail::EvaluationBudget budget(maximum);
		auto prior = budget.Reserve(detail::RetainedPayloadBytes(input));
		REQUIRE(prior);
		{
			detail::NodeContext context(node, *entry, request, budget);
			context.ByteBudget = maximum;
			context.Values = {{"attribute_array_process", EnumValue{0}}, {"side", 4.5}, {"smooth_side", .75}};
			for (const auto port : {"material_bottom", "material_side"})
				context.ValueViews.emplace_back(port, &input);
			const bool okay = detail::RunProcessorBatch(context, detail::FindExecutor(CONE));
			INFO(context.FailurePort << ':' << context.FailureMessage);
			CHECK(okay == accepted);
			if (accepted) {
				REQUIRE(context.OutputValues.size() == 1);
				const auto &mesh = std::get<MeshValue3D>(context.OutputValues.front().Data);
				CHECK(detail::ValidMeshPayload(mesh));
				CHECK(mesh.Data->Parts[0].Vertices.size() == 12);
				CHECK(mesh.Data->Materials[0] == std::get<MaterialValue3D>(input));
				peak = budget.Peak();
			} else {
				CHECK(context.FailureCode == Status::LimitExceeded);
				CHECK(context.OutputValues.empty());
			}
		}
		CHECK(budget.Used() == prior->Bytes());
	};
	run(Limits::MaximumEvaluationBytes, true);
	const uint64_t exact = peak;
	run(exact, true);
	run(exact - 1, false);
}

TEST_CASE(
	"Cone rejects malformed transform and material inputs before output publication",
	"[imagegraph][mesh_cone]"
) {
	const double bad = std::numeric_limits<double>::infinity();
	for (const auto port : {"position", "anchor", "scale"}) {
		const auto run = RunNode(CONE, {}, {{port, Vector3{1, bad, 3}}});
		CHECK_FALSE(run.Ok);
		CHECK(run.Code == Status::InvalidValue);
		CHECK(run.Port == port);
		CHECK(run.Values.empty());
	}
	const auto rotation = RunNode(CONE, {}, {{"rotation", Quaternion{0, 0, bad, 1}}});
	CHECK_FALSE(rotation.Ok);
	CHECK(rotation.Code == Status::InvalidValue);
	CHECK(rotation.Port == "rotation");
	CHECK(rotation.Values.empty());
	const auto wrong = RunNode(CONE, {}, {{"material_side", int64_t{1}}});
	CHECK_FALSE(wrong.Ok);
	CHECK(wrong.Code == Status::TypeMismatch);
	CHECK(wrong.Port == "material_side");
	CHECK(wrong.Values.empty());
	const auto raw = RunNode(CONE, {}, {{"side", int64_t{2}}});
	REQUIRE(raw.Ok);
	const auto &mesh = std::get<MeshValue3D>(*raw.OutputValue("mesh"));
	CHECK(mesh.Data->Parts[0].Vertices.size() == 6);
	CHECK(mesh.Data->Parts[1].Vertices.size() == 6);
	CHECK(mesh.Data->Edges.size() == 4);
	const auto fraction = RunNode(CONE, {}, {{"side", 2.5}});
	CHECK_FALSE(fraction.Ok);
	CHECK(fraction.Code == Status::UnsupportedExecution);
	CHECK(fraction.Port == "side");
	CHECK(fraction.Values.empty());
}

TEST_CASE(
	"Persisted linked Surface getters clone default Cone bottom and side materials", "[imagegraph][mesh_cone]"
) {
	Document doc;
	doc.FormatVersion = 9;
	doc.Nodes = {
		{"bottom",
		 "pc.solid",
		 "",
		 {},
		 {{"dimension", Vector2{1, 1}},
		  {"dimension_unit", EnumValue{0}},
		  {"color", Colour{12, 34, 56, 255}}}},
		{"side",
		 "pc.solid",
		 "",
		 {},
		 {{"dimension", Vector2{1, 1}},
		  {"dimension_unit", EnumValue{0}},
		  {"color", Colour{98, 76, 54, 255}}}},
		{"cone", std::string(CONE), "", {}, {}}
	};
	doc.Links = {
		{"bottom", "surface_out", "cone", "material_bottom"}, {"side", "surface_out", "cone", "material_side"}
	};
	doc.Outputs = {{"out", "cone", "mesh"}};
	const auto mesh = std::get<MeshValue3D>(Replay(doc));
	REQUIRE(mesh.Data);
	REQUIRE(mesh.Data->Materials.size() == 2);
	const std::array<std::array<uint8_t, 4>, 2> colors{{{12, 34, 56, 255}, {98, 76, 54, 255}}};
	for (size_t i = 0; i < 2; ++i) {
		const auto &data = mesh.Data->Materials[i].Get();
		REQUIRE(data.Surface);
		CHECK(data.Surface->Width == 1);
		CHECK(data.Surface->Height == 1);
		CHECK(data.Surface->Format == SurfaceFormat::RGBA8Unorm);
		CHECK(data.Surface->Pixels == std::vector<uint8_t>(colors[i].begin(), colors[i].end()));
		MaterialData3D expected;
		expected.Surface = data.Surface;
		CHECK(data == expected);
	}
	CHECK(
		mesh.Data->Materials[0].Get().Surface->Pixels.data() !=
		mesh.Data->Materials[1].Get().Surface->Pixels.data()
	);
}
TEST_CASE(
	"Cone raw row kernel handles zero negative and nonfinite Side without normalizing",
	"[imagegraph][mesh_cone]"
) {
	const auto zero = RunNode(CONE, {}, {{"side", int64_t{0}}});
	REQUIRE(zero.Ok);
	const auto &mesh = std::get<MeshValue3D>(*zero.OutputValue("mesh"));
	REQUIRE(mesh.Data);
	REQUIRE(mesh.Data->Parts.size() == 2);
	CHECK(mesh.Data->Parts[0].Vertices.empty());
	CHECK(mesh.Data->Parts[1].Vertices.empty());
	CHECK(mesh.Data->Edges.empty());
	CHECK(detail::ValidMeshPayload(mesh));
	const auto negative = RunNode(CONE, {}, {{"side", int64_t{-1}}});
	CHECK_FALSE(negative.Ok);
	CHECK(negative.Code == Status::InvalidValue);
	CHECK(negative.Port == "side");
	CHECK(negative.Values.empty());
	for (double bad :
		 {std::numeric_limits<double>::quiet_NaN(),
		  std::numeric_limits<double>::infinity(),
		  -std::numeric_limits<double>::infinity()}) {
		const auto run = RunNode(CONE, {}, {{"side", bad}});
		CHECK_FALSE(run.Ok);
		CHECK(run.Code == Status::InvalidValue);
		CHECK(run.Port == "side");
		CHECK(run.Values.empty());
	}
}
