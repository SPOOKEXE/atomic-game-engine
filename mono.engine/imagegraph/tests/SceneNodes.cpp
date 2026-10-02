#include "../src/ScenePayload.hpp"
#include "NodeHarness.hpp"

#include <engine/testing/Suite.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

TEST_SUITE_ID("engine.imagegraph.scene_nodes")
using namespace engine::imagegraph;
using imagegraph_test::RunNode;

TEST_CASE("3D light preserves source controls point bias and inactive result", "[imagegraph][scene_nodes]") {
	const auto point = RunNode(
		"pc.3_d_light_point",
		{},
		{{"position", Vector3{1, 2, 3}}, {"intensity", .25}, {"radius", 7.0}, {"cast_shadow", true}}
	);
	INFO(point.Message);
	REQUIRE(point.Ok);
	const auto &light = std::get<LightValue3D>(*point.OutputValue("light"));
	REQUIRE(light.Data);
	CHECK(light.Data->Kind == LightKind3D::Point);
	CHECK((light.Data->Transform.Position == Vector3{1, 2, 3}));
	CHECK(light.Data->Intensity == .25);
	CHECK(light.Data->Radius == 7);
	CHECK(light.Data->ShadowBias == 1);
	CHECK(light.Data->CastShadow);
	CHECK(light.Data->ShadowMapSize == 1024);
	const auto inactive = RunNode("pc.3_d_light_point", {}, {{"active", false}});
	REQUIRE(inactive.Ok);
	CHECK_FALSE(std::get<LightValue3D>(*inactive.OutputValue("light")).Data);
	const auto directional = RunNode("pc.3_d_light_directional", {}, {{"position", Vector3{0, 0, 1}}});
	REQUIRE(directional.Ok);
	const auto &sun = std::get<LightValue3D>(*directional.OutputValue("light"));
	CHECK(sun.Data->Kind == LightKind3D::Directional);
	CHECK(sun.Data->ShadowBias == .01);
	CHECK(sun.Data->ShadowMapScale == 16);
	CHECK(std::abs(sun.Data->Transform.Rotation.Y + std::sqrt(.5)) < 1e-12);
}

TEST_CASE("3D scene keeps ordered owned mesh light and nested groups", "[imagegraph][scene_nodes]") {
	const auto plane = RunNode("pc.3_d_mesh_plane", {});
	REQUIRE(plane.Ok);
	const auto light = RunNode("pc.3_d_light_point", {});
	REQUIRE(light.Ok);
	Node node{"scene", "pc.3_d_scene", "", {}, {}};
	node.DynamicInputs = {
		{"first", ValueType::Mesh, std::nullopt}, {"second", ValueType::Mesh, std::nullopt}
	};
	const auto *entry = FindCatalogueEntry(node.Type);
	REQUIRE(entry);
	EvaluationRequest request;
	detail::NodeContext context(node, *entry, request);
	context.ByteBudget = Limits::MaximumEvaluationBytes;
	context.Values.emplace_back("first", *plane.OutputValue("mesh"));
	context.Values.emplace_back("second", *light.OutputValue("light"));
	const auto executor = detail::FindExecutor(node.Type);
	REQUIRE(executor);
	REQUIRE(executor(context));
	REQUIRE(context.OutputValues.size() == 1);
	const auto &scene = std::get<SceneValue3D>(context.OutputValues.front().Data);
	REQUIRE(scene.Data);
	REQUIRE(scene.Data->Objects.size() == 2);
	CHECK(std::holds_alternative<MeshValue3D>(scene.Data->Objects[0].Data));
	CHECK(std::holds_alternative<LightValue3D>(scene.Data->Objects[1].Data));
	CHECK(detail::ValidScenePayload(scene));
	SceneValue3D copy = scene;
	std::get<MeshValue3D>(copy.Data->Objects[0].Data).Data->Parts[0].Vertices[0].Position.X = 99;
	CHECK(std::get<MeshValue3D>(scene.Data->Objects[0].Data).Data->Parts[0].Vertices[0].Position.X != 99);
	SceneValue3D nested;
	nested.Data.emplace().Objects.push_back({scene.Data});
	CHECK(detail::ValidScenePayload(nested));
	CHECK(detail::SceneStorageBytes<false>(nested) > detail::SceneStorageBytes<false>(scene));
}

TEST_CASE(
	"Scene transform applies ordered rows to mesh light and child group transforms",
	"[imagegraph][scene_nodes]"
) {
	const auto plane = RunNode("pc.3_d_mesh_plane", {});
	REQUIRE(plane.Ok);
	const auto point = RunNode("pc.3_d_light_point", {});
	REQUIRE(point.Ok);
	SceneValue3D scene;
	scene.Data.emplace().Objects = {
		{std::get<MeshValue3D>(*plane.OutputValue("mesh"))},
		{std::get<LightValue3D>(*point.OutputValue("light"))}
	};
	ArrayValue positions;
	positions.ElementType = ValueType::Scalar;
	positions.Nested = {{1.0, 2.0, 3.0}, {4.0, 5.0, 6.0}};
	ArrayValue scales;
	scales.ElementType = ValueType::Scalar;
	scales.Nested = {{2.0, 3.0, 4.0}};
	const auto run = RunNode(
		"pc.3_d_transform_scene",
		{},
		{{"scene", scene},
		 {"position", positions},
		 {"scale", scales},
		 {"scaling_type", EnumValue{1}},
		 {"positioning_type", EnumValue{1}}}
	);
	INFO(run.Message);
	REQUIRE(run.Ok);
	const auto &output = std::get<SceneValue3D>(*run.OutputValue("scene"));
	const auto &mesh = std::get<MeshValue3D>(output.Data->Objects[0].Data);
	CHECK((mesh.Data->LocalTransforms.front().Position == Vector3{1, 2, 3}));
	CHECK((mesh.Data->LocalTransforms.front().Scale == Vector3{2, 3, 4}));
	const auto &light = std::get<LightValue3D>(output.Data->Objects[1].Data);
	CHECK((light.Data->Transform.Position == Vector3{4, 5, 6}));
	CHECK((light.Data->Transform.Scale == Vector3{1, 1, 1}));
	CHECK((
		std::get<MeshValue3D>(scene.Data->Objects[0].Data).Data->LocalTransforms.front().Position == Vector3{}
	));
}

TEST_CASE(
	"Persisted graph routes typed lights and meshes into source scene dynamic inputs",
	"[imagegraph][scene_nodes]"
) {
	Document document;
	document.FormatVersion = 7;
	Node scene{"scene", "pc.3_d_scene", "", {}, {}};
	scene.DynamicInputs = {
		{"first", ValueType::Mesh, std::nullopt}, {"second", ValueType::Mesh, std::nullopt}
	};
	document.Nodes = {
		{"plane", "pc.3_d_mesh_plane", "", {}, {}}, {"light", "pc.3_d_light_point", "", {}, {}}, scene
	};
	document.Links = {{"plane", "mesh", "scene", "first"}, {"light", "light", "scene", "second"}};
	document.Outputs = {{"out", "scene", "scene"}};
	Diagnostic diagnostic;
	Document restored;
	REQUIRE(Read(Write(document), restored, diagnostic) == Status::Ok);
	Plan plan;
	const auto compile = Compile(restored, plan, diagnostic);
	INFO(diagnostic.Message);
	REQUIRE(compile == Status::Ok);
	EvaluatedValue result;
	const auto status = EvaluateValue(restored, plan, "out", {}, result, diagnostic);
	INFO(diagnostic.NodeId << ':' << diagnostic.Port << ' ' << diagnostic.Message);
	REQUIRE(status == Status::Ok);
	const auto &output = std::get<SceneValue3D>(result.Data);
	REQUIRE(output.Data);
	REQUIRE(output.Data->Objects.size() == 2);
	CHECK(std::holds_alternative<MeshValue3D>(output.Data->Objects[0].Data));
	CHECK(std::holds_alternative<LightValue3D>(output.Data->Objects[1].Data));
}

TEST_CASE("3D mirror retains reflected and original wrapper draw semantics", "[imagegraph][scene_nodes]") {
	const auto plane = RunNode("pc.3_d_mesh_plane", {});
	REQUIRE(plane.Ok);
	const auto mirrored = RunNode(
		"pc.3_d_mirror",
		{},
		{{"mesh", *plane.OutputValue("mesh")},
		 {"axis", int64_t(1)},
		 {"position", Vector3{2, 3, 4}},
		 {"show_original", true}}
	);
	INFO(mirrored.Message);
	REQUIRE(mirrored.Ok);
	const auto &mesh = std::get<MeshValue3D>(*mirrored.OutputValue("mesh"));
	REQUIRE(mesh.Data);
	REQUIRE(mesh.Data->LocalTransforms.size() == 2);
	const auto &wrapper = mesh.Data->LocalTransforms.front();
	CHECK(wrapper.Mirror);
	CHECK(wrapper.ShowOriginal);
	CHECK((wrapper.Scale == Vector3{1, -1, 1}));
	CHECK((wrapper.Position == Vector3{0, 6, 0}));
	const auto &original = std::get<MeshValue3D>(*plane.OutputValue("mesh"));
	CHECK(mesh.Data->Parts == original.Data->Parts);
}

TEST_CASE(
	"3D point affector uses source sphere shell and signed plane falloff", "[imagegraph][scene_nodes]"
) {
	const auto sphere = RunNode(
		"pc.3_d_point_affector",
		{},
		{{"points", Vector3{.5, 0, 0}},
		 {"initial_value", Vector3{10, 20, 30}},
		 {"final_value", Vector3{0, 0, 0}}}
	);
	INFO(sphere.Message);
	REQUIRE(sphere.Ok);
	CHECK((std::get<Vector3>(*sphere.OutputValue("output")) == Vector3{5, 10, 15}));
	const auto plane = RunNode(
		"pc.3_d_point_affector",
		{},
		{{"shape", int64_t(1)},
		 {"points", Vector3{0, 0, -1}},
		 {"initial_value", Vector3{10, 20, 30}},
		 {"final_value", Vector3{1, 2, 3}}}
	);
	REQUIRE(plane.Ok);
	CHECK((std::get<Vector3>(*plane.OutputValue("output")) == Vector3{1, 2, 3}));
}
TEST_CASE(
	"3D repeat uses source one based scatter seeds and preserves wrapped object ownership",
	"[imagegraph][scene_nodes]"
) {
	MeshValue3D mesh;
	auto &data = mesh.Data.emplace();
	data.LocalTransforms.emplace_back();
	data.Materials.emplace_back();
	data.Parts.push_back(
		{{{{0, 0, 0}, {0, 0, 1}, {0, 0}}, {{1, 0, 0}, {0, 0, 1}, {1, 0}}, {{0, 1, 0}, {0, 0, 1}, {0, 1}}},
		 0,
		 {}}
	);
	ArrayValue scatter{ValueType::Scalar, {10.0, 20.0, 0.0, 0.0, 0.0, 0.0}};
	auto run = RunNode(
		"pc.3_d_repeat",
		{},
		{{"objects", mesh}, {"seed", 12345.0}, {"amount", int64_t(2)}, {"position_scatter", scatter}}
	);
	INFO(run.Message);
	REQUIRE(run.Ok);
	const auto &scene = std::get<SceneValue3D>(*run.OutputValue("scene"));
	REQUIRE(scene.Data);
	REQUIRE(scene.Data->Objects.size() == 2);
	const auto &first = std::get<OwnedPayload3D<SceneData3D>>(scene.Data->Objects[0].Data);
	const auto &second = std::get<OwnedPayload3D<SceneData3D>>(scene.Data->Objects[1].Data);
	CHECK(first->Transform.Position.X == 15.377437060408964);
	CHECK(second->Transform.Position.X == Catch::Approx(16.93394841343814).margin(1e-13));
	REQUIRE(first->Objects.size() == 1);
	CHECK(std::get<MeshValue3D>(first->Objects[0].Data) == mesh);
	CHECK(mesh.Data->LocalTransforms.front().Position.X == 0);
	auto huge = RunNode("pc.3_d_repeat", {}, {{"objects", mesh}, {"amount", int64_t(4096)}});
	CHECK_FALSE(huge.Ok);
	CHECK(huge.Code == Status::LimitExceeded);
}
TEST_CASE(
	"3D repeat generates source grid and circular transforms without changing mesh vertices",
	"[imagegraph][scene_nodes]"
) {
	MeshValue3D mesh;
	mesh.Data.emplace().LocalTransforms.emplace_back();
	auto grid = RunNode(
		"pc.3_d_repeat", {}, {{"objects", mesh}, {"pattern", EnumValue{1}}, {"grid", Vector3{2, 2, 1}}}
	);
	INFO(grid.Message);
	REQUIRE(grid.Ok);
	const auto &scene = std::get<SceneValue3D>(*grid.OutputValue("scene"));
	REQUIRE(scene.Data->Objects.size() == 4);
	const auto &last = std::get<OwnedPayload3D<SceneData3D>>(scene.Data->Objects[3].Data);
	CHECK((last->Transform.Position == Vector3{1, 1, 0}));
	auto circle = RunNode(
		"pc.3_d_repeat",
		{},
		{{"objects", mesh},
		 {"pattern", EnumValue{2}},
		 {"amount", int64_t(2)},
		 {"shift_position", Vector3{}},
		 {"look_at_center", 1.0}}
	);
	INFO(circle.Message);
	REQUIRE(circle.Ok);
	const auto &circular = std::get<SceneValue3D>(*circle.OutputValue("scene"));
	REQUIRE(circular.Data->Objects.size() == 2);
	const auto &opposite = std::get<OwnedPayload3D<SceneData3D>>(circular.Data->Objects[1].Data);
	CHECK(opposite->Transform.Position.X == -1);
	CHECK(std::abs(opposite->Transform.Position.Y) < 1e-12);
	CHECK(std::abs(opposite->Transform.Rotation.Z + 1) < 1e-12);
}
