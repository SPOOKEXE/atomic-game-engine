#include "../src/PixelOpsShapeRender.hpp"

#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <vector>

TEST_SUITE_ID("engine.imagegraph.pixel_ops_shape_render")

using engine::imagegraph::Image;
using engine::imagegraph::detail::RenderShapeBase;
using engine::imagegraph::detail::ShapeAuxiliaryOutputs;
using engine::imagegraph::detail::ShapeKind;
using engine::imagegraph::detail::ShapeRenderControls;
using engine::imagegraph::detail::ShapeRenderStatus;

TEST_CASE("Shape default rectangle retains RGB outside its coverage", "[imagegraph]") {
	Image output{3, 3, std::vector<uint8_t>(36), 0};
	ShapeRenderControls control;
	control.Geometry.HalfSize = {1.0 / 6.0, 1.0 / 6.0};
	control.Color = {50, 100, 150, 200};
	REQUIRE(RenderShapeBase(output, control) == ShapeRenderStatus::Ok);
	for (size_t pixel = 0; pixel < 9; pixel++) {
		const size_t index = pixel * 4;
		CHECK(output.Pixels[index] == 50);
		CHECK(output.Pixels[index + 1] == 100);
		CHECK(output.Pixels[index + 2] == 150);
		CHECK(output.Pixels[index + 3] == (pixel == 4 ? 200 : 0));
	}
}

TEST_CASE("Shape source mask and solid background compose exact pixels", "[imagegraph]") {
	Image output{3, 3, std::vector<uint8_t>(36), 0};
	const Image mask{1, 1, {255, 255, 255, 128}, 0};
	ShapeRenderControls control;
	control.Geometry.HalfSize = {1.0 / 6.0, 1.0 / 6.0};
	control.Color = {50, 100, 150, 200};
	control.Background = 1;
	control.BackgroundColor = {0, 0, 255, 255};
	REQUIRE(RenderShapeBase(output, control, &mask) == ShapeRenderStatus::Ok);
	CHECK(
		std::vector<uint8_t>(output.Pixels.begin(), output.Pixels.begin() + 4) ==
		std::vector<uint8_t>{0, 0, 255, 255}
	);
	CHECK(
		std::vector<uint8_t>(output.Pixels.begin() + 16, output.Pixels.begin() + 20) ==
		std::vector<uint8_t>{25, 50, 75, 100}
	);
	control.Background = 0;
	control.MultiplyAlpha = true;
	REQUIRE(RenderShapeBase(output, control, &mask) == ShapeRenderStatus::Ok);
	CHECK(
		std::vector<uint8_t>(output.Pixels.begin() + 16, output.Pixels.begin() + 20) ==
		std::vector<uint8_t>{10, 20, 30, 100}
	);
}

TEST_CASE("Shape falls back from missing background surface and refuses collapsed level", "[imagegraph]") {
	Image output{1, 1, {7, 7, 7, 7}, 0};
	ShapeRenderControls control;
	control.Background = 2;
	REQUIRE(RenderShapeBase(output, control) == ShapeRenderStatus::Ok);
	CHECK(output.Pixels == std::vector<uint8_t>{255, 255, 255, 255});
	control.Background = 0;
	control.LevelOut = 0.0;
	CHECK(RenderShapeBase(output, control) == ShapeRenderStatus::InvalidControl);
	CHECK(output.Pixels == std::vector<uint8_t>{255, 255, 255, 255});
}

TEST_CASE("Shape ellipse, half and triangle produce source coverage", "[imagegraph]") {
	Image output{3, 3, std::vector<uint8_t>(36), 0};
	ShapeRenderControls control;
	control.Geometry.Kind = ShapeKind::Ellipse;
	control.Geometry.HalfSize = {0.3, 0.3};
	REQUIRE(RenderShapeBase(output, control) == ShapeRenderStatus::Ok);
	for (size_t pixel = 0; pixel < 9; pixel++)
		CHECK(output.Pixels[pixel * 4 + 3] == (pixel == 4 ? 255 : 0));
	control.Geometry.Kind = ShapeKind::Half;
	control.Geometry.Point1 = {1.5, 1.5};
	REQUIRE(RenderShapeBase(output, control) == ShapeRenderStatus::Ok);
	for (size_t pixel = 0; pixel < 9; pixel++)
		CHECK(output.Pixels[pixel * 4 + 3] == (pixel >= 6 ? 255 : 0));
	control.Geometry.Kind = ShapeKind::Triangle;
	control.Geometry.HalfSize = {0.5, 0.5};
	control.Geometry.Point1 = {0.0, 0.0};
	control.Geometry.Point2 = {3.0, 0.0};
	control.Geometry.Point3 = {0.0, 3.0};
	REQUIRE(RenderShapeBase(output, control) == ShapeRenderStatus::Ok);
	CHECK(output.Pixels[3] == 255);
	CHECK(output.Pixels[8 * 4 + 3] == 0);
}

TEST_CASE("Shape renders Mask, Height and UV outputs from source equations", "[imagegraph]") {
	Image surface{3, 3, std::vector<uint8_t>(36), 0};
	Image mask{3, 3, std::vector<uint8_t>(36), 0};
	Image height{3, 3, std::vector<uint8_t>(36), 0};
	Image uv{3, 3, std::vector<uint8_t>(36), 0};
	ShapeRenderControls control;
	control.Geometry.HalfSize = {1.0 / 6.0, 1.0 / 6.0};
	REQUIRE(
		RenderShapeBase(surface, control, nullptr, nullptr, ShapeAuxiliaryOutputs{&mask, &height, &uv}) ==
		ShapeRenderStatus::Ok
	);
	CHECK(
		std::vector<uint8_t>(mask.Pixels.begin() + 16, mask.Pixels.begin() + 20) ==
		std::vector<uint8_t>{255, 255, 255, 255}
	);
	CHECK(
		std::vector<uint8_t>(mask.Pixels.begin(), mask.Pixels.begin() + 4) ==
		std::vector<uint8_t>{0, 0, 0, 255}
	);
	CHECK(
		std::vector<uint8_t>(height.Pixels.begin() + 16, height.Pixels.begin() + 20) ==
		std::vector<uint8_t>{43, 43, 43, 255}
	);
	CHECK(
		std::vector<uint8_t>(uv.Pixels.begin() + 16, uv.Pixels.begin() + 20) ==
		std::vector<uint8_t>{128, 128, 0, 255}
	);
	CHECK(uv.Pixels[3] == 0);
}

TEST_CASE("Shape graph selects each source output and routes Height to an image consumer", "[imagegraph]") {
	using namespace engine::imagegraph;
	Document document;
	document.FormatVersion = 3;
	Node shape;
	shape.Id = "shape";
	shape.Type = "image.shape";
	shape.Values = {
		{"width", int64_t{3}}, {"height", int64_t{3}}, {"half_size", Vector2{1.0 / 6.0, 1.0 / 6.0}}
	};
	document.Nodes.push_back(shape);
	for (const char *port : {"colored", "mask", "height", "uv"})
		document.Outputs.push_back({port, "shape", port});
	Node pass;
	pass.Id = "pass";
	pass.Type = "image.passthrough";
	document.Nodes.push_back(pass);
	document.Links.push_back({"shape", "height", "pass", "image"});
	document.Outputs.push_back({"routed", "pass", "image"});
	Plan plan;
	Diagnostic diagnostic;
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	Image colored, mask, height, uv, routed;
	REQUIRE(Evaluate(document, plan, "colored", colored, diagnostic) == Status::Ok);
	REQUIRE(Evaluate(document, plan, "mask", mask, diagnostic) == Status::Ok);
	REQUIRE(Evaluate(document, plan, "height", height, diagnostic) == Status::Ok);
	REQUIRE(Evaluate(document, plan, "uv", uv, diagnostic) == Status::Ok);
	REQUIRE(Evaluate(document, plan, "routed", routed, diagnostic) == Status::Ok);
	CHECK(colored.Pixels[3] == 0);
	CHECK(colored.Pixels[19] == 255);
	CHECK(mask.Pixels[3] == 255);
	CHECK(mask.Pixels[16] == 255);
	CHECK(height.Pixels[16] == 43);
	CHECK(uv.Pixels[16] == 128);
	CHECK(routed.Pixels == height.Pixels);
	document.Nodes[0].Values.push_back({"position_mode", int64_t{0}});
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	CHECK(Evaluate(document, plan, "colored", colored, diagnostic) == Status::UnsupportedExecution);
}
