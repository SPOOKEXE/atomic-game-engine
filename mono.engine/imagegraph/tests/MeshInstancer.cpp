#include "../src/SourceGradient.hpp"
#include "../src/SourceQuaternion.hpp"
#include "NodeHarness.hpp"

#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>
TEST_SUITE_ID("engine.imagegraph.mesh_instancer")
using namespace engine::imagegraph;
using imagegraph_test::RunNode;
namespace {
	MeshValue3D BaseMesh() {
		MeshValue3D mesh;
		auto &data = mesh.Data.emplace();
		data.LocalTransforms.emplace_back();
		data.Materials.emplace_back();
		data.Parts.emplace_back();
		data.Parts.front().Vertices = {
			{{0, 0, 0}, {0, 0, 1}, {0, 0}}, {{1, 0, 0}, {0, 0, 1}, {1, 0}}, {{0, 1, 0}, {0, 0, 1}, {0, 1}}
		};
		return mesh;
	}
}
TEST_CASE(
	"Source instancer stores bounded f32 records with zero based source scatter seeds",
	"[imagegraph][mesh_instancer]"
) {
	auto mesh = BaseMesh();
	ArrayValue scatter{ValueType::Scalar, {10.0, 20.0, 0.0, 0.0, 0.0, 0.0}};
	auto run = RunNode(
		"pc.3_d_instancer",
		{},
		{{"mesh", mesh},
		 {"seed", 12345.0},
		 {"amounts", int64_t(2)},
		 {"position_scatter", scatter},
		 {"starting_rotation", Quaternion{0, 0, std::sqrt(.5), std::sqrt(.5)}}}
	);
	INFO(run.Message);
	REQUIRE(run.Ok);
	const auto &output = std::get<MeshValue3D>(*run.OutputValue("mesh"));
	REQUIRE(output.Data);
	CHECK(output.Data->Instanced);
	REQUIRE(output.Data->Instances.size() == 2);
	CHECK(output.Data->Instances[0].Fields[0] == float(17.990098701785364));
	CHECK(output.Data->Instances[1].Fields[0] == float(16.377437060408964));
	CHECK(output.Data->Instances[0].Fields[6] == 90);
	CHECK(output.Data->Instances[0].Fields[8] == 1);
	CHECK(output.Data->Instances[0].Fields[3] == 1);
	CHECK(output.Data->Parts[0].Vertices == mesh.Data->Parts[0].Vertices);
	CHECK(output.Data->Edges.empty());
	auto huge = RunNode("pc.3_d_instancer", {}, {{"mesh", mesh}, {"amounts", int64_t(4097)}});
	CHECK_FALSE(huge.Ok);
	CHECK(huge.Code == Status::LimitExceeded);
}
TEST_CASE(
	"Source instancer flattens parent positions retaining raw normals and object matrix",
	"[imagegraph][mesh_instancer]"
) {
	auto mesh = BaseMesh();
	mesh.Data->LocalTransforms.front().Position = {4, 0, 0};
	SceneValue3D group;
	auto &data = group.Data.emplace();
	data.Transform.Position = {10, 0, 0};
	data.Objects.push_back({mesh});
	auto run = RunNode("pc.3_d_instancer", {}, {{"mesh", group}, {"amounts", int64_t(2)}});
	INFO(run.Message);
	REQUIRE(run.Ok);
	const auto &output = std::get<MeshValue3D>(*run.OutputValue("mesh"));
	CHECK((output.Data->Parts[0].Vertices[0].Position == Vector3{10, 0, 0}));
	CHECK((output.Data->Parts[0].Vertices[0].Normal == Vector3{0, 0, 1}));
	CHECK((output.Data->InstanceObjectTransform.Position == Vector3{10, 0, 0}));
	CHECK((output.Data->LocalTransforms.front().Position == Vector3{}));
}
TEST_CASE("Source instancer palette ping pong repeats its last index once", "[imagegraph][mesh_instancer]") {
	auto mesh = BaseMesh();
	ArrayValue palette{
		ValueType::Colour, {Colour{10, 0, 0, 255}, Colour{20, 0, 0, 255}, Colour{30, 0, 0, 255}}
	};
	auto run = RunNode(
		"pc.3_d_instancer",
		{},
		{{"mesh", mesh},
		 {"amounts", int64_t(5)},
		 {"colors_per_index", palette},
		 {"colors_per_index_select", EnumValue{1}}}
	);
	INFO(run.Message);
	REQUIRE(run.Ok);
	const auto &output = std::get<MeshValue3D>(*run.OutputValue("mesh"));
	const float red[]{10 / 255.f, 20 / 255.f, 30 / 255.f, 30 / 255.f, 20 / 255.f};
	for (size_t i = 0; i < 5; ++i)
		CHECK(output.Data->Instances[i].Fields[3] == red[i]);
}
TEST_CASE(
	"Compiled primitive to instancer uses source getters and typed transport", "[imagegraph][mesh_instancer]"
) {
	Document document;
	document.FormatVersion = 9;
	document.Nodes = {
		{"cube", "pc.3_d_mesh_cube", "", {}, {}},
		{"instances", "pc.3_d_instancer", "", {}, {{"amounts", int64_t(2)}}}
	};
	document.Links = {{"cube", "mesh", "instances", "mesh"}};
	document.Outputs = {{"mesh", "instances", "mesh"}};
	Plan plan;
	Diagnostic diagnostic;
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	EvaluatedValue output;
	INFO(diagnostic.Message);
	REQUIRE(EvaluateValue(document, plan, "mesh", {}, output, diagnostic) == Status::Ok);
	const auto &mesh = std::get<MeshValue3D>(output.Data);
	REQUIRE(mesh.Data);
	CHECK(mesh.Data->Instanced);
	CHECK(mesh.Data->Instances.size() == 2);
	CHECK_FALSE(mesh.Data->Parts.empty());
}

TEST_CASE(
	"Instancer gradient cache uses all source keys and half even cache indices",
	"[imagegraph][mesh_instancer]"
) {
	Gradient gradient;
	gradient.Mode = 0;
	gradient.Keys = {{0, {0, 0, 0, 0}}, {1, {255, 255, 255, 255}}};
	CHECK(detail::SourceCachedGradient(gradient, 1.0 / 256) == Colour{0, 0, 0, 0});
	CHECK(detail::SourceCachedGradient(gradient, 3.0 / 256) == Colour{4, 4, 4, 4});
	CHECK(detail::SourceCachedGradient(gradient, .5) == Colour{128, 128, 128, 128});
	gradient.Mode = 1;
	CHECK(detail::SourceCachedGradient(gradient, .5) == Colour{0, 0, 0, 0});
	CHECK(detail::SourceCachedGradient(gradient, 1) == Colour{255, 255, 255, 255});
	gradient.Keys.clear();
	for (size_t i = 0; i < 100; ++i)
		gradient.Keys.push_back({double(i) / 99, Colour{uint8_t(i), 0, 0, 255}});
	CHECK(detail::SourceCachedGradient(gradient, 1) == Colour{99, 0, 0, 255});
}
