#include "NodeHarness.hpp"

#include <engine/testing/Suite.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

TEST_SUITE_ID("engine.imagegraph.sourcemesh2dnodes")
TEST_DEPENDS("engine.imagegraph.catalogue")
namespace {
	using namespace engine::imagegraph;
	const MeshData2D &Mesh(const imagegraph_test::NodeRun &run) {
		const Value *value = run.OutputValue("mesh");
		REQUIRE(value != nullptr);
		const auto *mesh = std::get_if<MeshValue2D>(value);
		REQUIRE(mesh != nullptr);
		REQUIRE(bool(mesh->Data));
		return *mesh->Data;
	}
}
TEST_CASE(
	"source lattice preserves vertex UV and exact triangle edge and quad ordering", "[imagegraph][mesh2d]"
) {
	using namespace imagegraph_test;
	const auto run = RunNode(
		"pc.mesh_create_lattice",
		{},
		{{"area", Area{5, 7, 2, 3}}, {"area_unit", EnumValue{0}}, {"sample", Vector2{2, 1}}, {"quad", true}}
	);
	INFO(run.Message);
	REQUIRE(run.Ok);
	const auto &mesh = Mesh(run);
	REQUIRE(mesh.Simulation.Points.size() == 6);
	CHECK(mesh.Simulation.Points[0].Position == Vector2{3, 4});
	CHECK(mesh.Simulation.Points[5].Position == Vector2{7, 10});
	CHECK(mesh.Simulation.Points[1].UV == Vector2{.5, 0});
	CHECK(mesh.Triangles == std::vector<std::array<uint32_t, 3>>{{0, 1, 3}, {3, 1, 4}, {1, 2, 4}, {4, 2, 5}});
	CHECK(mesh.Quads == std::vector<std::array<uint32_t, 2>>{{0, 1}, {2, 3}});
	REQUIRE(mesh.Simulation.Edges.size() == 7);
	CHECK(mesh.Simulation.Edges[0].First == 0);
	CHECK(mesh.Simulation.Edges[0].Second == 3);
	CHECK(mesh.Simulation.Edges[3].First == 0);
	CHECK(mesh.Simulation.Edges[3].Second == 1);
	CHECK(mesh.Center == Vector2{5, 7});
	CHECK(mesh.Bounds == std::array<double, 4>{3, 4, 7, 10});
	CHECK_FALSE(mesh.Verlet);
}
TEST_CASE(
	"source mesh transform uses center relative anchor and plain point clone semantics",
	"[imagegraph][mesh2d]"
) {
	using namespace imagegraph_test;
	const auto lattice = RunNode(
		"pc.mesh_create_lattice",
		{},
		{{"area", Area{5, 7, 2, 3}}, {"area_unit", EnumValue{0}}, {"sample", Vector2{1, 1}}, {"quad", true}}
	);
	REQUIRE(lattice.Ok);
	const auto transformed = RunNode(
		"pc.mesh_transform",
		{},
		{{"mesh", *lattice.OutputValue("mesh")},
		 {"position", Vector2{1, 2}},
		 {"rotation", 90.0},
		 {"scale", Vector2{2, 1}}}
	);
	INFO(transformed.Message);
	REQUIRE(transformed.Ok);
	const auto &mesh = Mesh(transformed);
	CHECK(mesh.Simulation.Points[0].Position.X == Catch::Approx(3));
	CHECK(mesh.Simulation.Points[0].Position.Y == Catch::Approx(13));
	CHECK(mesh.Simulation.Points[0].UV == Vector2{});
	CHECK(mesh.Quads.empty());
	CHECK(mesh.Center.X == Catch::Approx(6));
	CHECK(mesh.Center.Y == Catch::Approx(9));
	CHECK(Mesh(lattice).Simulation.Points[0].Position == Vector2{3, 4});
}
TEST_CASE("path mesh supports pinned ear fan and native Delaunay triangulation", "[imagegraph][mesh2d]") {
	using namespace imagegraph_test;
	Path2D path;
	path.Loop = true;
	for (const auto point : std::array<Vector2, 4>{{{0, 0}, {10, 0}, {10, 10}, {0, 10}}})
		path.Anchors.push_back({{point.X, point.Y, 0, 0, 0, 0}});
	for (int64_t algorithm = 0; algorithm < 3; ++algorithm) {
		const auto run = RunNode(
			"pc.mesh_create_path",
			{},
			{{"path", path}, {"sample", int64_t{1}}, {"algorithm", EnumValue{algorithm}}}
		);
		INFO(run.Message);
		REQUIRE(run.Ok);
		const auto &mesh = Mesh(run);
		CHECK(mesh.Triangles.size() == (algorithm == 1 ? 4 : 2));
		CHECK(mesh.Simulation.Points.size() == (algorithm == 1 ? 5 : 4));
		CHECK(mesh.Center.X == Catch::Approx(5));
		CHECK(mesh.Center.Y == Catch::Approx(5));
		CHECK(mesh.Simulation.Edges.empty());
		for (const auto &triangle : mesh.Triangles) {
			const auto a = mesh.Simulation.Points[triangle[0]].Position,
					   b = mesh.Simulation.Points[triangle[1]].Position,
					   c = mesh.Simulation.Points[triangle[2]].Position;
			CHECK((b.X - a.X) * (c.Y - a.Y) - (c.X - a.X) * (b.Y - a.Y) <= 0);
		}
	}
}
TEST_CASE("source lattice refuses oversized topology before construction", "[imagegraph][mesh2d]") {
	const auto run =
		imagegraph_test::RunNode("pc.mesh_create_lattice", {}, {{"sample", Vector2{4096, 4096}}});
	CHECK_FALSE(run.Ok);
	CHECK(run.Code == engine::imagegraph::Status::LimitExceeded);
	CHECK(run.Port == "sample");
}

TEST_CASE(
	"source polygon constructors publish mesh path and raster for every selectable shape",
	"[imagegraph][mesh2d]"
) {
	using namespace imagegraph_test;
	const std::array<std::pair<int64_t, size_t>, 16> shapes{
		{{0, 2},
		 {1, 4},
		 {2, 2},
		 {3, 2},
		 {5, 16},
		 {6, 32},
		 {7, 32},
		 {8, 28},
		 {9, 16},
		 {10, 16},
		 {12, 16},
		 {13, 32},
		 {14, 10},
		 {16, 34},
		 {18, 30},
		 {19, 36}}
	};
	for (const auto &[shape, count] : shapes) {
		INFO(shape);
		const auto run = RunNode(
			"pc.shape_polygon",
			{},
			{{"dimension", Vector2{40, 40}},
			 {"dimension_unit", EnumValue{0}},
			 {"position", Vector2{20, 20}},
			 {"position_unit", EnumValue{0}},
			 {"scale", Vector2{16, 16}},
			 {"scale_unit", EnumValue{0}},
			 {"shape", EnumValue{shape}}}
		);
		INFO(run.Message);
		REQUIRE(run.Ok);
		const auto &mesh = Mesh(run);
		CHECK(mesh.Triangles.size() == count);
		CHECK(mesh.Simulation.Points.size() == count * 3);
		CHECK(mesh.Simulation.Edges.empty());
		REQUIRE(run.OutputValue("path"));
		const auto *path = std::get_if<Path2D>(run.OutputValue("path"));
		REQUIRE(path);
		CHECK_FALSE(path->Anchors.empty());
		CHECK(run.Output().Width == 40);
		CHECK(run.Output().Height == 40);
		CHECK(FiniteSurfaceSamples(run.Output()));
	}
}

TEST_CASE(
	"source polygon preserves duplicate triangle vertices and declared solid background",
	"[imagegraph][mesh2d]"
) {
	using namespace imagegraph_test;
	const auto run = RunNode(
		"pc.shape_polygon",
		{},
		{{"dimension", Vector2{4, 4}},
		 {"dimension_unit", EnumValue{0}},
		 {"position", Vector2{2, 2}},
		 {"position_unit", EnumValue{0}},
		 {"scale", Vector2{1, 1}},
		 {"scale_unit", EnumValue{0}},
		 {"shape_color", Colour{255, 0, 0, 0}},
		 {"background", EnumValue{1}},
		 {"bg_color", Colour{0, 255, 0, 0}}}
	);
	INFO(run.Message);
	REQUIRE(run.Ok);
	const auto &mesh = Mesh(run);
	REQUIRE(mesh.Simulation.Points.size() == 6);
	CHECK(mesh.Simulation.Points[0].Position == Vector2{1, 1});
	CHECK(mesh.Simulation.Points[1].Position == Vector2{1, 3});
	CHECK(mesh.Simulation.Points[2].Position == Vector2{3, 1});
	CHECK(mesh.Simulation.Points[3].Position == mesh.Simulation.Points[2].Position);
	CHECK(mesh.Center == Vector2{2, 2});
	SurfacePixel sample{};
	REQUIRE(LoadSurfacePixel(run.Output(), 1, 1, sample));
	CHECK(sample == SurfacePixel{1, 0, 0, 1});
	REQUIRE(LoadSurfacePixel(run.Output(), 0, 0, sample));
	CHECK(sample == SurfacePixel{0, 1, 0, 1});
}

TEST_CASE(
	"source mesh warp publishes sampled UV topology and keeps black content filtering", "[imagegraph][mesh2d]"
) {
	using namespace imagegraph_test;
	const auto white = MakeImage(2, 2, std::vector<uint8_t>(16, 255));
	const auto full =
		RunNode("pc.mesh_warp", {{"surface_in", &white}}, {{"sample", int64_t{1}}, {"full_mesh", true}});
	INFO(full.Message);
	REQUIRE(full.Ok);
	const auto *value = full.OutputValue("mesh_data");
	REQUIRE(value);
	const auto &mesh = *std::get<MeshValue2D>(*value).Data;
	CHECK(mesh.Warp);
	CHECK_FALSE(mesh.Verlet);
	CHECK(mesh.Triangles == std::vector<std::array<uint32_t, 3>>{{0, 1, 2}, {1, 2, 3}});
	CHECK(mesh.Simulation.Points[3].UV == Vector2{1, 1});
	CHECK(mesh.Simulation.Edges.empty());
	CHECK(mesh.Center == Vector2{});
	CHECK(full.Output().Pixels == white.Pixels);
	const auto black = MakeImage(2, 2, {0, 0, 0, 255, 0, 0, 0, 255, 0, 0, 0, 255, 0, 0, 0, 255});
	const auto filtered = RunNode("pc.mesh_warp", {{"surface_in", &black}}, {{"sample", int64_t{1}}});
	REQUIRE(filtered.Ok);
	CHECK(std::get<MeshValue2D>(*filtered.OutputValue("mesh_data")).Data->Simulation.Points.empty());
	CHECK(filtered.Output().Pixels == black.Pixels);
}
TEST_CASE("source mesh warp applies durable pins and seven field Move controls", "[imagegraph][mesh2d]") {
	using namespace engine::imagegraph;
	using namespace imagegraph_test;
	const auto white = MakeImage(2, 2, std::vector<uint8_t>(16, 255));
	Node node{"warp", "pc.mesh_warp", "", {}, {}};
	node.DynamicInputs.push_back({"control_point_0", ValueType::Struct, std::nullopt});
	node.SourceProperties = {
		{"pin", ArrayValue{ValueType::Integer, {int64_t{0}}}},
		{"control_point_0", ArrayValue{ValueType::Scalar, {0., 0., 0., 1., 0., 100., 100.}}}
	};
	EvaluationRequest request;
	detail::NodeContext context(node, *FindCatalogueEntry(node.Type), request);
	context.ByteBudget = Limits::MaximumEvaluationBytes;
	context.Images.emplace_back("surface_in", &white);
	context.Values = {{"sample", int64_t{1}}, {"full_mesh", true}, {"attribute_iteration", int64_t{4}}};
	REQUIRE(detail::FindExecutor(node.Type)(context));
	REQUIRE(context.FailureCode == Status::Ok);
	const auto iterator =
		std::find_if(context.OutputValues.begin(), context.OutputValues.end(), [](const auto &value) {
			return value.Port == "mesh_data";
		});
	REQUIRE(iterator != context.OutputValues.end());
	const auto &mesh = *std::get<MeshValue2D>(iterator->Data).Data;
	CHECK(mesh.Simulation.Points[0].Position == Vector2{0, 0});
	CHECK(mesh.Simulation.Points[1].Position.X > 2.99);
	CHECK(mesh.Simulation.Points[1].UV == Vector2{1, 0});
	const auto transformed =
		RunNode("pc.mesh_transform", {}, {{"mesh", iterator->Data}, {"position", Vector2{1, 2}}});
	REQUIRE(transformed.Ok);
	CHECK(Mesh(transformed).Simulation.Points[1].UV == Vector2{1, 0});
	CHECK_FALSE(Mesh(transformed).Simulation.Points[0].Pin);
}
