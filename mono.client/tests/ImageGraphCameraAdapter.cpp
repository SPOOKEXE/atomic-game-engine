#include "../src/ImageGraphCameraAdapter.hpp"

#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <cmath>

TEST_SUITE_ID("client.imagegraph.camera_adapter")
TEST_DEPENDS("engine.imagegraph.document")

namespace {
	using namespace engine::imagegraph;
	Document CameraScene() {
		Document document;
		document.FormatVersion = 7;
		document.Nodes = {
			{"cube", "pc.3_d_mesh_cube", "", {}, {}},
			{"light", "pc.3_d_light_point", "", {}, {{"position", Vector3{2, -2, -3}}}},
			{"scene", "pc.3_d_scene", "", {}, {}},
			{"camera",
			 "pc.3_d_camera",
			 "",
			 {},
			 {{"dimension", Vector2{64, 32}},
			  {"dimension_unit", EnumValue{0}},
			  {"shader", EnumValue{2}},
			  {"clipping_distance", Vector2{.1, 20}},
			  {"backface_culling", EnumValue{0}},
			  {"gamma_adjust", true},
			  {"wire_mode", EnumValue{1}},
			  {"wireframe_color", Colour{12, 34, 56, 255}}}}
		};
		document.Nodes[2].DynamicInputs = {{"cube", ValueType::Mesh}, {"light", ValueType::Mesh}};
		document.Links = {
			{"cube", "mesh", "scene", "cube"},
			{"light", "light", "scene", "light"},
			{"scene", "scene", "camera", "scene"}
		};
		document.Outputs = {{"out", "camera", "rendered"}};
		document.Keyframes = {
			{"camera", "horizontal_angle", 0, 0.0, "linear"},
			{"camera", "horizontal_angle", 10, 90.0, "linear"}
		};
		return document;
	}
}

TEST_CASE(
	"Camera adapter captures owned scene payload and animated source view", "[client][imagegraph][camera]"
) {
	auto document = CameraScene();
	Plan plan;
	Diagnostic diagnostic;
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	engine::render::imagegraph::SourceCamera3DRequest first, second;
	REQUIRE(
		client::detail::BuildCameraRequest(
			document, plan, document.Nodes.back(), "rendered", 0, 77, false, first, diagnostic
		)
	);
	REQUIRE(
		client::detail::BuildCameraRequest(
			document, plan, document.Nodes.back(), "depth", 10, 77, false, second, diagnostic
		)
	);
	CHECK(first.Width == 64);
	CHECK(first.Height == 32);
	CHECK(first.Format == engine::assets::TextureFormat::RGBA8_LINEAR);
	REQUIRE(first.Scene.Data);
	CHECK(first.Scene.Data->Objects.size() == 2);
	CHECK(first.Shader == 1);
	CHECK(first.CullMode == 0);
	CHECK(first.GammaAdjust);
	CHECK(first.WireMode == 1);
	CHECK(first.WireColor[0] == 12.0f / 255);
	CHECK(first.View != second.View);
	CHECK(second.Output == engine::render::imagegraph::SourceCamera3DOutput::Depth);
	CHECK(&*first.Scene.Data != &*second.Scene.Data);
	for (float value : first.View)
		CHECK(std::isfinite(value));
	const auto original = first.View;
	CHECK_FALSE(
		client::detail::BuildCameraRequest(
			document, plan, document.Nodes.back(), "bogus", 0, 77, false, first, diagnostic
		)
	);
	CHECK(diagnostic.Code == Status::UnknownPort);
	CHECK(first.View == original);
}

TEST_CASE(
	"Camera adapter exposes source diagnostic outputs and rejects invalid wire controls",
	"[client][imagegraph][camera]"
) {
	using Output = engine::render::imagegraph::SourceCamera3DOutput;
	const auto [port, expected] = GENERATE(
		std::pair{"rendered", Output::Rendered},
		std::pair{"diffuse", Output::Diffuse},
		std::pair{"normal", Output::Normal},
		std::pair{"view_normal", Output::ViewNormal},
		std::pair{"depth", Output::Depth},
		std::pair{"shadow", Output::Shadow},
		std::pair{"ambient_occlusion", Output::AmbientOcclusion}
	);
	auto document = CameraScene();
	Plan plan;
	Diagnostic diagnostic;
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	engine::render::imagegraph::SourceCamera3DRequest request;
	REQUIRE(
		client::detail::BuildCameraRequest(
			document, plan, document.Nodes.back(), port, 0, 77, true, request, diagnostic
		)
	);
	CHECK(request.Output == expected);
	CHECK(request.Format == engine::assets::TextureFormat::RGBA8);
	document.Nodes.back().Values.push_back({"wireframe_thickness", -1.0});
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	CHECK_FALSE(
		client::detail::BuildCameraRequest(
			document, plan, document.Nodes.back(), port, 0, 77, true, request, diagnostic
		)
	);
	CHECK(diagnostic.Code == Status::InvalidValue);
	CHECK(request.Output == expected);
}
