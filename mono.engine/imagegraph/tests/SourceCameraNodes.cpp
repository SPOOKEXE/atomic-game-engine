#include "../src/nodes/SourceCameraNodes.hpp"

#include "../src/nodes/Processor.hpp"

#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <utility>

TEST_SUITE_ID("engine.imagegraph.source_camera")

namespace {
	using namespace engine::imagegraph;
	using namespace engine::imagegraph::detail;

	struct CameraResult {
		bool Ok = false;
		Status Code = Status::Ok;
		Image Output;
	};

	CameraResult RenderCamera(
		const Image &layer,
		uint32_t width,
		uint32_t height,
		int64_t oversample = 0,
		Vector2 position = {},
		double depth = 0.0,
		bool dof = false,
		bool hideLayer = false,
		int64_t positioning = 1,
		Vector2 parallax = {},
		double zoom = 1.0,
		double focalDistance = 0.0,
		double focalRange = 0.0,
		double defocus = 1.0
	) {
		const CatalogueEntry *entry = FindCatalogueEntry("pc.camera");
		if (!entry) return {};
		Node node{"camera", "pc.camera", "", {}, {}};
		node.DynamicInputs = {
			{"element_0", ValueType::Image, std::nullopt},
			{"positioning_0", ValueType::Enum, EnumValue{1}},
			{"position_0", ValueType::Vector2, Vector2{}},
			{"oversample_0", ValueType::Enum, EnumValue{0}},
			{"parallax_0", ValueType::Vector2, Vector2{}},
			{"depth_0", ValueType::Scalar, 0.0}
		};
		if (hideLayer)
			node.SourceProperties.push_back({"layer_visible", ArrayValue{ValueType::Boolean, {false}}});
		EvaluationRequest request;
		NodeContext context(node, *entry, request);
		context.ByteBudget = Limits::MaximumEvaluationBytes;
		context.Images.emplace_back("element_0", &layer);
		context.InputProvenanceResolved = true;
		context.Values = {
			{"camera_size", Vector2{double(width), double(height)}},
			{"camera_size_unit", EnumValue{0}},
			{"focus_center", Vector2{double(width / 2), double(height / 2)}},
			{"focus_center_unit", EnumValue{0}},
			{"zoom", zoom},
			{"depth_of_field", dof},
			{"focal_distance", focalDistance},
			{"focal_range", focalRange},
			{"defocus", defocus},
			{"attribute_color_depth", EnumValue{1}},
			{"attribute_array_process", EnumValue{0}},
			{"attribute_process", true},
			{"positioning_0", EnumValue{positioning}},
			{"position_0", position},
			{"position_0_unit", EnumValue{0}},
			{"oversample_0", EnumValue{oversample}},
			{"parallax_0", parallax},
			{"depth_0", depth}
		};
		CameraResult result;
		result.Ok = Camera(context);
		result.Code = context.FailureCode;
		if (!context.OutputImages.empty()) result.Output = std::move(context.OutputImages.front().second);
		return result;
	}

	Image Solid(uint8_t r, uint8_t g, uint8_t b, uint8_t a = 255) {
		return Image{1, 1, {r, g, b, a}, 0};
	}
}

TEST_CASE("Camera oversample modes preserve their source edge rules", "[imagegraph][source_camera]") {
	const Image white = Solid(255, 255, 255);
	const CameraResult empty = RenderCamera(white, 3, 1, 0);
	const CameraResult repeat = RenderCamera(white, 3, 1, 1);
	const CameraResult repeatX = RenderCamera(white, 3, 1, 2);
	REQUIRE(empty.Ok);
	REQUIRE(repeat.Ok);
	REQUIRE(repeatX.Ok);
	CHECK(ReadPixel(empty.Output, 0, 0)[3] == 0.0);
	CHECK(ReadPixel(empty.Output, 1, 0)[3] == 1.0);
	CHECK(ReadPixel(repeat.Output, 0, 0)[3] == 1.0);
	CHECK(ReadPixel(repeatX.Output, 0, 0)[3] == 1.0);

	const CameraResult repeatY = RenderCamera(white, 1, 3, 3);
	REQUIRE(repeatY.Ok);
	CHECK(ReadPixel(repeatY.Output, 0, 0)[3] == 1.0);
}

TEST_CASE("Camera visibility property skips a layer before compositing", "[imagegraph][source_camera]") {
	const Image red = Solid(255, 0, 0);
	const CameraResult visible = RenderCamera(red, 1, 1);
	const CameraResult hidden = RenderCamera(red, 1, 1, 0, {}, 0.0, false, true);
	REQUIRE(visible.Ok);
	REQUIRE(hidden.Ok);
	CHECK(ReadPixel(visible.Output, 0, 0) == Rgba{1, 0, 0, 1});
	CHECK(ReadPixel(hidden.Output, 0, 0) == Rgba{0, 0, 0, 0});
}

TEST_CASE("Camera depth of field changes a textured layer", "[imagegraph][source_camera]") {
	const Image stripe{2, 1, {0, 0, 0, 255, 255, 255, 255, 255}, 0};
	const CameraResult sharp = RenderCamera(stripe, 1, 1, 1, {}, 10.0, true, false, 1, {}, 1.0, 10.0);
	const CameraResult blurred =
		RenderCamera(stripe, 1, 1, 1, {}, 10.0, true, false, 1, {}, 1.0, 0.0, 0.0, 1.0);
	REQUIRE(sharp.Ok);
	REQUIRE(blurred.Ok);
	CHECK(ReadPixel(sharp.Output, 0, 0) != ReadPixel(blurred.Output, 0, 0));
}

TEST_CASE("Camera applies positioning, parallax, and zoom", "[imagegraph][source_camera]") {
	const Image stripes{3, 1, {0, 0, 0, 255, 128, 128, 128, 255, 255, 255, 255, 255}, 0};
	const CameraResult space = RenderCamera(stripes, 3, 1, 1, {0, 0}, 0, false, false, 0);
	const CameraResult camera = RenderCamera(stripes, 3, 1, 1, {0, 0}, 0, false, false, 1);
	const CameraResult parallax = RenderCamera(stripes, 3, 1, 1, {0, 0}, 0, false, false, 0, {1, 0});
	const CameraResult zoomed = RenderCamera(stripes, 3, 1, 1, {0, 0}, 0, false, false, 0, {}, 2.0);
	REQUIRE(space.Ok);
	REQUIRE(camera.Ok);
	REQUIRE(parallax.Ok);
	REQUIRE(zoomed.Ok);
	CHECK(space.Output.Pixels != camera.Output.Pixels);
	CHECK(space.Output.Pixels != parallax.Output.Pixels);
	CHECK(space.Output.Pixels != zoomed.Output.Pixels);
}
