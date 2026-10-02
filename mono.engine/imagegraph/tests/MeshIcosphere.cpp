#include "../src/MeshPayload.hpp"
#include "../src/ProcessorBatch.hpp"
#include "NodeHarness.hpp"

#include <engine/testing/Suite.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <iomanip>
#include <limits>
#include <locale>
#include <map>
#include <numbers>
#include <sstream>

TEST_SUITE_ID("engine.imagegraph.mesh_sphere_ico")
using namespace engine::imagegraph;
using imagegraph_test::RunNode;
namespace {
	constexpr std::string_view SPHERE = "pc.3_d_mesh_sphere_ico";
	MeshValue3D Mesh(std::vector<AuthoredValue> values = {}) {
		const auto *entry = FindCatalogueEntry(SPHERE);
		REQUIRE(entry);
		const Node node{"sphere", std::string(SPHERE), "", {}, {}};
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
		const bool okay = detail::RunProcessorBatch(context, detail::FindExecutor(SPHERE));
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
		CHECK(actual.X == Catch::Approx(expected.X).epsilon(0).margin(1e-12));
		CHECK(actual.Y == Catch::Approx(expected.Y).epsilon(0).margin(1e-12));
		CHECK(actual.Z == Catch::Approx(expected.Z).epsilon(0).margin(1e-12));
	}
	void Vertex(const MeshVertex3D &actual, Vector3 p, Vector3 n, Vector2 uv) {
		Point(actual.Position, p);
		Point(actual.Normal, n);
		CHECK(actual.UV.X == Catch::Approx(uv.X).epsilon(0).margin(1e-12));
		CHECK(actual.UV.Y == Catch::Approx(uv.Y).epsilon(0).margin(1e-12));
		CHECK((actual.Tint == Colour{255, 255, 255, 255}));
	}

	void Geometry(const MeshValue3D &mesh, uint32_t level = 1, bool smooth = false) {
		REQUIRE(mesh.Data);
		const auto &data = *mesh.Data;
		struct Reference {
			Vector3 P;
			bool Old;
			std::vector<size_t> Adjacent;
		};
		std::vector<Reference> points;
		const auto normal = [](Vector3 p) {
			const double d = std::sqrt(p.X * p.X + p.Y * p.Y + p.Z * p.Z);
			return d ? Vector3{p.X / d, p.Y / d, p.Z / d} : p;
		};
		const double b = 2 / (1 + std::sqrt(5.0));
		for (Vector3 p : std::array<Vector3, 13>{
				 {{1, 1, 1},
				  {0, b, -1},
				  {b, 1, 0},
				  {-b, 1, 0},
				  {0, b, 1},
				  {0, -b, 1},
				  {-1, 0, b},
				  {0, -b, -1},
				  {1, 0, -b},
				  {1, 0, b},
				  {-1, 0, -b},
				  {b, -1, 0},
				  {-b, -1, 0}}
			 }) {
			p = normal(p);
			points.push_back({{p.X / 2, p.Y / 2, p.Z / 2}, true, {}});
		}
		using Triangle = std::array<size_t, 3>;
		std::vector<Triangle> triangles{{3, 1, 2},	{2, 4, 3},	 {6, 4, 5},	  {5, 4, 9},  {8, 1, 7},
										{7, 1, 10}, {12, 5, 11}, {11, 7, 12}, {10, 3, 6}, {6, 12, 10},
										{9, 2, 8},	{8, 11, 9},	 {3, 4, 6},	  {9, 4, 2},  {10, 1, 3},
										{2, 1, 8},	{12, 7, 10}, {8, 7, 11},  {6, 5, 12}, {11, 5, 9}};
		for (uint32_t stage = 0; stage < level; ++stage) {
			// Unlike the kernel's endpoint keys, this oracle uses source coordinate strings.
			std::map<std::string, size_t> pool;
			std::vector<Triangle> replacement;
			const auto middle = [&](size_t first, size_t second) {
				const auto a = points[first].P, c = points[second].P;
				const Vector3 p{(a.X + c.X) / 2, (a.Y + c.Y) / 2, (a.Z + c.Z) / 2};
				std::ostringstream key;
				key.imbue(std::locale::classic());
				for (double coordinate : {p.X, p.Y, p.Z}) {
					if (coordinate == std::trunc(coordinate))
						key << int64_t(coordinate);
					else
						key << std::fixed << std::setprecision(2) << coordinate;
					key << ',';
				}
				const auto found = pool.find(key.str());
				if (found != pool.end()) return found->second;
				const size_t index = points.size();
				points.push_back({p, false, {}});
				pool.emplace(key.str(), index);
				return index;
			};
			for (const auto &triangle : triangles) {
				const auto a = triangle[0], c = triangle[1], d = triangle[2];
				const auto ac = middle(a, c), cd = middle(c, d), da = middle(d, a);
				points[a].Adjacent.insert(points[a].Adjacent.end(), {ac, da});
				points[c].Adjacent.insert(points[c].Adjacent.end(), {ac, cd});
				points[d].Adjacent.insert(points[d].Adjacent.end(), {cd, da});
				replacement.insert(replacement.end(), {{a, ac, da}, {ac, c, cd}, {da, cd, d}, {ac, cd, da}});
			}
			for (const auto &triangle : replacement)
				for (size_t id : triangle) {
					auto &vertex = points[id];
					if (vertex.Old && !vertex.Adjacent.empty()) {
						const double valence = double(vertex.Adjacent.size()) / 2, beta = 3 / (5 * valence);
						std::array<double, 3> sums{};
						for (size_t neighbor : vertex.Adjacent) {
							const auto p = points[neighbor].P;
							sums[0] += p.X;
							sums[1] += p.Y;
							sums[2] += p.Z;
						}
						const auto p = vertex.P;
						vertex.P = {
							sums[0] * .5 * beta + p.X * (1 - valence * beta),
							sums[1] * .5 * beta + p.Y * (1 - valence * beta),
							sums[2] * .5 * beta + p.Z * (1 - valence * beta)
						};
					}
					vertex.Old = true;
					vertex.Adjacent.clear();
				}
			triangles = std::move(replacement);
		}
		REQUIRE(data.Parts.size() == 1);
		REQUIRE(data.Materials.size() == 1);
		CHECK(data.Parts[0].MaterialIndex == 0);
		REQUIRE(data.Parts[0].Vertices.size() == triangles.size() * 3);
		REQUIRE(data.Edges.size() == triangles.size() * 3);
		const auto angle = [](double x, double y) {
			double a = std::atan2(-y, x) * 180 / std::numbers::pi;
			return a < 0 ? a + 360 : a;
		};
		for (size_t i = 0; i < triangles.size(); ++i) {
			INFO("level=" << level << " triangle=" << i);
			const std::array<Vector3, 3> p{
				points[triangles[i][0]].P, points[triangles[i][1]].P, points[triangles[i][2]].P
			};
			std::array<double, 3> a{p[2].X - p[0].X, p[2].Y - p[0].Y, p[2].Z - p[0].Z},
				c{p[1].X - p[0].X, p[1].Y - p[0].Y, p[1].Z - p[0].Z}, det{};
			for (size_t axis = 0; axis < 3; ++axis)
				det[axis] = a[(axis + 1) % 3] * c[(axis + 2) % 3] - a[(axis + 2) % 3] * c[(axis + 1) % 3];
			for (size_t j = 0; j < 3; ++j) {
				double va = std::fmod(angle(p[j].X, p[j].Z) + 90, 360);
				if (va > 180) va = 360 - va;
				Vertex(
					data.Parts[0].Vertices[i * 3 + j],
					p[j],
					smooth ? normal(p[j]) : Vector3{det[0], det[1], det[2]},
					{angle(p[j].X, p[j].Y) / 360, va / 180}
				);
			}
			Point(data.Edges[i * 3].From, p[0]);
			Point(data.Edges[i * 3].To, p[1]);
			Point(data.Edges[i * 3 + 1].From, p[0]);
			Point(data.Edges[i * 3 + 1].To, p[2]);
			Point(data.Edges[i * 3 + 2].From, p[2]);
			Point(data.Edges[i * 3 + 2].To, p[1]);
		}
	}
}

TEST_CASE(
	"Icosphere default is inward Loop geometry with exact ordered fields", "[imagegraph][mesh_sphere_ico]"
) {
	const auto mesh = Mesh();
	Geometry(mesh);
	CHECK(mesh.Data->Parts[0].Vertices.size() == 240);
	CHECK(mesh.Data->Edges.size() == 240);
	CHECK(mesh.Data->LocalTransforms == std::vector<MeshTransform3D>{MeshTransform3D{}});
	CHECK(mesh.Data->Materials == std::vector<MaterialValue3D>(1));
	const auto first = mesh.Data->Parts[0].Vertices[0].Position;
	CHECK(std::sqrt(first.X * first.X + first.Y * first.Y + first.Z * first.Z) < .5);
	const auto n = mesh.Data->Parts[0].Vertices[0].Normal;
	CHECK(std::sqrt(n.X * n.X + n.Y * n.Y + n.Z * n.Z) != Catch::Approx(1));
}
TEST_CASE(
	"Icosphere accepted levels preserve formatted pooling and smooth only normals",
	"[imagegraph][mesh_sphere_ico]"
) {
	for (uint32_t level = 0; level <= 3; ++level) {
		const auto flat = Mesh({{"subdivision", int64_t(level)}}),
				   smooth = Mesh({{"subdivision", int64_t(level)}, {"smooth_normal", true}});
		Geometry(flat, level);
		Geometry(smooth, level, true);
		REQUIRE(flat.Data->Parts[0].Vertices.size() == smooth.Data->Parts[0].Vertices.size());
		for (size_t i = 0; i < flat.Data->Parts[0].Vertices.size(); ++i) {
			CHECK(flat.Data->Parts[0].Vertices[i].Position == smooth.Data->Parts[0].Vertices[i].Position);
			CHECK(flat.Data->Parts[0].Vertices[i].UV == smooth.Data->Parts[0].Vertices[i].UV);
		}
		if (level == 0)
			for (const auto &v : flat.Data->Parts[0].Vertices)
				CHECK(
					std::sqrt(
						v.Position.X * v.Position.X + v.Position.Y * v.Position.Y +
						v.Position.Z * v.Position.Z
					) == Catch::Approx(.5)
				);
	}
}
TEST_CASE(
	"Icosphere source Int min half even and Bool threshold project before execution",
	"[imagegraph][mesh_sphere_ico]"
) {
	for (const auto &pair : std::array<std::pair<double, uint32_t>, 7>{
			 {{-2, 0}, {-.5, 0}, {.5, 0}, {1.5, 2}, {2.5, 2}, {3.0, 3}, {0, 0}}
		 })
		Geometry(Mesh({{"subdivision", pair.first}}), pair.second);
	Geometry(Mesh({{"smooth_normal", .5}}), 1, false);
	Geometry(Mesh({{"smooth_normal", .75}}), 1, true);
	Geometry(Mesh({{"smooth_normal", -.5}}), 1, false);
}
TEST_CASE(
	"Persisted Icosphere subdivision material and normals use all seven slot schedules",
	"[imagegraph][mesh_sphere_ico]"
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
			{"sphere",
			 std::string(SPHERE),
			 "",
			 {},
			 {{"subdivision", ArrayValue{ValueType::Integer, {int64_t{0}, int64_t{1}}}},
			  {"smooth_normal", ArrayValue{ValueType::Boolean, {false, true, false}}},
			  {"attribute_array_process", EnumValue{mode}}}}
		};
		doc.Links = {{"material", "material", "sphere", "material"}};
		doc.Outputs = {{"out", "sphere", "mesh"}};
		const auto rows = std::get<ArrayValue>(Replay(doc));
		const size_t count = mode < 2 ? 3 : 18;
		REQUIRE(rows.Elements.size() == count);
		const std::array<size_t, 7> lengths{1, 1, 1, 1, 2, 3, 3};
		std::array<size_t, 7> suffix{};
		size_t product = 18;
		for (size_t slot = 0; slot < 7; ++slot) {
			product /= lengths[slot];
			suffix[slot] = product;
		}
		for (size_t row = 0; row < count; ++row) {
			const auto index = [&](size_t slot) {
				return mode == 0   ? row % lengths[slot]
					   : mode == 1 ? std::min(row, lengths[slot] - 1)
								   : (row / suffix[mode == 2 ? slot : 6 - slot]) % lengths[slot];
			};
			const auto &mesh = std::get<MeshValue3D>(rows.Elements[row]);
			Geometry(mesh, uint32_t(index(4)), index(6) == 1);
			CHECK((mesh.Data->Materials[0].Get().Diffuse == std::array<double, 3>{.1, .5, .9}[index(5)]));
		}
	}
}
TEST_CASE(
	"Persisted Icosphere linked transform arrays retain all source slot schedules without composition",
	"[imagegraph][mesh_sphere_ico]"
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
			{"sphere", std::string(SPHERE), "", {}, {{"attribute_array_process", EnumValue{mode}}}}
		};
		doc.Links = {
			{"positions", "vector", "sphere", "position"},
			{"scales", "vector", "sphere", "scale"},
			{"anchors", "vector", "sphere", "anchor"},
			{"rotations", "array", "sphere", "rotation"}
		};
		doc.Outputs = {{"out", "sphere", "mesh"}};
		const auto array = std::get<ArrayValue>(Replay(doc));
		const size_t count = mode < 2 ? 3 : 24;
		REQUIRE(array.Elements.size() == count);
		for (size_t i = 0; i < count; ++i) {
			INFO("mode=" << mode << " row=" << i);
			const auto &mesh = std::get<MeshValue3D>(array.Elements[i]);
			REQUIRE(mesh.Data);
			REQUIRE(mesh.Data->LocalTransforms.size() == 1);

			std::array<size_t, 7> lengths{};
			lengths.fill(1);
			lengths[0] = 2;
			lengths[1] = 2;
			lengths[2] = 3;
			lengths[3] = 2;
			std::array<size_t, 7> suffix{};
			size_t product = 24;
			for (size_t slot = 0; slot < 7; ++slot) {
				product /= lengths[slot];
				suffix[slot] = product;
			}
			const auto selected = [&](size_t slot) {
				return mode == 0   ? i % lengths[slot]
					   : mode == 1 ? std::min(i, lengths[slot] - 1)
								   : (i / suffix[mode == 2 ? slot : 6 - slot]) % lengths[slot];
			};
			const size_t position = selected(0), rotationIndex = selected(1), scale = selected(2),
						 anchor = selected(3);
			Geometry(mesh);
			CHECK((
				mesh.Data->LocalTransforms.front() ==
				MeshTransform3D{positions[position], anchors[anchor], rotations[rotationIndex], scales[scale]}
			));
		}
	}
}
TEST_CASE(
	"Persisted Material Icosphere Transform GetData retains all controls and ordered local chain",
	"[imagegraph][mesh_sphere_ico]"
) {
	Document doc;
	doc.FormatVersion = 9;
	const MeshTransform3D local{{2, 3, 4}, {.1, .2, .3}, {.1, .2, .3, .4}, {-1, 2, .5}},
		outer{{-2, 5, 1}, {1, 2, 3}, {0, 0, 1, 0}, {2, -3, 4}};
	doc.Nodes = {
		{"material", "pc.3_d_material", "", {}, {{"diffuse", .25}}},
		{"sphere",
		 std::string(SPHERE),
		 "",
		 {},
		 {{"subdivision", int64_t{2}},
		  {"smooth_normal", true},
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
		{"material", "material", "sphere", "material"},
		{"sphere", "mesh", "transform", "mesh"},
		{"transform", "mesh", "get", "mesh"}
	};
	doc.Outputs = {{"out", "transform", "mesh"}};
	const auto mesh = std::get<MeshValue3D>(Replay(doc));
	Geometry(mesh, 2, true);
	CHECK((mesh.Data->LocalTransforms == std::vector<MeshTransform3D>{outer, local}));
	CHECK(mesh.Data->Materials[0].Get().Diffuse == .25);
	for (const auto &expected : std::vector<AuthoredValue>{
			 {"position", outer.Position},
			 {"origin", outer.Anchor},
			 {"rotation", Vector4{0, 0, 1, 0}},
			 {"scale", outer.Scale}
		 }) {
		doc.Outputs = {{"out", "get", expected.Port}};
		CHECK(Replay(doc) == expected.Data);
	}
}
TEST_CASE(
	"Icosphere material retains owned float textures normals maps and every descriptor field",
	"[imagegraph][mesh_sphere_ico]"
) {
	MaterialValue3D material;
	auto &m = material.Edit();
	m.Diffuse = .2;
	m.Specular = .7;
	m.Shininess = 32;
	m.Reflectance = .4;
	m.NormalStrength = .3;
	m.TextureScale = {2, -3};
	m.TextureShift = {-.25, .5};
	m.TextureFilter = 2;
	m.Metal = true;
	m.MetallicMapped = true;
	m.RoughnessMapped = true;
	m.MetallicRange = {.1, .9};
	m.RoughnessRange = {.2, .8};
	m.Surface = Image{1, 1, std::vector<uint8_t>(16), 0, SurfaceFormat::RGBA32Float};
	REQUIRE(StoreSurfacePixel(*m.Surface, 0, 0, SurfacePixel{-2, 4, .25, 1}));
	m.Surface->Pixels.reserve(512);
	m.Normal = Image{1, 1, std::vector<uint8_t>(16), 0, SurfaceFormat::RGBA32Float};
	REQUIRE(StoreSurfacePixel(*m.Normal, 0, 0, SurfacePixel{-.5, 2, .75, 1}));
	m.Normal->Pixels.reserve(256);
	m.PropertiesMap = Image{1, 1, {43, 76, 0, 255}};
	const auto mesh = Mesh({{"material", material}});
	CHECK(mesh.Data->Materials[0] == material);
	CHECK(&mesh.Data->Materials[0].Get() != &material.Get());
	const auto &copy = mesh.Data->Materials[0].Get();
	for (const auto &pair :
		 {std::pair{&copy.Surface, &m.Surface},
		  std::pair{&copy.Normal, &m.Normal},
		  std::pair{&copy.PropertiesMap, &m.PropertiesMap}}) {
		REQUIRE(*pair.first);
		CHECK((*pair.first)->Pixels.data() != (*pair.second)->Pixels.data());
	}
}
TEST_CASE(
	"Persisted Surface source getter clones default Icosphere material independently",
	"[imagegraph][mesh_sphere_ico]"
) {
	Document doc;
	doc.FormatVersion = 9;
	doc.Nodes = {
		{"surface",
		 "pc.solid",
		 "",
		 {},
		 {{"dimension", Vector2{1, 1}},
		  {"dimension_unit", EnumValue{0}},
		  {"color", Colour{12, 34, 56, 255}}}},
		{"sphere", std::string(SPHERE), "", {}, {}}
	};
	doc.Links = {{"surface", "surface_out", "sphere", "material"}};
	doc.Outputs = {{"out", "sphere", "mesh"}};
	const auto mesh = std::get<MeshValue3D>(Replay(doc));
	const auto &m = mesh.Data->Materials[0].Get();
	REQUIRE(m.Surface);
	CHECK(m.Surface->Width == 1);
	CHECK(m.Surface->Height == 1);
	CHECK(m.Surface->Format == SurfaceFormat::RGBA8Unorm);
	CHECK((m.Surface->Pixels == std::vector<uint8_t>{12, 34, 56, 255}));
	MaterialData3D expected;
	expected.Surface = m.Surface;
	CHECK(m == expected);
}
TEST_CASE(
	"Icosphere shared ledger admits retained material clone and getter scratch at exact peak atomically",
	"[imagegraph][mesh_sphere_ico]"
) {
	MaterialValue3D material;
	material.Edit().Surface = Image{1, 1, {1, 2, 3, 255}};
	material.Edit().Surface->Pixels.reserve(512);
	const Value input = std::move(material);
	const auto *entry = FindCatalogueEntry(SPHERE);
	REQUIRE(entry);
	const Node node{"sphere", std::string(SPHERE), "", {}, {}};
	const EvaluationRequest request;
	uint64_t peak = 0;
	const auto run = [&](uint64_t maximum, bool accepted) {
		detail::EvaluationBudget ledger(maximum);
		auto prior = ledger.Reserve(detail::RetainedPayloadBytes(input));
		REQUIRE(prior);
		{
			detail::NodeContext context(node, *entry, request, ledger);
			context.ByteBudget = maximum;
			context.Values = {
				{"attribute_array_process", EnumValue{0}}, {"subdivision", 1.5}, {"smooth_normal", .75}
			};
			context.ValueViews.emplace_back("material", &input);
			const bool okay = detail::RunProcessorBatch(context, detail::FindExecutor(SPHERE));
			INFO(context.FailurePort << ':' << context.FailureMessage);
			CHECK(okay == accepted);
			if (accepted) {
				REQUIRE(context.OutputValues.size() == 1);
				const auto &mesh = std::get<MeshValue3D>(context.OutputValues[0].Data);
				CHECK(detail::ValidMeshPayload(mesh));
				CHECK(mesh.Data->Parts[0].Vertices.size() == 960);
				CHECK(mesh.Data->Materials[0] == std::get<MaterialValue3D>(input));
				peak = ledger.Peak();
			} else {
				CHECK(context.FailureCode == Status::LimitExceeded);
				CHECK(context.OutputValues.empty());
			}
		}
		CHECK(ledger.Used() == prior->Bytes());
	};
	run(Limits::MaximumEvaluationBytes, true);
	const uint64_t exact = peak;
	run(exact, true);
	run(exact - 1, false);
}

TEST_CASE(
	"Persisted Icosphere raw flat numeric levels keep source getter order", "[imagegraph][mesh_sphere_ico]"
) {
	Document doc;
	doc.FormatVersion = 9;
	Node raw{"levels", "pc.array", "", {}, {{"type", EnumValue{0}}, {"spread_array", true}}};
	raw.DynamicInputs = {
		{"input_0", ValueType::Scalar, -.5},
		{"input_1", ValueType::Scalar, .5},
		{"input_2", ValueType::Scalar, 1.5},
		{"input_3", ValueType::Scalar, 2.5}
	};
	doc.Nodes = {raw, {"sphere", std::string(SPHERE), "", {}, {}}};
	doc.Links = {{"levels", "array", "sphere", "subdivision"}};
	doc.Outputs = {{"out", "sphere", "mesh"}};
	const auto rows = std::get<ArrayValue>(Replay(doc));
	REQUIRE(rows.Elements.size() == 4);
	for (size_t i = 0; i < 4; ++i)
		Geometry(std::get<MeshValue3D>(rows.Elements[i]), i < 2 ? 0 : 2);
}
TEST_CASE(
	"Icosphere invalid controls source capacity and malformed material refuse atomically",
	"[imagegraph][mesh_sphere_ico]"
) {
	for (double bad :
		 {-1.0, std::numeric_limits<double>::infinity(), std::numeric_limits<double>::quiet_NaN()}) {
		const auto run = RunNode(SPHERE, {}, {{"subdivision", bad}});
		CHECK_FALSE(run.Ok);
		CHECK(run.Code == Status::InvalidValue);
		CHECK(run.Port == "subdivision");
		CHECK(run.Values.empty());
	}
	for (int64_t level : {4, 5, 100}) {
		const auto run = RunNode(SPHERE, {}, {{"subdivision", level}});
		CHECK_FALSE(run.Ok);
		CHECK(run.Code == Status::LimitExceeded);
		CHECK(run.Port == "mesh");
		CHECK(run.Values.empty());
	}
	const auto fractional = RunNode(SPHERE, {}, {{"subdivision", 1.5}});
	CHECK_FALSE(fractional.Ok);
	CHECK(fractional.Code == Status::UnsupportedExecution);
	CHECK(fractional.Port == "subdivision");
	CHECK(fractional.Values.empty());
	for (const auto port : {"position", "anchor", "scale"}) {
		const auto run =
			RunNode(SPHERE, {}, {{port, Vector3{0, std::numeric_limits<double>::infinity(), 1}}});
		CHECK_FALSE(run.Ok);
		CHECK(run.Code == Status::InvalidValue);
		CHECK(run.Port == port);
		CHECK(run.Values.empty());
	}
	const auto rotation =
		RunNode(SPHERE, {}, {{"rotation", Quaternion{0, 0, std::numeric_limits<double>::infinity(), 1}}});
	CHECK_FALSE(rotation.Ok);
	CHECK(rotation.Code == Status::InvalidValue);
	CHECK(rotation.Port == "rotation");
	CHECK(rotation.Values.empty());
	MaterialValue3D bad;
	bad.Edit().Surface = Image{1, 1, std::vector<uint8_t>(16), 0, SurfaceFormat::RGBA32Float};
	const float infinite = std::numeric_limits<float>::infinity();
	std::memcpy(bad.Edit().Surface->Pixels.data(), &infinite, sizeof(infinite));
	const auto material = RunNode(SPHERE, {}, {{"material", bad}});
	CHECK_FALSE(material.Ok);
	CHECK(material.Code == Status::InvalidValue);
	CHECK(material.Port == "material");
	CHECK(material.Values.empty());
}
TEST_CASE(
	"Icosphere nested integer authoring remains a named preexecution carrier gap",
	"[imagegraph][mesh_sphere_ico]"
) {
	Document doc;
	doc.FormatVersion = 9;
	doc.Nodes = {
		{"sphere",
		 std::string(SPHERE),
		 "",
		 {},
		 {{"subdivision", ArrayValue{ValueType::Integer, {}, {{int64_t{1}, int64_t{2}}}}}}}
	};
	doc.Outputs = {{"out", "sphere", "mesh"}};
	Document restored;
	Diagnostic diagnostic;
	CHECK(Read(Write(doc), restored, diagnostic) == Status::Malformed);
	CHECK(diagnostic.Message.find("malformed imagegraph record") != std::string::npos);
	Plan plan;
	CHECK(Compile(doc, plan, diagnostic) == Status::TypeMismatch);
	CHECK(diagnostic.Port == "subdivision");
	CHECK(diagnostic.Message == "authored property has the wrong value type");
}
