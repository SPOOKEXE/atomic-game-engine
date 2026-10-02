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

TEST_SUITE_ID("engine.imagegraph.mesh_torus")
using namespace engine::imagegraph;
using imagegraph_test::RunNode;
namespace {
	constexpr std::string_view TORUS = "pc.3_d_mesh_torus";
	MeshValue3D Mesh(std::vector<AuthoredValue> values = {}) {
		const auto *entry = FindCatalogueEntry(TORUS);
		REQUIRE(entry);
		const Node node{"torus", std::string(TORUS), "", {}, {}};
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
		const bool okay = detail::RunProcessorBatch(context, detail::FindExecutor(TORUS));
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
		double toroidal = 16,
		double poloidal = 8,
		double major = 1,
		double minor = .2,
		double angleT = 0,
		double angleP = 0,
		double twist = 0,
		bool smooth = false
	) {
		REQUIRE(mesh.Data);
		const auto &data = *mesh.Data;
		const size_t nt = size_t(std::ceil(toroidal)), np = size_t(std::ceil(poloidal));
		REQUIRE(data.Parts.size() == 1);
		REQUIRE(data.Materials.size() == 1);
		CHECK(data.Parts[0].MaterialIndex == 0);
		REQUIRE(data.Parts[0].Vertices.size() == 6 * nt * np);
		REQUIRE(data.Edges.size() == 4 * nt * np);
		// The documented integer-neighborhood rule applies to each lengthdir result before products.
		const auto snap = [](double value) {
			const double lower = std::floor(value), upper = lower + 1;
			if (value - lower <= .0001) return lower;
			if (upper - value <= .0001) return upper;
			return value;
		};
		const auto ring = [&](double turn, double phase) -> Vector3 {
			const double t = (turn * 360 + angleT) * std::numbers::pi / 180,
						 a = phase * std::numbers::pi / 180;
			const double radial = major + snap(minor * std::cos(a));
			return {radial * snap(std::cos(t)), radial * snap(-std::sin(t)), snap(-minor * std::sin(a))};
		};
		for (size_t i = 0; i < nt; ++i)
			for (size_t j = 0; j < np; ++j) {
				INFO("i=" << i << " j=" << j);
				const double t0 = double(i) / toroidal, t1 = double(i + 1) / toroidal,
							 p0 = double(j) / poloidal * 360 + angleP,
							 p1 = double(j + 1) / poloidal * 360 + angleP;
				const std::array<Vector3, 4> p{
					{ring(t0, p0),
					 ring(t1, p0 + twist / poloidal * 360),
					 ring(t1, p1 + twist / poloidal * 360),
					 ring(t0, p1)}
				};
				const double a0 = (t0 * 360 + angleT) * std::numbers::pi / 180,
							 a1 = (t1 * 360 + angleT) * std::numbers::pi / 180;
				const std::array<Vector3, 4> centers{
					{{major * snap(std::cos(a0)), major * snap(-std::sin(a0)), 0},
					 {major * snap(std::cos(a1)), major * snap(-std::sin(a1)), 0},
					 {major * snap(std::cos(a1)), major * snap(-std::sin(a1)), 0},
					 {major * snap(std::cos(a0)), major * snap(-std::sin(a0)), 0}}
				};
				std::array<Vector3, 4> n{};
				if (smooth)
					for (size_t k = 0; k < 4; ++k)
						n[k] = {p[k].X - centers[k].X, p[k].Y - centers[k].Y, p[k].Z};
				else {
					Vector3 sum{};
					for (size_t k = 0; k < 4; ++k) {
						sum.X += p[k].X;
						sum.Y += p[k].Y;
						sum.Z += p[k].Z;
					}
					n.fill(
						{sum.X / 4 - (centers[0].X + centers[1].X) / 2,
						 sum.Y / 4 - (centers[0].Y + centers[1].Y) / 2,
						 sum.Z / 4}
					);
				}
				const std::array<Vector2, 4> uv{
					{{1 - t0, 1 - double(j) / poloidal},
					 {1 - t1, 1 - double(j) / poloidal},
					 {1 - t1, 1 - double(j + 1) / poloidal},
					 {1 - t0, 1 - double(j + 1) / poloidal}}
				};
				const std::array<size_t, 6> order{0, 2, 1, 0, 3, 2};
				for (size_t k = 0; k < 6; ++k)
					Vertex(
						data.Parts[0].Vertices[(i * np + j) * 6 + k], p[order[k]], n[order[k]], uv[order[k]]
					);
				for (size_t k = 0; k < 4; ++k) {
					Point(data.Edges[(i * np + j) * 4 + k].From, p[k]);
					Point(data.Edges[(i * np + j) * 4 + k].To, p[(k + 1) % 4]);
				}
			}
	}
}
TEST_CASE(
	"Torus defaults own all source ordered vertices normals UV tint and duplicated cell edges",
	"[imagegraph][mesh_torus]"
) {
	const auto mesh = Mesh();
	Geometry(mesh);
	CHECK(mesh.Data->LocalTransforms == std::vector<MeshTransform3D>{MeshTransform3D{}});
	CHECK(mesh.Data->Materials == std::vector<MaterialValue3D>(1));
	CHECK((mesh.Data->Parts[0].Vertices[0].Position == Vector3{1.2, 0, 0}));
	CHECK((mesh.Data->Parts[0].Vertices[0].UV == Vector2{1, 1}));
}
TEST_CASE(
	"Torus every control preserves unnormalized source normals angle degrees and constant percell twist",
	"[imagegraph][mesh_torus]"
) {
	for (bool smooth : {false, true}) {
		const auto mesh = Mesh(
			{{"toroidal_slices", int64_t{3}},
			 {"poloidal_slices", int64_t{4}},
			 {"toroidal_radius", -1.25},
			 {"poloidal_radius", -.3},
			 {"toroidal_angle", 31.0},
			 {"poloidal_angle", 17.0},
			 {"twist", 1.25},
			 {"smooth_normal", smooth}}
		);
		Geometry(mesh, 3, 4, -1.25, -.3, 31, 17, 1.25, smooth);
	}
	const auto smooth = Mesh({{"smooth_normal", true}});
	Point(smooth.Data->Parts[0].Vertices[0].Normal, {.2, 0, 0});
	const auto zero = Mesh({{"poloidal_radius", 0.0}});
	Geometry(zero, 16, 8, 1, 0);
}
TEST_CASE(
	"Torus source twist preserves discontinuous intercell seam and explicit endpoint UV duplication",
	"[imagegraph][mesh_torus]"
) {
	const auto mesh = Mesh({{"toroidal_slices", int64_t{4}}, {"poloidal_slices", int64_t{4}}, {"twist", .5}});
	Geometry(mesh, 4, 4, 1, .2, 0, 0, .5);
	const auto &v = mesh.Data->Parts[0].Vertices;
	CHECK(v[2].Position != v[24].Position);
	const auto untwisted = Mesh({{"toroidal_slices", int64_t{4}}, {"poloidal_slices", int64_t{4}}});
	Point(untwisted.Data->Parts[0].Vertices[2].Position, untwisted.Data->Parts[0].Vertices[24].Position);
	CHECK(untwisted.Data->Parts[0].Vertices.back().UV.X == 0);
	CHECK(untwisted.Data->Parts[0].Vertices.back().UV.Y == 0);
}
TEST_CASE(
	"Torus Int getters clamp and half even round while raw fractional kernel loops retain denominators",
	"[imagegraph][mesh_torus]"
) {
	CHECK(Mesh({{"toroidal_slices", 3.5}, {"poloidal_slices", 4.5}}).Data->Parts[0].Vertices.size() == 96);
	CHECK(Mesh({{"toroidal_slices", -1.5}, {"poloidal_slices", -.5}}).Data->Parts[0].Vertices.size() == 54);
	const auto raw = RunNode(TORUS, {}, {{"toroidal_slices", 3.25}, {"poloidal_slices", 3.5}});
	REQUIRE(raw.Ok);
	Geometry(std::get<MeshValue3D>(*raw.OutputValue("mesh")), 3.25, 3.5);
	CHECK(
		std::get<MeshValue3D>(*raw.OutputValue("mesh")).Data->Parts[0].Vertices.back().UV.X ==
		Catch::Approx(1 - 4 / 3.25)
	);
	CHECK(Mesh({{"smooth_normal", .5}}) == Mesh({{"smooth_normal", false}}));
	CHECK(Mesh({{"smooth_normal", .5001}}) == Mesh({{"smooth_normal", true}}));
}
TEST_CASE(
	"Torus persisted scalar control arrays follow every source thirteen slot schedule",
	"[imagegraph][mesh_torus]"
) {
	const std::array<std::string_view, 8> ports{
		"poloidal_slices",
		"toroidal_radius",
		"poloidal_radius",
		"material",
		"smooth_normal",
		"toroidal_angle",
		"poloidal_angle",
		"twist"
	};
	const std::array<size_t, 8> slots{5, 6, 7, 8, 9, 10, 11, 12};
	for (size_t c = 0; c < ports.size(); ++c)
		for (int64_t mode : {0, 1, 2, 3}) {
			Document doc;
			doc.FormatVersion = 9;
			Node torus{
				"torus",
				std::string(TORUS),
				"",
				{},
				{{"toroidal_slices", ArrayValue{ValueType::Integer, {int64_t{3}, int64_t{4}}}},
				 {"attribute_array_process", EnumValue{mode}}}
			};
			const std::array<double, 3> controls = c == 5 || c == 6	  ? std::array<double, 3>{31, 73, 140}
												   : c == 1 || c == 2 ? std::array<double, 3>{-.3, 0, 1.4}
												   : c == 7			  ? std::array<double, 3>{.25, .5, 1.25}
																	  : std::array<double, 3>{.1, .5, .9};
			if (c == 0)
				torus.Values.push_back(
					{std::string(ports[c]),
					 ArrayValue{ValueType::Integer, {int64_t{3}, int64_t{4}, int64_t{5}}}}
				);
			else if (c == 4)
				torus.Values.push_back(
					{std::string(ports[c]), ArrayValue{ValueType::Boolean, {false, true, false}}}
				);
			else if (c != 3)
				torus.Values.push_back(
					{std::string(ports[c]),
					 ArrayValue{ValueType::Scalar, {controls[0], controls[1], controls[2]}}}
				);
			doc.Nodes = {torus};
			if (c == 3) {
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
				doc.Links = {{"material", "material", "torus", "material"}};
			}
			doc.Outputs = {{"out", "torus", "mesh"}};
			const auto rows = std::get<ArrayValue>(Replay(doc));
			const size_t count = mode < 2 ? 3 : 6;
			REQUIRE(rows.Elements.size() == count);
			// Independent pinned source running table covers all thirteen original slots.
			std::array<size_t, 13> lengths{};
			lengths.fill(1);
			lengths[4] = 2;
			lengths[slots[c]] = 3;
			std::array<size_t, 13> running{};
			size_t product = 6;
			for (size_t s = 0; s < 13; ++s) {
				product /= lengths[s];
				running[s] = product;
			}
			for (size_t i = 0; i < count; ++i) {
				INFO("port=" << ports[c] << " mode=" << mode << " row=" << i);
				const auto index = [&](size_t slot) {
					return mode == 0   ? i % lengths[slot]
						   : mode == 1 ? std::min(i, lengths[slot] - 1)
									   : (i / running[mode == 2 ? slot : 12 - slot]) % lengths[slot];
				};
				const size_t a = index(4), b = index(slots[c]);
				const auto &mesh = std::get<MeshValue3D>(rows.Elements[i]);
				Geometry(
					mesh,
					3 + double(a),
					c == 0 ? 3 + double(b) : 8,
					c == 1 ? controls[b] : 1,
					c == 2 ? controls[b] : .2,
					c == 5 ? controls[b] : 0,
					c == 6 ? controls[b] : 0,
					c == 7 ? controls[b] : 0,
					c == 4 && b == 1
				);
				if (c == 3) CHECK(mesh.Data->Materials[0].Get().Diffuse == controls[b]);
			}
		}
}
TEST_CASE(
	"Persisted Torus linked transform arrays retain all source slot schedules without composition",
	"[imagegraph][mesh_torus]"
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
			{"torus", std::string(TORUS), "", {}, {{"attribute_array_process", EnumValue{mode}}}}
		};
		doc.Links = {
			{"positions", "vector", "torus", "position"},
			{"scales", "vector", "torus", "scale"},
			{"anchors", "vector", "torus", "anchor"},
			{"rotations", "array", "torus", "rotation"}
		};
		doc.Outputs = {{"out", "torus", "mesh"}};
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
	"Persisted Material Torus Transform GetData retains all controls and ordered local chain",
	"[imagegraph][mesh_torus]"
) {
	Document doc;
	doc.FormatVersion = 9;
	const MeshTransform3D local{{2, 3, 4}, {.1, .2, .3}, {.1, .2, .3, .4}, {-1, 2, .5}},
		outer{{-2, 5, 1}, {1, 2, 3}, {0, 0, 1, 0}, {2, -3, 4}};
	doc.Nodes = {
		{"material", "pc.3_d_material", "", {}, {{"diffuse", .25}}},
		{"torus",
		 std::string(TORUS),
		 "",
		 {},
		 {{"toroidal_slices", int64_t{3}},
		  {"poloidal_slices", int64_t{4}},
		  {"toroidal_radius", 1.25},
		  {"poloidal_radius", .3},
		  {"toroidal_angle", 31.0},
		  {"poloidal_angle", 17.0},
		  {"twist", 1.25},
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
		{"material", "material", "torus", "material"},
		{"torus", "mesh", "transform", "mesh"},
		{"transform", "mesh", "get", "mesh"}
	};
	doc.Outputs = {{"out", "transform", "mesh"}};
	const auto mesh = std::get<MeshValue3D>(Replay(doc));
	Geometry(mesh, 3, 4, 1.25, .3, 31, 17, 1.25, true);
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
	"Torus material retains owned float textures normals maps and every descriptor field",
	"[imagegraph][mesh_torus]"
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
	"Persisted Surface source getter clones default Torus material independently", "[imagegraph][mesh_torus]"
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
		{"torus", std::string(TORUS), "", {}, {}}
	};
	doc.Links = {{"surface", "surface_out", "torus", "material"}};
	doc.Outputs = {{"out", "torus", "mesh"}};
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
	"Torus shared ledger admits retained material clone and getter scratch at exact peak atomically",
	"[imagegraph][mesh_torus]"
) {
	MaterialValue3D material;
	material.Edit().Surface = Image{1, 1, {1, 2, 3, 255}};
	material.Edit().Surface->Pixels.reserve(512);
	const Value input = std::move(material);
	const auto *entry = FindCatalogueEntry(TORUS);
	REQUIRE(entry);
	const Node node{"torus", std::string(TORUS), "", {}, {}};
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
				{"toroidal_slices", 3.5},
				{"poloidal_slices", 3.5},
				{"smooth_normal", .75}
			};
			context.ValueViews.emplace_back("material", &input);
			const bool okay = detail::RunProcessorBatch(context, detail::FindExecutor(TORUS));
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
	"Torus malformed controls count bounds and actual arithmetic overflow refuse atomically",
	"[imagegraph][mesh_torus]"
) {
	const double infinity = std::numeric_limits<double>::infinity();
	for (const auto port :
		 {"toroidal_slices",
		  "poloidal_slices",
		  "toroidal_radius",
		  "poloidal_radius",
		  "toroidal_angle",
		  "poloidal_angle",
		  "twist"}) {
		const auto run = RunNode(TORUS, {}, {{port, infinity}});
		CHECK_FALSE(run.Ok);
		CHECK(run.Code == Status::InvalidValue);
		CHECK(run.Port == port);
		CHECK(run.Values.empty());
	}
	const auto cap =
		RunNode(TORUS, {}, {{"toroidal_slices", int64_t{4096}}, {"poloidal_slices", int64_t{4096}}});
	CHECK_FALSE(cap.Ok);
	CHECK(cap.Code == Status::LimitExceeded);
	CHECK(cap.Values.empty());
	CHECK(
		Mesh({{"toroidal_slices", int64_t{31}}, {"poloidal_slices", int64_t{22}}})
			.Data->Parts[0]
			.Vertices.size() == 4092
	);
	const auto negative = RunNode(TORUS, {}, {{"toroidal_slices", -1.0}});
	CHECK_FALSE(negative.Ok);
	CHECK(negative.Code == Status::InvalidValue);
	CHECK(negative.Port == "toroidal_slices");
	const auto zero = RunNode(TORUS, {}, {{"toroidal_slices", 0.0}});
	REQUIRE(zero.Ok);
	const auto &empty = std::get<MeshValue3D>(*zero.OutputValue("mesh"));
	CHECK(empty.Data->Parts[0].Vertices.empty());
	CHECK(empty.Data->Edges.empty());
	CHECK(detail::ValidMeshPayload(empty));
	const auto overflow = RunNode(
		TORUS,
		{},
		{{"toroidal_radius", std::numeric_limits<double>::max()},
		 {"poloidal_radius", std::numeric_limits<double>::max()}}
	);
	CHECK_FALSE(overflow.Ok);
	CHECK(overflow.Code == Status::InvalidValue);
	CHECK(overflow.Port == "mesh");
	CHECK(overflow.Values.empty());
	MaterialValue3D bad;
	bad.Edit().Diffuse = infinity;
	const auto invalid = RunNode(TORUS, {}, {{"material", bad}});
	CHECK_FALSE(invalid.Ok);
	CHECK(invalid.Code == Status::InvalidValue);
	CHECK(invalid.Port == "material");
	for (const auto port : {"position", "anchor", "scale"}) {
		const auto run = RunNode(TORUS, {}, {{port, Vector3{0, infinity, 1}}});
		CHECK_FALSE(run.Ok);
		CHECK(run.Code == Status::InvalidValue);
		CHECK(run.Port == port);
		CHECK(run.Values.empty());
	}
	const auto rotation = RunNode(TORUS, {}, {{"rotation", Quaternion{0, 0, infinity, 1}}});
	CHECK_FALSE(rotation.Ok);
	CHECK(rotation.Code == Status::InvalidValue);
	CHECK(rotation.Port == "rotation");
}

TEST_CASE(
	"Torus lengthdir snapping preserves tiny radii and both sides of integer neighborhoods",
	"[imagegraph][mesh_torus]"
) {
	const auto tiny = Mesh({{"poloidal_radius", .00005}});
	CHECK((tiny.Data->Parts[0].Vertices[0].Position == Vector3{1, 0, 0}));
	Geometry(tiny, 16, 8, 1, .00005);
	for (double minor : {.00005, .0002, .99995, 1.00005, .9998, -.99995})
		for (bool smooth : {false, true}) {
			INFO("minor=" << minor << " smooth=" << smooth);
			const auto mesh = Mesh(
				{{"toroidal_slices", int64_t{3}},
				 {"poloidal_slices", int64_t{4}},
				 {"poloidal_radius", minor},
				 {"toroidal_angle", .5},
				 {"twist", .25},
				 {"smooth_normal", smooth}}
			);
			Geometry(mesh, 3, 4, 1, minor, .5, 0, .25, smooth);
		}
	const auto inside = Mesh({{"poloidal_radius", .99995}, {"toroidal_angle", .5}});
	CHECK(inside.Data->Parts[0].Vertices[0].Position.X == 2);
	CHECK(
		inside.Data->Parts[0].Vertices[0].Position.Y ==
		Catch::Approx(-2 * std::sin(.5 * std::numbers::pi / 180)).margin(1e-12)
	);
	const auto outside = Mesh({{"poloidal_radius", .9998}});
	CHECK(outside.Data->Parts[0].Vertices[0].Position.X == Catch::Approx(1.9998).margin(1e-12));
}
