#include "../src/PixelOpsGradient.hpp"

#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

TEST_SUITE_ID("engine.imagegraph.pixel_ops_gradient")

using engine::imagegraph::detail::GradientColor;
using engine::imagegraph::detail::GradientGeometry;
using engine::imagegraph::detail::GradientKey;
using engine::imagegraph::detail::GradientProgress;
using engine::imagegraph::detail::GradientRenderControls;
using engine::imagegraph::detail::GradientStatus;
using engine::imagegraph::detail::RenderGradientBase;

TEST_CASE("Gradient UV alpha and mask alpha multiply before RGB transfer", "[imagegraph]") {
	using namespace engine::imagegraph;
	Image output{1, 1, {0, 0, 0, 0}};
	Image uv{1, 1, {255, 255, 0, 128}};
	Image mask{1, 1, {0, 0, 0, 128}};
	GradientGeometry geometry;
	geometry.CenterX = 0.5;
	geometry.CenterY = 0.5;
	const std::array<engine::imagegraph::detail::GradientKey, 2> keys{
		{{0, {0, 0, 0, 255}}, {1, {255, 255, 255, 255}}}
	};
	GradientRenderControls control;
	control.UVMap = &uv;
	REQUIRE(RenderGradientBase(output, geometry, keys, 0, &mask, control) == GradientStatus::Ok);
	CHECK(output.Pixels == std::vector<uint8_t>{255, 255, 255, 64});
	control.UVMix = 0.0;
	REQUIRE(RenderGradientBase(output, geometry, keys, 0, &mask, control) == GradientStatus::Ok);
	CHECK(output.Pixels == std::vector<uint8_t>{128, 128, 128, 64});
	uv.Width = 2;
	CHECK(RenderGradientBase(output, geometry, keys, 0, &mask, control) == GradientStatus::InvalidControl);
}

TEST_CASE("Gradient mapped angle uses source RGB average", "[imagegraph]") {
	using namespace engine::imagegraph;
	Image output{1, 1, {0, 0, 0, 0}};
	Image angle{1, 1, {255, 255, 255, 255}};
	GradientGeometry geometry;
	geometry.CenterX = 0.0;
	geometry.CenterY = 0.5;
	const std::array<engine::imagegraph::detail::GradientKey, 2> keys{
		{{0, {0, 0, 0, 255}}, {1, {255, 255, 255, 255}}}
	};
	GradientRenderControls control;
	control.AngleMap = &angle;
	control.AngleMaximum = 180.0;
	REQUIRE(RenderGradientBase(output, geometry, keys, 0, nullptr, control) == GradientStatus::Ok);
	CHECK(output.Pixels == std::vector<uint8_t>{0, 0, 0, 255});
	angle.Pixels = {0, 0, 0, 255};
	REQUIRE(RenderGradientBase(output, geometry, keys, 0, nullptr, control) == GradientStatus::Ok);
	CHECK(output.Pixels == std::vector<uint8_t>{255, 255, 255, 255});
}

TEST_CASE("Gradient mapped radius shift and scale use source RGB average", "[imagegraph]") {
	using namespace engine::imagegraph;
	Image output{1, 1, {0, 0, 0, 0}};
	Image map{1, 1, {255, 255, 255, 255}};
	const std::array<engine::imagegraph::detail::GradientKey, 2> keys{
		{{0, {0, 0, 0, 255}}, {1, {255, 255, 255, 255}}}
	};
	GradientGeometry geometry;
	geometry.Type = 1;
	geometry.CenterX = 0.0;
	geometry.CenterY = 0.5;
	GradientRenderControls controls;
	controls.RadiusMap = &map;
	controls.RadiusMaximum = 1.0;
	REQUIRE(RenderGradientBase(output, geometry, keys, 0, nullptr, controls) == GradientStatus::Ok);
	CHECK(output.Pixels == std::vector<uint8_t>{128, 128, 128, 255});
	controls.RadiusMap = nullptr;
	controls.ShiftMap = &map;
	controls.ShiftMaximum = 0.5;
	geometry.Type = 0;
	geometry.CenterX = 0.5;
	REQUIRE(RenderGradientBase(output, geometry, keys, 0, nullptr, controls) == GradientStatus::Ok);
	CHECK(output.Pixels == std::vector<uint8_t>{255, 255, 255, 255});
	controls.ShiftMap = nullptr;
	controls.ScaleMap = &map;
	controls.ScaleMaximum = 2.0;
	geometry.CenterX = 0.0;
	REQUIRE(RenderGradientBase(output, geometry, keys, 0, nullptr, controls) == GradientStatus::Ok);
	CHECK(output.Pixels == std::vector<uint8_t>{191, 191, 191, 255});
	controls.ScaleMaximum = 0.0;
	CHECK(
		RenderGradientBase(output, geometry, keys, 0, nullptr, controls) == GradientStatus::UndefinedDivision
	);
}

TEST_CASE("Gradient progress curve and RGB level follow shader order", "[imagegraph]") {
	using namespace engine::imagegraph;
	Image output{2, 1, std::vector<uint8_t>(8)};
	GradientGeometry geometry;
	geometry.CenterX = 1.0;
	geometry.CenterY = 0.5;
	const std::array<engine::imagegraph::detail::GradientKey, 2> keys{
		{{0, {0, 0, 0, 255}}, {1, {255, 255, 255, 255}}}
	};
	GradientRenderControls control;
	Curve step = detail::IdentityColorCurve();
	step.Header[2] = 1.0;
	control.ProgressRemap = &step;
	control.LevelOut = {0.0, 0.5};
	REQUIRE(RenderGradientBase(output, geometry, keys, 0, nullptr, control) == GradientStatus::Ok);
	CHECK(output.Pixels == std::vector<uint8_t>{0, 0, 0, 255, 0, 0, 0, 255});
	step.Anchors.insert(step.Anchors.begin() + 1, {{0, 0, 0.5, 1, 0, 0}});
	REQUIRE(RenderGradientBase(output, geometry, keys, 0, nullptr, control) == GradientStatus::Ok);
	CHECK(output.Pixels == std::vector<uint8_t>{0, 0, 0, 255, 128, 128, 128, 255});
	control.LevelIn = {0.5, 0.5};
	CHECK(
		RenderGradientBase(output, geometry, keys, 0, nullptr, control) == GradientStatus::UndefinedDivision
	);
}

TEST_CASE("Gradient inverse axis and brightness curve use source defaults and order", "[imagegraph]") {
	using namespace engine::imagegraph;
	Image output{1, 1, {0, 0, 0, 0}};
	GradientGeometry geometry;
	geometry.CenterX = 0.5;
	geometry.CenterY = 0.5;
	const std::array<engine::imagegraph::detail::GradientKey, 2> keys{
		{{0, {0, 0, 0, 255}}, {1, {255, 255, 255, 255}}}
	};
	GradientRenderControls control;
	control.InverseAxis = 1.0;
	REQUIRE(RenderGradientBase(output, geometry, keys, 0, nullptr, control) == GradientStatus::Ok);
	CHECK(output.Pixels == std::vector<uint8_t>{128, 128, 128, 255});
	Curve constant = detail::IdentityColorCurve();
	constant.Header[3] = 0.5;
	constant.Header[4] = 0.5;
	control.InverseCurve = &constant;
	control.BrightnessCurve = &constant;
	REQUIRE(RenderGradientBase(output, geometry, keys, 0, nullptr, control) == GradientStatus::Ok);
	CHECK(output.Pixels == std::vector<uint8_t>{128, 128, 128, 255});
}

TEST_CASE("Gradient graph applies UV alpha and exposes map size diagnostic", "[imagegraph]") {
	using namespace engine::imagegraph;
	Document document;
	document.FormatVersion = 6;
	document.Nodes = {
		{"uv",
		 "image.solid",
		 "",
		 {},
		 {{"width", int64_t{1}}, {"height", int64_t{1}}, {"colour", Colour{255, 255, 0, 128}}}},
		{"gradient",
		 "image.gradient",
		 "",
		 {},
		 {{"width", int64_t{1}},
		  {"height", int64_t{1}},
		  {"gradient", Gradient{0, {{0, {0, 0, 0, 255}}, {1, {255, 255, 255, 255}}}}},
		  {"center", Vector2{0.5, 0.5}}}}
	};
	document.Links = {{"uv", "image", "gradient", "uv_map"}};
	document.Outputs = {{"out", "gradient", "image"}};
	Plan plan;
	Diagnostic diagnostic;
	Image output;
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	REQUIRE(Evaluate(document, plan, "out", output, diagnostic) == Status::Ok);
	CHECK(output.Pixels == std::vector<uint8_t>{255, 255, 255, 128});
	document.Nodes[0].Values[0].Data = int64_t{2};
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	CHECK(Evaluate(document, plan, "out", output, diagnostic) == Status::UnsupportedExecution);
	CHECK(diagnostic.NodeId == "gradient");
}

TEST_CASE("Gradient composes through Blend and palette Posterize with exact pixels", "[imagegraph]") {
	using namespace engine::imagegraph;
	Document document;
	document.FormatVersion = 6;
	document.Nodes = {
		{"background",
		 "image.solid",
		 "",
		 {},
		 {{"width", int64_t{2}}, {"height", int64_t{1}}, {"colour", Colour{0, 0, 0, 255}}}},
		{"gradient",
		 "image.gradient",
		 "",
		 {},
		 {{"width", int64_t{2}},
		  {"height", int64_t{1}},
		  {"gradient", Gradient{0, {{0, {0, 0, 0, 255}}, {1, {255, 255, 255, 255}}}}},
		  {"center", Vector2{1.0, 0.5}}}},
		{"blend", "image.blend", "", {}, {}},
		{"posterize",
		 "image.posterize",
		 "",
		 {},
		 {{"use_palette", true},
		  {"posterize_alpha", false},
		  {"palette",
		   ArrayValue{
			   ValueType::Colour,
			   {ElementValue{Colour{0, 0, 0, 255}}, ElementValue{Colour{255, 255, 255, 255}}}
		   }}}}
	};
	document.Links = {
		{"background", "image", "blend", "background"},
		{"gradient", "image", "blend", "foreground"},
		{"blend", "image", "posterize", "image"}
	};
	document.Outputs = {{"out", "posterize", "image"}};
	Plan plan;
	Diagnostic diagnostic;
	Image output;
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	REQUIRE(Evaluate(document, plan, "out", output, diagnostic) == Status::Ok);
	CHECK(output.Pixels == std::vector<uint8_t>{0, 0, 0, 255, 255, 255, 255, 255});
	const uint64_t hash = output.Hash;
	REQUIRE(Evaluate(document, plan, "out", output, diagnostic) == Status::Ok);
	CHECK(output.Hash == hash);
}

TEST_CASE("Gradient accepts a typed linked Angle and mapped scalar surface", "[imagegraph]") {
	using namespace engine::imagegraph;
	Document document;
	document.FormatVersion = 6;
	document.Nodes = {
		{"angle", "value.number", "", {}, {{"value", 180.0}}},
		{"map",
		 "image.solid",
		 "",
		 {},
		 {{"width", int64_t{1}}, {"height", int64_t{1}}, {"colour", Colour{255, 255, 255, 255}}}},
		{"gradient",
		 "image.gradient",
		 "",
		 {},
		 {{"width", int64_t{1}},
		  {"height", int64_t{1}},
		  {"gradient", Gradient{0, {{0, {0, 0, 0, 255}}, {1, {255, 255, 255, 255}}}}},
		  {"center", Vector2{0.0, 0.5}},
		  {"type", int64_t{1}},
		  {"radius", 0.5},
		  {"radius_max", 1.0}}}
	};
	document.Links = {
		{"angle", "number", "gradient", "angle_value"}, {"map", "image", "gradient", "radius_map"}
	};
	document.Outputs = {{"out", "gradient", "image"}};
	Plan plan;
	Diagnostic diagnostic;
	Image output;
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	REQUIRE(Evaluate(document, plan, "out", output, diagnostic) == Status::Ok);
	CHECK(output.Pixels == std::vector<uint8_t>{128, 128, 128, 255});
	document.Nodes[2].Values[4].Data = int64_t{0};
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	REQUIRE(Evaluate(document, plan, "out", output, diagnostic) == Status::Ok);
	CHECK(output.Pixels == std::vector<uint8_t>{0, 0, 0, 255});
}

TEST_CASE("Gradient graph uses v3 keys and reports unsupported key modes", "[imagegraph]") {
	using namespace engine::imagegraph;
	Document document;
	document.FormatVersion = 3;
	Node node;
	node.Id = "gradient";
	node.Type = "image.gradient";
	node.Values = {
		{"width", int64_t{2}},
		{"height", int64_t{1}},
		{"gradient", Gradient{0, {{0.0, {0, 0, 0, 255}}, {1.0, {255, 255, 255, 255}}}}},
		{"center", Vector2{1.0, 0.5}}
	};
	document.Nodes.push_back(node);
	document.Outputs.push_back({"out", "gradient", "image"});
	Plan plan;
	Diagnostic diagnostic;
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	Image output;
	REQUIRE(Evaluate(document, plan, "out", output, diagnostic) == Status::Ok);
	CHECK(output.Pixels == std::vector<uint8_t>{64, 64, 64, 255, 191, 191, 191, 255});
	document.Nodes[0].Values[2].Data = Gradient{7, {{0.0, {0, 0, 0, 255}}, {1.0, {255, 255, 255, 255}}}};
	CHECK(Compile(document, plan, diagnostic) == Status::InvalidValue);
	CHECK(diagnostic.Port == "gradient");
}

TEST_CASE("Gradient geometry follows four source shape equations", "[imagegraph]") {
	GradientGeometry controls;
	controls.CenterX = 2.0;
	controls.CenterY = 2.0;
	controls.UniformRatio = false;
	double progress = -1.0;
	REQUIRE(GradientProgress(controls, 4, 4, 0.75, 0.5, progress) == GradientStatus::Ok);
	CHECK(progress == 0.75);
	controls.Type = 1;
	REQUIRE(GradientProgress(controls, 4, 4, 0.75, 0.5, progress) == GradientStatus::Ok);
	CHECK(progress == 0.5);
	controls.Type = 2;
	REQUIRE(GradientProgress(controls, 4, 4, 0.5, 0.75, progress) == GradientStatus::Ok);
	CHECK(progress == 0.25);
	controls.Type = 3;
	REQUIRE(GradientProgress(controls, 4, 4, 0.75, 0.5, progress) == GradientStatus::Ok);
	CHECK(progress == 0.5);
}

TEST_CASE("Gradient loop and source scaling order are fixed", "[imagegraph]") {
	GradientGeometry controls;
	controls.CenterX = 0.5;
	controls.CenterY = 0.5;
	controls.Shift = 1.0;
	double progress = -1.0;
	REQUIRE(GradientProgress(controls, 1, 1, 0.5, 0.5, progress) == GradientStatus::Ok);
	CHECK(progress == 1.5);
	controls.Loop = 1;
	REQUIRE(GradientProgress(controls, 1, 1, 0.5, 0.5, progress) == GradientStatus::Ok);
	CHECK(progress == 0.5);
	controls.Loop = 2;
	REQUIRE(GradientProgress(controls, 1, 1, 0.5, 0.5, progress) == GradientStatus::Ok);
	CHECK(progress == 0.5);
	controls.Loop = 0;
	controls.Scale = 2.0;
	REQUIRE(GradientProgress(controls, 1, 1, 0.5, 0.5, progress) == GradientStatus::Ok);
	CHECK(progress == 1.0);
}

TEST_CASE("Gradient keys interpolate RGBA and preserve step boundaries", "[imagegraph]") {
	const std::array<GradientKey, 2> keys{{{0.0, {0, 0, 0, 0}}, {1.0, {255, 128, 64, 255}}}};
	std::array<double, 4> color{};
	REQUIRE(GradientColor(keys, 0, 0.5, color) == GradientStatus::Ok);
	CHECK(color == std::array<double, 4>{127.5, 64.0, 32.0, 127.5});
	REQUIRE(GradientColor(keys, 1, 0.5, color) == GradientStatus::Ok);
	CHECK(color == std::array<double, 4>{0.0, 0.0, 0.0, 0.0});
	REQUIRE(GradientColor(keys, 1, 1.0, color) == GradientStatus::Ok);
	CHECK(color == std::array<double, 4>{255.0, 128.0, 64.0, 255.0});
	REQUIRE(GradientColor(keys, 0, -1.0, color) == GradientStatus::Ok);
	CHECK(color == std::array<double, 4>{0.0, 0.0, 0.0, 0.0});
	REQUIRE(GradientColor(keys, 0, 2.0, color) == GradientStatus::Ok);
	CHECK(color == std::array<double, 4>{255.0, 128.0, 64.0, 255.0});
}

TEST_CASE("Gradient key modes follow pinned HSV Oklab gamma and CMYK shader equations", "[imagegraph]") {
	using namespace engine::imagegraph;
	const std::array<engine::imagegraph::detail::GradientKey, 2> keys{
		{{0, {255, 0, 0, 255}}, {1, {0, 255, 0, 255}}}
	};
	struct Expected {
		int64_t Mode;
		std::vector<uint8_t> Pixels;
	};
	const std::array expected{
		Expected{2, {255, 255, 0, 255}},
		Expected{3, {207, 167, 4, 255}},
		Expected{4, {186, 186, 0, 255}},
		Expected{5, {0, 0, 255, 255}},
		Expected{6, {128, 128, 0, 255}}
	};
	GradientGeometry geometry;
	geometry.CenterX = 0.5;
	geometry.CenterY = 0.5;
	for (const Expected &item : expected) {
		Image output{1, 1, {0, 0, 0, 0}};
		REQUIRE(RenderGradientBase(output, geometry, keys, item.Mode) == GradientStatus::Ok);
		CHECK(output.Pixels == item.Pixels);
	}
	const std::array<engine::imagegraph::detail::GradientKey, 2> black{
		{{0, {0, 0, 0, 255}}, {1, {255, 0, 0, 255}}}
	};
	std::array<double, 4> channels{};
	CHECK(GradientColor(black, 6, 0.5, channels) == GradientStatus::UndefinedCmykBlack);
}

TEST_CASE("Gradient custom key times sample at pixel centers", "[imagegraph]") {
	const std::array<GradientKey, 3> keys{
		{{0.0, {0, 0, 0, 255}}, {0.5, {200, 0, 0, 255}}, {1.0, {200, 200, 0, 255}}}
	};
	GradientGeometry controls;
	controls.CenterX = 2.0;
	controls.CenterY = 0.5;
	std::array<double, 4> color{};
	for (uint32_t x = 0; x < 4; x++) {
		double progress = -1.0;
		REQUIRE(GradientProgress(controls, 4, 1, (x + 0.5) / 4.0, 0.5, progress) == GradientStatus::Ok);
		REQUIRE(GradientColor(keys, 0, progress, color) == GradientStatus::Ok);
		const std::array<std::array<double, 4>, 4> expected{
			{{{50, 0, 0, 255}}, {{150, 0, 0, 255}}, {{200, 50, 0, 255}}, {{200, 150, 0, 255}}}
		};
		CHECK(color == expected[x]);
	}
}

TEST_CASE("Gradient rejects unsupported and undefined source controls", "[imagegraph]") {
	GradientGeometry controls;
	double progress = 7.0;
	controls.Scale = 0.0;
	CHECK(GradientProgress(controls, 4, 4, 0.25, 0.25, progress) == GradientStatus::UndefinedDivision);
	CHECK(progress == 7.0);
	controls.Scale = 1.0;
	controls.Type = 1;
	controls.Radius = 0.0;
	CHECK(GradientProgress(controls, 4, 4, 0.25, 0.25, progress) == GradientStatus::UndefinedDivision);
	controls.Type = 9;
	CHECK(GradientProgress(controls, 4, 4, 0.25, 0.25, progress) == GradientStatus::InvalidControl);
	const std::array<GradientKey, 2> duplicate{{{0.0, {}}, {0.0, {}}}};
	std::array<double, 4> color{};
	CHECK(GradientColor(duplicate, 0, 0.5, color) == GradientStatus::InvalidControl);
	CHECK(GradientColor(duplicate, 4, 0.5, color) == GradientStatus::InvalidControl);
}

TEST_CASE("Gradient default surface samples custom keys and mask alpha", "[imagegraph]") {
	const std::array<GradientKey, 3> keys{
		{{0.0, {0, 0, 0, 255}}, {0.5, {200, 0, 0, 255}}, {1.0, {200, 200, 0, 255}}}
	};
	GradientGeometry geometry;
	geometry.CenterX = 2.0;
	geometry.CenterY = 0.5;
	engine::imagegraph::Image output{4, 1, std::vector<uint8_t>(16), 0};
	const engine::imagegraph::Image mask{2, 1, {0, 0, 0, 255, 0, 0, 0, 128}, 0};
	REQUIRE(RenderGradientBase(output, geometry, keys, 0, &mask) == GradientStatus::Ok);
	CHECK(
		output.Pixels ==
		std::vector<uint8_t>{50, 0, 0, 255, 150, 0, 0, 255, 200, 50, 0, 128, 200, 150, 0, 128}
	);
}
