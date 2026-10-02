#include <engine/imagegraph/SourceCamera3D.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cmath>
TEST_SUITE_ID("engine.imagegraph.source_camera_3d")
using namespace engine::imagegraph;
TEST_CASE(
	"Source camera lookat uses left handed depth and source negative Z up", "[imagegraph][source_camera_3d]"
) {
	SourceCameraPose pose;
	pose.Position = {0, 0, 0};
	pose.Target = {1, 0, 0};
	pose.Up = {0, 0, -1};
	pose.Projection = SourceCameraProjection::Perspective;
	pose.FieldOfViewDegrees = 90;
	pose.ClippingDistance = {1, 10};
	std::array<double, 16> view{}, projection{};
	Diagnostic diagnostic;
	REQUIRE(ResolveSourceCameraMatrices(pose, 200, 100, view, projection, diagnostic) == Status::Ok);
	CHECK(view[2] == 1);
	CHECK(view[4] == -1);
	CHECK(view[9] == -1);
	CHECK(std::abs(projection[0] - .5) < 1e-12);
	CHECK(std::abs(projection[5] - 1) < 1e-12);
	auto depth = [&](double z) { return (projection[10] * z + projection[14]) / z; };
	CHECK(std::abs(depth(1)) < 1e-12);
	CHECK(std::abs(depth(10) - 1) < 1e-12);
	pose.Projection = SourceCameraProjection::Orthographic;
	pose.OrthographicViewSize = {2, 1};
	REQUIRE(ResolveSourceCameraMatrices(pose, 200, 100, view, projection, diagnostic) == Status::Ok);
	CHECK(projection[0] == 1);
	CHECK(projection[5] == 2);
	CHECK(projection[15] == 1);
	pose.Target = pose.Position;
	const auto original = view;
	CHECK(ResolveSourceCameraMatrices(pose, 200, 100, view, projection, diagnostic) == Status::InvalidValue);
	CHECK(view == original);
}
TEST_CASE(
	"Source camera dimensions preserve linked units and half even rounding", "[imagegraph][source_camera_3d]"
) {
	Document document;
	document.Project.emplace();
	document.Project->SurfaceWidth = 200;
	document.Project->SurfaceHeight = 100;
	Node node{"camera", "pc.3_d_camera", "", {}, {}};
	Diagnostic diagnostic;
	uint32_t width = 0, height = 0;
	std::vector<EvaluationInputValue> inputs{
		{"dimension", Vector2{.5, .5}, false, {}}, {"dimension_unit", int64_t(1), false, {}}
	};
	REQUIRE(ResolveSourceCameraDimensions(document, node, inputs, width, height, diagnostic) == Status::Ok);
	CHECK(width == 100);
	CHECK(height == 50);
	inputs[0] = {"dimension", Vector2{2.5, 3.5}, true, {}};
	REQUIRE(ResolveSourceCameraDimensions(document, node, inputs, width, height, diagnostic) == Status::Ok);
	CHECK(width == 2);
	CHECK(height == 4);
}
TEST_CASE(
	"Source camera project shader round trips and inherited selection resolves",
	"[imagegraph][source_camera_3d]"
) {
	Document document;
	document.FormatVersion = 9;
	document.Project.emplace();
	document.Project->Shader3D = 1;
	Diagnostic diagnostic;
	uint32_t shader = 42;
	REQUIRE(ResolveSourceCameraShader(document, {}, shader, diagnostic) == Status::Ok);
	CHECK(shader == 1);
	std::vector<EvaluationInputValue> inputs{{"shader", int64_t(1), false, {}}};
	REQUIRE(ResolveSourceCameraShader(document, inputs, shader, diagnostic) == Status::Ok);
	CHECK(shader == 0);
	Document copy;
	const std::string text = Write(document);
	REQUIRE(Read(text, copy, diagnostic) == Status::Ok);
	REQUIRE(copy.Project);
	CHECK(copy.Project->Shader3D == 1);
	document.Project->Shader3D = 0;
	REQUIRE(Read(Write(document), copy, diagnostic) == Status::Ok);
	CHECK(copy.Project->Shader3D == 0);
}
