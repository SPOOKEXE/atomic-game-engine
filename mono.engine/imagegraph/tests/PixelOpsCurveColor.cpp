#include "../src/PixelOpsCurveColor.hpp"

#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <vector>

TEST_SUITE_ID("engine.imagegraph.pixel_ops_curve_color")

using engine::imagegraph::Colour;
using engine::imagegraph::Curve;
using engine::imagegraph::Gradient;
using engine::imagegraph::Image;
using engine::imagegraph::detail::ColorizeControls;
using engine::imagegraph::detail::CurveColorControls;
using engine::imagegraph::detail::CurveColorStatus;
using engine::imagegraph::detail::IdentityColorCurve;
using engine::imagegraph::detail::RenderColorize;
using engine::imagegraph::detail::RenderCurveColor;
using engine::imagegraph::detail::SampleColorCurve;

TEST_CASE("Curve identity preserves RGBA and brightness acts after channels", "[imagegraph]") {
	const Image source{1, 1, {51, 102, 153, 204}, 0};
	Image output{1, 1, {0, 0, 0, 0}, 0};
	CurveColorControls control{
		IdentityColorCurve(),
		IdentityColorCurve(),
		IdentityColorCurve(),
		IdentityColorCurve(),
		IdentityColorCurve()
	};
	REQUIRE(RenderCurveColor(source, output, control) == CurveColorStatus::Ok);
	CHECK(output.Pixels == source.Pixels);
	control.Red.Anchors[0][3] = 1.0;
	control.Red.Anchors[0][5] = -1.0 / 3.0;
	control.Red.Anchors[1][1] = 1.0 / 3.0;
	control.Red.Anchors[1][3] = 0.0;
	REQUIRE(RenderCurveColor(source, output, control) == CurveColorStatus::Ok);
	CHECK(output.Pixels == std::vector<uint8_t>{204, 102, 153, 204});
}

TEST_CASE("Curve source treats zero mean RGB with the brightness target", "[imagegraph]") {
	const Image source{1, 1, {0, 0, 0, 77}, 0};
	Image output{1, 1, {0, 0, 0, 0}, 0};
	CurveColorControls control{
		IdentityColorCurve(),
		IdentityColorCurve(),
		IdentityColorCurve(),
		IdentityColorCurve(),
		IdentityColorCurve()
	};
	control.Brightness.Anchors[0][3] = 0.5;
	control.Brightness.Anchors[0][5] = 0.0;
	control.Brightness.Anchors[1][1] = 0.0;
	control.Brightness.Anchors[1][3] = 0.5;
	REQUIRE(RenderCurveColor(source, output, control) == CurveColorStatus::Ok);
	CHECK(output.Pixels == std::vector<uint8_t>{128, 128, 128, 77});
}

TEST_CASE("Curve evaluates steps, cubic handles and header remapping", "[imagegraph]") {
	Curve curve = IdentityColorCurve();
	curve.Header[2] = 1.0;
	curve.Anchors[0][3] = 0.25;
	curve.Anchors.insert(curve.Anchors.begin() + 1, {0, 0, 0.5, 0.75, 0, 0});
	double value = 0.0;
	REQUIRE(SampleColorCurve(curve, 0.25, value) == CurveColorStatus::Ok);
	CHECK(value == 0.25);
	REQUIRE(SampleColorCurve(curve, 0.5, value) == CurveColorStatus::Ok);
	CHECK(value == 0.75);
	curve = IdentityColorCurve();
	curve.Anchors[0][5] = 0.0;
	curve.Anchors[1][1] = 0.0;
	REQUIRE(SampleColorCurve(curve, 0.25, value) == CurveColorStatus::Ok);
	CHECK(value == 0.15625);
	curve = IdentityColorCurve();
	curve.Header = {0.25, 0.5, 0.0, 0.2, 0.6, 0.0};
	REQUIRE(SampleColorCurve(curve, 0.375, value) == CurveColorStatus::Ok);
	CHECK(value == 0.4);
}

TEST_CASE("Curve invalid controls leave destination untouched", "[imagegraph]") {
	const Image source{1, 1, {10, 20, 30, 40}, 0};
	Image output{1, 1, {7, 7, 7, 7}, 0};
	CurveColorControls control{
		IdentityColorCurve(),
		IdentityColorCurve(),
		IdentityColorCurve(),
		IdentityColorCurve(),
		IdentityColorCurve()
	};
	control.Red.Header[1] = 0.0;
	CHECK(RenderCurveColor(source, output, control) == CurveColorStatus::InvalidControl);
	CHECK(output.Pixels == std::vector<uint8_t>{7, 7, 7, 7});
}

TEST_CASE("Colorize maps Rec709 brightness through gradient keys", "[imagegraph]") {
	const Image source{3, 1, {0, 0, 0, 255, 128, 128, 128, 255, 255, 255, 255, 255}, 0};
	Image output{3, 1, std::vector<uint8_t>(12), 0};
	const Gradient gradient{0, {{0.0, {0, 0, 0, 255}}, {1.0, {255, 255, 255, 255}}}};
	REQUIRE(RenderColorize(source, output, gradient, {}) == CurveColorStatus::Ok);
	CHECK(output.Pixels == source.Pixels);
	const auto first = output.Pixels;
	REQUIRE(RenderColorize(source, output, gradient, {}) == CurveColorStatus::Ok);
	CHECK(output.Pixels == first);
}

TEST_CASE("Colorize shift, looping and source alpha change gradient position", "[imagegraph]") {
	const Image source{1, 1, {255, 255, 255, 128}, 0};
	Image output{1, 1, {0, 0, 0, 0}, 0};
	const Gradient gradient{0, {{0.0, {0, 0, 0, 0}}, {1.0, {255, 255, 255, 255}}}};
	REQUIRE(RenderColorize(source, output, gradient, {}) == CurveColorStatus::Ok);
	CHECK(output.Pixels == std::vector<uint8_t>{128, 128, 128, 128});
	ColorizeControls control;
	control.MultiplyAlpha = false;
	REQUIRE(RenderColorize(source, output, gradient, control) == CurveColorStatus::Ok);
	CHECK(output.Pixels == std::vector<uint8_t>{255, 255, 255, 128});
	control.KeepAlpha = false;
	REQUIRE(RenderColorize(source, output, gradient, control) == CurveColorStatus::Ok);
	CHECK(output.Pixels == std::vector<uint8_t>{255, 255, 255, 255});
	const Image black{1, 1, {0, 0, 0, 255}, 0};
	control.Overflow = 1;
	control.Shift = 1.25;
	REQUIRE(RenderColorize(black, output, gradient, control) == CurveColorStatus::Ok);
	CHECK(output.Pixels == std::vector<uint8_t>{64, 64, 64, 64});
}

TEST_CASE("Colorize rejects undefined range and unavailable gradient modes", "[imagegraph]") {
	const Image source{1, 1, {10, 20, 30, 40}, 0};
	Image output{1, 1, {7, 7, 7, 7}, 0};
	Gradient gradient{0, {{0.0, {0, 0, 0, 255}}, {1.0, {255, 255, 255, 255}}}};
	ColorizeControls control;
	control.Range = {0.5, 0.5};
	CHECK(RenderColorize(source, output, gradient, control) == CurveColorStatus::UndefinedDivision);
	CHECK(output.Pixels == std::vector<uint8_t>{7, 7, 7, 7});
	control.Range = {0.0, 1.0};
	gradient.Mode = 2;
	CHECK(RenderColorize(source, output, gradient, control) == CurveColorStatus::InvalidControl);
	CHECK(output.Pixels == std::vector<uint8_t>{7, 7, 7, 7});
}

TEST_CASE("Curve graph evaluates authored brightness and channel selection", "[imagegraph]") {
	using namespace engine::imagegraph;
	Document document;
	document.FormatVersion = 5;
	Node source{
		"source",
		"image.gradient",
		"",
		{},
		{{"width", int64_t{2}},
		 {"height", int64_t{1}},
		 {"gradient", Gradient{0, {{0.0, {0, 0, 0, 255}}, {1.0, {255, 255, 255, 255}}}}},
		 {"center", Vector2{1.0, 0.5}}}
	};
	Node curve{"curve", "image.curve", "", {}, {{"channel", int64_t{1}}}};
	Curve white = IdentityColorCurve();
	white.Anchors[0][3] = 1.0;
	white.Anchors[0][5] = 0.0;
	white.Anchors[1][1] = 0.0;
	white.Anchors[1][3] = 1.0;
	curve.Values.push_back({"brightness", white});
	document.Nodes = {source, curve};
	document.Links = {{"source", "image", "curve", "image"}};
	document.Outputs = {{"out", "curve", "image"}};
	Plan plan;
	Diagnostic diagnostic;
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	Image output;
	REQUIRE(Evaluate(document, plan, "out", output, diagnostic) == Status::Ok);
	CHECK(output.Pixels == std::vector<uint8_t>{255, 64, 64, 255, 255, 191, 191, 255});
	curve.Values[1].Data = Curve{{0, 0, 0, 0, 1, 0}, white.Anchors};
	document.Nodes[1] = curve;
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	CHECK(Evaluate(document, plan, "out", output, diagnostic) == Status::UnsupportedExecution);
	CHECK(diagnostic.NodeId == "curve");
}

TEST_CASE("Colorize graph samples typed gradient and rejects mapped input", "[imagegraph]") {
	using namespace engine::imagegraph;
	Document document;
	document.FormatVersion = 5;
	Node source{
		"source",
		"image.gradient",
		"",
		{},
		{{"width", int64_t{2}},
		 {"height", int64_t{1}},
		 {"gradient", Gradient{0, {{0.0, {0, 0, 0, 255}}, {1.0, {255, 255, 255, 255}}}}},
		 {"center", Vector2{1.0, 0.5}}}
	};
	Node colorize{
		"colorize",
		"image.colorize",
		"",
		{},
		{{"gradient", Gradient{0, {{0.0, {255, 0, 0, 255}}, {1.0, {0, 0, 255, 255}}}}}}
	};
	document.Nodes = {source, colorize};
	document.Links = {{"source", "image", "colorize", "image"}};
	document.Outputs = {{"out", "colorize", "image"}};
	Plan plan;
	Diagnostic diagnostic;
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	Image output;
	REQUIRE(Evaluate(document, plan, "out", output, diagnostic) == Status::Ok);
	CHECK(output.Pixels == std::vector<uint8_t>{191, 0, 64, 255, 64, 0, 191, 255});
	document.Links.push_back({"source", "image", "colorize", "gradient_map"});
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	CHECK(Evaluate(document, plan, "out", output, diagnostic) == Status::UnsupportedExecution);
	CHECK(diagnostic.NodeId == "colorize");
}
