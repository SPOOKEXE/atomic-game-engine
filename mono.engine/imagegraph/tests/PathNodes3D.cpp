#include "../src/nodes/Path3D.hpp"
#include "NodeHarness.hpp"

#include <engine/imagegraph/Document.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>
TEST_SUITE_ID("engine.imagegraph.path_nodes_3d")
using namespace engine::imagegraph;
namespace {
	ArrayValue Anchor3(std::initializer_list<double> fields) {
		ArrayValue row{ValueType::Scalar, {}};
		for (double value : fields)
			row.Elements.emplace_back(value);
		return row;
	}
}
TEST_CASE(
	"3D path preserves Z handles and ten fields through compiled source execution",
	"[imagegraph][path_nodes_3d]"
) {
	Document document;
	document.FormatVersion = 9;
	document.Nodes.push_back(
		{"path",
		 "pc.path_3_d",
		 "",
		 {},
		 {{"sample_path", .5}},
		 {{"anchor_0", ValueType::Array, Anchor3({0, 0, 0, 0, 0, 0, 0, 0, 10, .25})},
		  {"anchor_1", ValueType::Array, Anchor3({10, 0, 0, 0, 0, 10, 0, 0, 0, .5})}}}
	);
	document.Outputs = {
		{"position", "path", "position_out"}, {"path", "path", "path_data"}, {"anchors", "path", "anchors"}
	};
	Plan plan;
	Diagnostic diagnostic;
	INFO(diagnostic.Message);
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	EvaluatedValue value;
	REQUIRE(EvaluateValue(document, plan, "position", {}, value, diagnostic) == Status::Ok);
	CHECK((std::get<Vector3>(value.Data) == Vector3{5, 0, 7.5}));
	REQUIRE(EvaluateValue(document, plan, "path", {}, value, diagnostic) == Status::Ok);
	const auto &path = std::get<PathValue3D>(value.Data);
	REQUIRE(path.Data);
	REQUIRE(path.Data->Anchors.size() == 2);
	CHECK(path.Data->Anchors[0].Controls[8] == 10);
	CHECK(path.Data->Anchors[0].Index == .25);
	CHECK(path.Data->Anchors[1].Index == .5);
	const auto text = Write(document);
	Document restored;
	REQUIRE(Read(text, restored, diagnostic) == Status::Ok);
	CHECK(restored == document);
	Document saved;
	saved.FormatVersion = 9;
	saved.Junctions.push_back({"path", "", ValueType::Path3D, path});
	REQUIRE(Read(Write(saved), restored, diagnostic) == Status::Ok);
	REQUIRE(restored.Junctions.size() == 1);
	CHECK(std::get<PathValue3D>(*restored.Junctions.front().Default) == path);
}
TEST_CASE(
	"3D path sampler retains source endpoint clamp and length Z accumulation", "[imagegraph][path_nodes_3d]"
) {
	PathData3D data;
	data.Resolution = 2;
	data.Anchors = {{{0, 0, 0, 0, 0, 0, 0, 0, 0}, 0}, {{0, 0, 2, 0, 0, 0, 0, 0, 0}, 0}};
	detail::PathRuntime3D runtime(data);
	CHECK(runtime.Length() == 3);
	CHECK(std::abs(runtime.Ratio(1).Position.Z - 1.98) < 1e-12);
	CHECK(runtime.Ratio(-.5).Position.Z == 1);
	CHECK(runtime.BySegment(1.5).Position.Z == 1);
	CHECK(runtime.Ratio(.5).Weight == 1);
}

TEST_CASE(
	"3D path transform preserves source two dimensional weights and rotation before scale",
	"[imagegraph][path_nodes_3d]"
) {
	Path2D planar;
	planar.Anchors = {{{0, 0, 0, 0, 0, 0}, 0}, {{10, 0, 0, 0, 0, 0}, 0}};
	planar.Weights = {{0, 4}, {100, 8}};
	const Node node{"transform", "pc.path_3_d_transform", "", {}, {}};
	const auto *entry = FindCatalogueEntry(node.Type);
	REQUIRE(entry);
	EvaluationRequest request;
	detail::NodeContext context(node, *entry, request);
	context.ByteBudget = Limits::MaximumEvaluationBytes;
	context.Values = {
		{"path", planar},
		{"position", Vector3{1, 2, 3}},
		{"anchor", Vector3{1, 0, 0}},
		{"scale", Vector3{2, 3, 4}},
		{"rotation", Quaternion{0, 0, -std::sqrt(.5), std::sqrt(.5)}}
	};
	const auto executor = detail::FindExecutor(node.Type);
	REQUIRE(executor);
	REQUIRE(executor(context));
	const auto &output = std::get<PathValue3D>(context.OutputValues.front().Data);
	REQUIRE(output.Data);
	REQUIRE(output.Data->Source2D);
	detail::PathRuntime3D runtime(*output.Data, &context);
	REQUIRE(runtime.Valid());
	CHECK(runtime.Length() == 10);
	const auto point = runtime.Ratio(.5);
	CHECK(std::abs(point.Position.X - 2) < 1e-12);
	CHECK(std::abs(point.Position.Y + 10) < 1e-12);
	CHECK(point.Position.Z == 3);
	CHECK(point.Weight == 6);
	Document document;
	document.FormatVersion = 9;
	document.Junctions.push_back({"path", "", ValueType::Path3D, output});
	Document restored;
	Diagnostic diagnostic;
	REQUIRE(Read(Write(document), restored, diagnostic) == Status::Ok);
	CHECK(restored == document);
}
TEST_CASE(
	"3D path camera preserves lazy projection dimensions and source clip depth weights",
	"[imagegraph][path_nodes_3d]"
) {
	PathValue3D path;
	auto &data = path.Data.emplace();
	data.Anchors = {{{0, 0, 0, 0, 0, 0, 0, 0, 0}, 0}, {{0, 2, 0, 0, 0, 0, 0, 0, 0}, 0}};
	const Node node{"camera", "pc.path_3_d_camera", "", {}, {}};
	const auto *entry = FindCatalogueEntry(node.Type);
	REQUIRE(entry);
	EvaluationRequest request;
	detail::NodeContext context(node, *entry, request);
	context.ByteBudget = Limits::MaximumEvaluationBytes;
	context.Values = {
		{"path", path},
		{"dimension", Vector2{4, 2}},
		{"dimension_unit", EnumValue{0}},
		{"position", Vector3{10, 0, 0}},
		{"lookat_position", Vector3{}},
		{"postioning_mode", EnumValue{1}},
		{"projection", EnumValue{1}},
		{"orthographic_scale", .5},
		{"depth_range", Vector2{.1, 100}},
		{"apply_depth_to_weight", true}
	};
	auto executor = detail::FindExecutor(node.Type);
	REQUIRE(executor);
	REQUIRE(executor(context));
	const auto &output = std::get<PathValue3D>(context.OutputValues.front().Data);
	REQUIRE(output.Data);
	detail::PathRuntime3D runtime(*output.Data, &context);
	REQUIRE(runtime.Valid());
	CHECK(runtime.Length() == 2);
	auto point = runtime.Ratio(.5);
	CHECK((point.Position == Vector3{4, 2, 0}));
	CHECK(std::abs(point.Weight - (10 - .1) / (100 - .1)) < 1e-12);
	Document saved;
	saved.FormatVersion = 9;
	saved.Junctions.push_back({"path", "", ValueType::Path3D, output});
	Document restored;
	Diagnostic diagnostic;
	REQUIRE(Read(Write(saved), restored, diagnostic) == Status::Ok);
	CHECK(restored == saved);
	saved.FormatVersion = 8;
	Plan plan;
	CHECK(Compile(saved, plan, diagnostic) == Status::UnsupportedVersion);
}
TEST_CASE(
	"Segmented source paths have a distinct v9 codec and retain their marker through spatial wrappers",
	"[imagegraph][path_nodes_3d]"
) {
	static_assert(sizeof(Value) <= 88);
	Path2D path;
	path.Segmented = true;
	path.Loop = true;
	path.Anchors = {{{0, 0, 0, 0, 0, 0}, 0}, {{1, 0, 0, 0, 0, 0}, 1}, {{0, 1, 0, 0, 0, 0}, 2}};
	Document saved;
	saved.FormatVersion = 9;
	saved.Junctions.push_back({"shape", "", ValueType::Path2D, path});
	Diagnostic diagnostic;
	Document restored;
	const auto text = Write(saved);
	CHECK(text.find(" ps ") != std::string::npos);
	REQUIRE(Read(text, restored, diagnostic) == Status::Ok);
	CHECK(restored == saved);
	PathValue3D spatial;
	spatial.Data.emplace().Source2D = path;
	saved.Junctions.front() = {"spatial", "", ValueType::Path3D, spatial};
	REQUIRE(Read(Write(saved), restored, diagnostic) == Status::Ok);
	CHECK(restored == saved);
	saved.Junctions.front() = {"shape", "", ValueType::Path2D, path};
	saved.FormatVersion = 8;
	Plan plan;
	CHECK(Compile(saved, plan, diagnostic) == Status::UnsupportedVersion);
	path.Segmented = false;
	saved.Junctions.front().Default = path;
	CHECK(Write(saved).find(" ps ") == std::string::npos);
	saved.Nodes.push_back(
		{"constant",
		 "image.solid",
		 "",
		 {},
		 {{"width", int64_t(1)}, {"height", int64_t(1)}, {"colour", Colour{255, 255, 255, 255}}}}
	);
	saved.Outputs.push_back({"out", "constant", "image"});
	CHECK(Compile(saved, plan, diagnostic) == Status::Ok);
}

TEST_CASE(
	"Generic source path sampler preserves spatial vector direction and projected planar outputs",
	"[imagegraph][path_nodes_3d]"
) {
	PathValue3D path;
	auto &data = path.Data.emplace();
	data.Anchors = {{{0, 0, 0, 0, 0, 0, 0, 0, 0}, 0}, {{0, 0, 2, 0, 0, 0, 0, 0, 0}, 1}};
	auto sampled = imagegraph_test::RunNode("pc.path_sample", {}, {{"path", path}, {"ratio", .5}});
	INFO(sampled.Message);
	REQUIRE(sampled.Ok);
	CHECK(std::get<Vector3>(*sampled.OutputValue("position")) == Vector3{0, 0, 1});
	const auto direction = std::get<Vector3>(*sampled.OutputValue("direction"));
	CHECK(std::abs(direction.Z + .0004) < 1e-12);
	CHECK(std::get<double>(*sampled.OutputValue("weight")) == 1);
	auto ping = imagegraph_test::RunNode(
		"pc.path_sample", {}, {{"path", path}, {"ratio", 1.5}, {"type", EnumValue{1}}}
	);
	REQUIRE(ping.Ok);
	CHECK(std::get<Vector3>(*ping.OutputValue("position")) == Vector3{0, 0, 1});
	CHECK(std::abs(std::get<Vector3>(*ping.OutputValue("direction")).Z - .0004) < 1e-12);
	PathTransform3D camera;
	camera.Projective = true;
	camera.CameraView = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
	camera.CameraProjection = camera.CameraView;
	data.Transforms.push_back(camera);
	auto projected = imagegraph_test::RunNode("pc.path_sample", {}, {{"path", path}, {"ratio", .5}});
	REQUIRE(projected.Ok);
	CHECK(std::get<Vector2>(*projected.OutputValue("position")) == Vector2{1, 1});
	CHECK(std::holds_alternative<double>(*projected.OutputValue("direction")));
}
