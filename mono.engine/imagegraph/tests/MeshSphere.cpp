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

TEST_SUITE_ID("engine.imagegraph.mesh_sphere_uv")
using namespace engine::imagegraph;
using imagegraph_test::RunNode;
namespace {
	constexpr std::string_view SPHERE = "pc.3_d_mesh_sphere_uv";
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

	void Geometry(
		const MeshValue3D &mesh,
		double horizontal = 8,
		double vertical = 16,
		bool smooth = false,
		int projection = 0
	) {
		REQUIRE(mesh.Data);
		const auto &data = *mesh.Data;
		const size_t h = size_t(horizontal), v = size_t(std::ceil(vertical));
		REQUIRE(data.Parts.size() == 1);
		REQUIRE(data.Materials.size() == 1);
		CHECK(data.Parts[0].MaterialIndex == 0);
		REQUIRE(data.Parts[0].Vertices.size() == 6 * h * v);
		REQUIRE(data.Edges.size() == 2 * h * v);
		// Independent spherical coordinates, positive-Y orientation and source-unsnapped trig.
		const auto point = [](double longitude, double latitude) {
			const double phi = longitude * std::numbers::pi / 180, theta = latitude * std::numbers::pi / 180;
			return Vector3{
				.5 * std::cos(theta) * std::cos(phi),
				.5 * std::cos(theta) * std::sin(phi),
				.5 * std::sin(theta)
			};
		};
		const auto tex = [&](double latitude) {
			const double a = latitude * std::numbers::pi / 180;
			if (projection == 0) return (1 - std::sin(a)) / 2;
			if (projection == 1) return (90 - latitude) / 180;
			return 1 - 2 * std::atan(std::exp(a)) / std::numbers::pi;
		};
		for (size_t i = 0; i < v; ++i)
			for (size_t j = 0; j < h; ++j) {
				INFO("longitude cell=" << i << " latitude cell=" << j);
				const double a = 360 * double(i) / vertical, b = 360 * double(i + 1) / vertical,
							 c = 90 - 180 * double(j) / horizontal, d = 90 - 180 * double(j + 1) / horizontal;
				const std::array<Vector3, 4> p{point(a, c), point(b, c), point(a, d), point(b, d)};
				std::array<Vector3, 4> n = p;
				if (!smooth) {
					// A determinant of the two source-ordered triangle edges, normalized independently.
					const std::array<double, 3> down{p[2].X - p[0].X, p[2].Y - p[0].Y, p[2].Z - p[0].Z},
						across{p[1].X - p[0].X, p[1].Y - p[0].Y, p[1].Z - p[0].Z};
					std::array<double, 3> determinant{};
					for (size_t k = 0; k < 3; ++k)
						determinant[k] =
							down[(k + 1) % 3] * across[(k + 2) % 3] - down[(k + 2) % 3] * across[(k + 1) % 3];
					const double length = std::hypot(determinant[0], determinant[1], determinant[2]);
					REQUIRE(length > 0);
					n.fill({determinant[0] / length, determinant[1] / length, determinant[2] / length});
				}
				const std::array<Vector2, 4> uv{
					{{a / 360, tex(c)}, {b / 360, tex(c)}, {a / 360, tex(d)}, {b / 360, tex(d)}}
				};
				constexpr std::array<size_t, 6> order{0, 1, 2, 1, 3, 2};
				for (size_t k = 0; k < 6; ++k)
					Vertex(
						data.Parts[0].Vertices[(i * h + j) * 6 + k], p[order[k]], n[order[k]], uv[order[k]]
					);
				Point(data.Edges[(i * h + j) * 2].From, p[0]);
				Point(data.Edges[(i * h + j) * 2].To, p[1]);
				Point(data.Edges[(i * h + j) * 2 + 1].From, p[0]);
				Point(data.Edges[(i * h + j) * 2 + 1].To, p[2]);
			}
	}
}

TEST_CASE(
	"UV Sphere default source swaps axes and retains every ordered field", "[imagegraph][mesh_sphere_uv]"
) {
	const auto mesh = Mesh();
	Geometry(mesh);
	CHECK(mesh.Data->Parts[0].Vertices.size() == 768);
	CHECK(mesh.Data->Edges.size() == 256);
	CHECK(mesh.Data->LocalTransforms == std::vector<MeshTransform3D>{MeshTransform3D{}});
	CHECK(mesh.Data->Materials == std::vector<MaterialValue3D>(1));
	const auto &north = mesh.Data->Parts[0].Vertices[0];
	// Pinned geometry calls dcos/dsin, so native floating trig residues are retained.
	CHECK(north.Position.X != 0);
	Point(north.Position, {.5 * std::cos(std::numbers::pi / 2), 0, .5});
	CHECK(mesh.Data->Parts[0].Vertices[1].Position.Y > 0);
	CHECK(mesh.Data->Parts[0].Vertices[0].UV.X == 0);
	CHECK(mesh.Data->Parts[0].Vertices[(16 * 8 - 1) * 6 + 4].UV.X == 1);
}
TEST_CASE(
	"UV Sphere nondefault slices smooth normals and both documented projections",
	"[imagegraph][mesh_sphere_uv]"
) {
	for (bool smooth : {false, true})
		for (int64_t projection : {0, 1}) {
			const auto mesh = Mesh(
				{{"horizontal_slices", int64_t{3}},
				 {"vertical_slices", int64_t{5}},
				 {"smooth_normal", smooth},
				 {"projection", EnumValue{projection}}}
			);
			Geometry(mesh, 3, 5, smooth, int(projection));
			if (smooth) {
				const auto &vertex = mesh.Data->Parts[0].Vertices[2];
				Point(vertex.Normal, vertex.Position);
				CHECK(std::hypot(vertex.Normal.X, vertex.Normal.Y, vertex.Normal.Z) == Catch::Approx(.5));
			}
		}
}
TEST_CASE(
	"UV Sphere source getters min clamp then half even round and Bool threshold",
	"[imagegraph][mesh_sphere_uv]"
) {
	for (const auto &controls : std::array<std::array<double, 4>, 4>{
			 {{-2, -3, 2, 3}, {2.5, 3.5, 2, 4}, {3.5, 4.5, 4, 4}, {4.5, 5.5, 4, 6}}
		 }) {
		Geometry(
			Mesh({{"horizontal_slices", controls[0]}, {"vertical_slices", controls[1]}}),
			controls[2],
			controls[3]
		);
	}
	Geometry(Mesh({{"smooth_normal", .5}}), 8, 16, false);
	Geometry(Mesh({{"smooth_normal", .75}}), 8, 16, true);
	// Default EScroll scalar getter clamps to the last visible choice, not latent switch case 2.
	Geometry(Mesh({{"projection", EnumValue{2}}}), 8, 16, false, 1);
}
TEST_CASE(
	"UV Sphere raw projection arrays reach latent source Mercator case", "[imagegraph][mesh_sphere_uv]"
) {
	Document doc;
	doc.FormatVersion = 9;
	doc.Nodes = {
		{"sphere",
		 std::string(SPHERE),
		 "",
		 {},
		 {{"horizontal_slices", int64_t{3}},
		  {"vertical_slices", int64_t{4}},
		  {"projection", ArrayValue{ValueType::Integer, {int64_t{0}, int64_t{1}, int64_t{2}}}}}}
	};
	doc.Outputs = {{"out", "sphere", "mesh"}};
	const auto array = std::get<ArrayValue>(Replay(doc));
	REQUIRE(array.Elements.size() == 3);
	for (size_t i = 0; i < 3; ++i)
		Geometry(std::get<MeshValue3D>(array.Elements[i]), 3, 4, false, int(i));
	const auto &lambert = std::get<MeshValue3D>(array.Elements[0]).Data->Parts[0].Vertices[2];
	const auto &equirectangular = std::get<MeshValue3D>(array.Elements[1]).Data->Parts[0].Vertices[2];
	const auto &mercator = std::get<MeshValue3D>(array.Elements[2]).Data->Parts[0].Vertices[2];
	CHECK(lambert.UV.Y == Catch::Approx(.25));
	CHECK(equirectangular.UV.Y == Catch::Approx(1.0 / 3));
	CHECK(mercator.UV.Y != Catch::Approx(equirectangular.UV.Y));
}
TEST_CASE(
	"UV Sphere every geometry material control persists all nine-slot schedules",
	"[imagegraph][mesh_sphere_uv]"
) {
	constexpr std::array<std::string_view, 4> ports{
		"vertical_slices", "material", "smooth_normal", "projection"
	};
	constexpr std::array<size_t, 4> slots{5, 6, 7, 8};
	for (size_t c = 0; c < ports.size(); ++c)
		for (int64_t mode : {0, 1, 2, 3}) {
			Document doc;
			doc.FormatVersion = 9;
			Node sphere{
				"sphere",
				std::string(SPHERE),
				"",
				{},
				{{"horizontal_slices", ArrayValue{ValueType::Integer, {int64_t{2}, int64_t{3}}}},
				 {"attribute_array_process", EnumValue{mode}}}
			};
			if (c == 0)
				sphere.Values.push_back(
					{"vertical_slices", ArrayValue{ValueType::Integer, {int64_t{3}, int64_t{4}, int64_t{5}}}}
				);
			if (c == 2)
				sphere.Values.push_back(
					{"smooth_normal", ArrayValue{ValueType::Boolean, {false, true, false}}}
				);
			if (c == 3)
				sphere.Values.push_back(
					{"projection", ArrayValue{ValueType::Integer, {int64_t{0}, int64_t{1}, int64_t{2}}}}
				);
			doc.Nodes = {sphere};
			if (c == 1) {
				doc.Nodes.insert(
					doc.Nodes.begin(),
					Node{
						"material",
						"pc.3_d_material",
						"",
						{},
						{{"diffuse", ArrayValue{ValueType::Scalar, {.1, .5, .9}}}}
					}
				);
				doc.Links = {{"material", "material", "sphere", "material"}};
			}
			doc.Outputs = {{"out", "sphere", "mesh"}};
			const auto rows = std::get<ArrayValue>(Replay(doc));
			const size_t count = mode < 2 ? 3 : 6;
			REQUIRE(rows.Elements.size() == count);
			std::array<size_t, 9> lengths{};
			lengths.fill(1);
			lengths[4] = 2;
			lengths[slots[c]] = 3;
			std::array<size_t, 9> suffix{};
			size_t product = 6;
			for (size_t slot = 0; slot < 9; ++slot) {
				product /= lengths[slot];
				suffix[slot] = product;
			}
			for (size_t row = 0; row < count; ++row) {
				INFO("control=" << ports[c] << " mode=" << mode << " row=" << row);
				const auto index = [&](size_t slot) {
					return mode == 0   ? row % lengths[slot]
						   : mode == 1 ? std::min(row, lengths[slot] - 1)
									   : (row / suffix[mode == 2 ? slot : 8 - slot]) % lengths[slot];
				};
				const size_t a = index(4), b = index(slots[c]);
				const auto &mesh = std::get<MeshValue3D>(rows.Elements[row]);
				Geometry(
					mesh, 2 + double(a), c == 0 ? 3 + double(b) : 16, c == 2 && b == 1, c == 3 ? int(b) : 0
				);
				if (c == 1)
					CHECK((mesh.Data->Materials[0].Get().Diffuse == std::array<double, 3>{.1, .5, .9}[b]));
			}
		}
}
TEST_CASE(
	"Persisted UV Sphere linked transform arrays retain all source slot schedules without composition",
	"[imagegraph][mesh_sphere_uv]"
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

			std::array<size_t, 9> lengths{};
			lengths.fill(1);
			lengths[0] = 2;
			lengths[1] = 2;
			lengths[2] = 3;
			lengths[3] = 2;
			std::array<size_t, 9> suffix{};
			size_t product = 24;
			for (size_t slot = 0; slot < 9; ++slot) {
				product /= lengths[slot];
				suffix[slot] = product;
			}
			const auto selected = [&](size_t slot) {
				return mode == 0   ? i % lengths[slot]
					   : mode == 1 ? std::min(i, lengths[slot] - 1)
								   : (i / suffix[mode == 2 ? slot : 8 - slot]) % lengths[slot];
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
	"Persisted Material UV Sphere Transform GetData retains all controls and ordered local chain",
	"[imagegraph][mesh_sphere_uv]"
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
		 {{"horizontal_slices", int64_t{3}},
		  {"vertical_slices", int64_t{5}},
		  {"projection", EnumValue{1}},
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
	Geometry(mesh, 3, 5, true, 1);
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
	"UV Sphere material retains owned float textures normals maps and every descriptor field",
	"[imagegraph][mesh_sphere_uv]"
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
	m.Normal = Image{1, 1, {128, 128, 255, 255}};
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
	"Persisted Surface source getter clones default UV Sphere material independently",
	"[imagegraph][mesh_sphere_uv]"
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
	"UV Sphere shared ledger admits retained material clone and getter scratch at exact peak atomically",
	"[imagegraph][mesh_sphere_uv]"
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
				{"attribute_array_process", EnumValue{0}},
				{"horizontal_slices", 3.5},
				{"vertical_slices", 3.5},
				{"smooth_normal", .75}
			};
			context.ValueViews.emplace_back("material", &input);
			const bool okay = detail::RunProcessorBatch(context, detail::FindExecutor(SPHERE));
			INFO(context.FailurePort << ':' << context.FailureMessage);
			CHECK(okay == accepted);
			if (accepted) {
				REQUIRE(context.OutputValues.size() == 1);
				const auto &mesh = std::get<MeshValue3D>(context.OutputValues[0].Data);
				CHECK(detail::ValidMeshPayload(mesh));
				CHECK(mesh.Data->Parts[0].Vertices.size() == 96);
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
	"UV Sphere raw integral and fractional slice domains retain denominator or named gaps",
	"[imagegraph][mesh_sphere_uv]"
) {
	const auto raw = RunNode(
		SPHERE, {}, {{"horizontal_slices", int64_t{3}}, {"vertical_slices", 3.5}, {"smooth_normal", true}}
	);
	INFO(raw.Message);
	REQUIRE(raw.Ok);
	Geometry(std::get<MeshValue3D>(*raw.OutputValue("mesh")), 3, 3.5, true);
	const auto below = RunNode(
		SPHERE,
		{},
		{{"horizontal_slices", int64_t{1}}, {"vertical_slices", int64_t{4}}, {"smooth_normal", true}}
	);
	REQUIRE(below.Ok);
	Geometry(std::get<MeshValue3D>(*below.OutputValue("mesh")), 1, 4, true);
	const auto fractional = RunNode(SPHERE, {}, {{"horizontal_slices", 2.5}});
	CHECK_FALSE(fractional.Ok);
	CHECK(fractional.Code == Status::UnsupportedExecution);
	CHECK(fractional.Port == "horizontal_slices");
	CHECK(fractional.Values.empty());
	const auto fractionalProjection = RunNode(SPHERE, {}, {{"projection", .5}});
	CHECK_FALSE(fractionalProjection.Ok);
	CHECK(fractionalProjection.Code == Status::UnsupportedExecution);
	CHECK(fractionalProjection.Port == "projection");
	const auto unused =
		RunNode(SPHERE, {}, {{"horizontal_slices", 2.5}, {"vertical_slices", 0.0}, {"projection", .5}});
	REQUIRE(unused.Ok);
	CHECK(std::get<MeshValue3D>(*unused.OutputValue("mesh")).Data->Parts[0].Vertices.empty());
	for (const auto port : {"horizontal_slices", "vertical_slices"}) {
		const auto zero = RunNode(SPHERE, {}, {{port, 0.0}});
		REQUIRE(zero.Ok);
		const auto &mesh = std::get<MeshValue3D>(*zero.OutputValue("mesh"));
		CHECK(mesh.Data->Parts[0].Vertices.empty());
		CHECK(mesh.Data->Edges.empty());
		CHECK(detail::ValidMeshPayload(mesh));
	}
}
TEST_CASE(
	"UV Sphere invalid controls count caps transforms material refuse atomically",
	"[imagegraph][mesh_sphere_uv]"
) {
	for (const auto port : {"horizontal_slices", "vertical_slices"})
		for (const auto bad :
			 {-1.0, std::numeric_limits<double>::infinity(), std::numeric_limits<double>::quiet_NaN()}) {
			const auto run = RunNode(SPHERE, {}, {{port, bad}});
			CHECK_FALSE(run.Ok);
			CHECK(run.Code == Status::InvalidValue);
			CHECK(run.Port == port);
			CHECK(run.Values.empty());
		}
	const auto cap =
		RunNode(SPHERE, {}, {{"horizontal_slices", int64_t{4096}}, {"vertical_slices", int64_t{4096}}});
	CHECK_FALSE(cap.Ok);
	CHECK(cap.Code == Status::LimitExceeded);
	CHECK(cap.Port == "mesh");
	CHECK(cap.Values.empty());
	const auto axisCap = RunNode(SPHERE, {}, {{"horizontal_slices", int64_t{4097}}});
	CHECK_FALSE(axisCap.Ok);
	CHECK(axisCap.Code == Status::LimitExceeded);
	CHECK(axisCap.Port == "horizontal_slices");
	Geometry(
		Mesh({{"horizontal_slices", int64_t{22}}, {"vertical_slices", int64_t{31}}, {"smooth_normal", true}}),
		22,
		31,
		true
	);
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
	MaterialValue3D bad;
	bad.Edit().Diffuse = std::numeric_limits<double>::infinity();
	const auto material = RunNode(SPHERE, {}, {{"material", bad}});
	CHECK_FALSE(material.Ok);
	CHECK(material.Code == Status::InvalidValue);
	CHECK(material.Port == "material");
	CHECK(material.Values.empty());
}

TEST_CASE(
	"UV Sphere finite raw vertical denominator arithmetic overflow refuses atomically",
	"[imagegraph][mesh_sphere_uv]"
) {
	// One loop iteration is source-defined, but the endpoint longitude exceeds finite double range.
	for (bool smooth : {false, true}) {
		const auto run = RunNode(
			SPHERE,
			{},
			{{"horizontal_slices", int64_t{8}},
			 {"vertical_slices", std::numeric_limits<double>::denorm_min()},
			 {"smooth_normal", smooth}}
		);
		INFO(run.Message);
		CHECK_FALSE(run.Ok);
		CHECK(run.Code == Status::InvalidValue);
		CHECK(run.Port == "mesh");
		CHECK(run.Values.empty());
		CHECK(run.Images.empty());
	}
}
