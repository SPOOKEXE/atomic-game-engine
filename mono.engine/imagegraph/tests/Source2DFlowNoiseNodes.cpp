#include "ComplexGeneratorFixture.hpp"

#include <engine/testing/Suite.hpp>

#include <catch2/catch_approx.hpp>

TEST_SUITE_ID("engine.imagegraph.source_flow_noise")
using namespace complex_generator_test;
namespace {
	Document Flow(Vector2 dimension = {4, 3}) {
		auto d = Graph("pc.flow_noise", dimension);
		std::erase_if(d.Nodes[0].Values, [](const auto &v) { return v.Port == "seed"; });
		return d;
	}
	void CheckImage(const Image &actual, const Image &expected) {
		CHECK(actual.Width == expected.Width);
		CHECK(actual.Height == expected.Height);
		CHECK(actual.Format == expected.Format);
		CHECK(actual.Pixels == expected.Pixels);
		CHECK(actual.Hash == SurfaceHash(actual));
		CHECK(actual == expected);
	}
}
TEST_CASE("Flow source defaults retain Y aspect and coupled sine cosine updates", "[source_flow_noise]") {
	Rows(Draw(Flow()), {{239, 254, 253, 242}, {153, 238, 253, 240}, {156, 203, 220, 255}});
}
TEST_CASE("Flow fractional Detail advances by one from its authored lower endpoint", "[source_flow_noise]") {
	auto d = Flow();
	Set(d, "detail", Vector2{.5, 2.5});
	Rows(Draw(d), {{214, 253, 252, 253}, {244, 237, 206, 158}, {238, 255, 235, 200}});
}
TEST_CASE("Flow inverted Detail preserves the source empty loop", "[source_flow_noise]") {
	auto d = Flow();
	Set(d, "detail", Vector2{8, 1});
	Rows(Draw(d), {{159, 214, 248, 253}, {159, 214, 248, 253}, {159, 214, 248, 253}});
}
TEST_CASE("Flow negative ranges preserve defined signed divisors", "[source_flow_noise]") {
	auto d = Flow();
	Set(d, "detail", Vector2{-3, -1});
	Rows(Draw(d), {{241, 251, 245, 220}, {190, 227, 255, 254}, {136, 196, 229, 255}});
	Set(d, "detail", Vector2{-1.5, .5});
	Rows(Draw(d), {{252, 234, 207, 69}, {254, 203, 151, 81}, {241, 246, 217, 113}});
}
TEST_CASE("Flow Rotation is uploaded in degrees and converted once in the shader", "[source_flow_noise]") {
	auto d = Flow();
	Set(d, "rotation", 30.0);
	Rows(Draw(d), {{175, 250, 250, 234}, {147, 185, 194, 233}, {73, 89, 125, 207}});
}
TEST_CASE("Flow Progress enters both coupled phase updates", "[source_flow_noise]") {
	auto d = Flow();
	Set(d, "progress", 1.25);
	Rows(Draw(d), {{253, 235, 253, 210}, {133, 152, 233, 252}, {84, 180, 218, 247}});
}
TEST_CASE("Flow Reference Position precedes raw dimension division", "[source_flow_noise]") {
	auto d = Flow();
	Set(d, "position", Vector2{.25, .5});
	Rows(Draw(d), {{73, 140, 188, 214}, {32, 157, 143, 234}, {134, 239, 254, 253}});
	const auto expected = Draw(d);
	Set(d, "position", Vector2{1, 1.5});
	Set(d, "position_unit", EnumValue{0});
	CheckImage(Draw(d), expected);
}
TEST_CASE("Flow zero Scale still receives source deformation updates", "[source_flow_noise]") {
	auto d = Flow();
	Set(d, "scale", Vector2{0, 0});
	Rows(Draw(d), {{166, 166, 166, 166}, {166, 166, 166, 166}, {166, 166, 166, 166}});
	Set(d, "detail", Vector2{8, 1});
	Rows(Draw(d), {{128, 128, 128, 128}, {128, 128, 128, 128}, {128, 128, 128, 128}});
}
TEST_CASE(
	"Flow Level In and reversed Level Out preserve unclamped arithmetic before storage", "[source_flow_noise]"
) {
	auto d = Flow();
	Set(d, "level_in", Vector2{.2, .8});
	Set(d, "level_out", Vector2{1, 0});
	Rows(Draw(d), {{0, 0, 0, 0}, {86, 0, 0, 0}, {81, 1, 0, 0}});
}
TEST_CASE("Flow Detail zero divisor is refused atomically", "[source_flow_noise]") {
	for (const Vector2 range : {Vector2{0, 0}, Vector2{-1, 2}}) {
		auto d = Flow();
		Set(d, "detail", range);
		Refuse(d, Status::UnsupportedExecution, "detail");
	}
}
TEST_CASE("Flow stalled native Detail increment is an explicit boundary", "[source_flow_noise]") {
	auto d = Flow();
	Set(d, "detail", Vector2{1e20, 1e20});
	Refuse(d, Status::UnsupportedExecution, "detail");
}
TEST_CASE("Flow zero input level span is refused atomically", "[source_flow_noise]") {
	auto d = Flow();
	Set(d, "level_in", Vector2{.5, .5});
	Refuse(d, Status::UnsupportedExecution, "level_in");
}
TEST_CASE("Flow raw zero width is not repaired by minimum-one allocation", "[source_flow_noise]") {
	Refuse(Flow({0, 1}), Status::UnsupportedExecution, "dimension");
}
TEST_CASE("Flow Pixel and Project dimensions retain half-even allocation rounding", "[source_flow_noise]") {
	auto d = Flow({2.5, 3.5});
	const auto expected = Draw(d);
	CHECK(expected.Width == 2);
	CHECK(expected.Height == 4);
	d.Project = ProjectSettings{.SurfaceWidth = 2, .SurfaceHeight = 2};
	Set(d, "dimension", Vector2{1.25, 1.75});
	Set(d, "dimension_unit", EnumValue{1});
	CheckImage(Draw(d), expected);
}
TEST_CASE("Flow Mask dimension units retain scale and original canvas", "[source_flow_noise]") {
	auto d = Flow({.5, .5});
	Set(d, "dimension_unit", EnumValue{2});
	d.Nodes.push_back(Solid("mask", {8, 4}, {255, 255, 255, 255}));
	d.Links = {{"mask", "surface_out", "generator", "mask"}};
	CheckImage(Draw(d), Draw(Flow({4, 2})));
}
TEST_CASE("Flow UV Mix zero retains alpha and complete image metadata", "[source_flow_noise]") {
	auto d = Flow();
	Set(d, "uv_mix", 0.0);
	d.Nodes.push_back(Solid("uv", {1, 1}, {0, 255, 0, 128}));
	d.Links = {{"uv", "surface_out", "generator", "uv_map"}};
	auto expected = Draw(Flow());
	for (size_t i = 3; i < expected.Pixels.size(); i += 4)
		expected.Pixels[i] = 128;
	expected.Hash = SurfaceHash(expected);
	CheckImage(Draw(d), expected);
}
TEST_CASE("Flow UV red and inverted green define mapped coordinates", "[source_flow_noise]") {
	auto d = Flow();
	Set(d, "detail", Vector2{8, 1});
	d.Nodes.push_back(Solid("uv", {1, 1}, {255, 0, 0, 128}));
	d.Links = {{"uv", "surface_out", "generator", "uv_map"}};
	const auto image = Draw(d);
	for (size_t i = 0; i < image.Pixels.size(); i += 4) {
		CHECK(image.Pixels[i] == 243);
		CHECK(image.Pixels[i + 1] == 243);
		CHECK(image.Pixels[i + 2] == 243);
		CHECK(image.Pixels[i + 3] == 128);
	}
	CHECK(image.Hash == SurfaceHash(image));
}
TEST_CASE("Flow mask mean RGB and alpha compose through source RGBA8", "[source_flow_noise]") {
	auto d = Flow();
	d.Nodes.push_back(Solid("mask", {1, 1}, {128, 128, 128, 128}));
	d.Links = {{"mask", "surface_out", "generator", "mask"}};
	auto expected = Draw(Flow());
	for (size_t i = 3; i < expected.Pixels.size(); i += 4)
		expected.Pixels[i] = 64;
	expected.Hash = SurfaceHash(expected);
	CheckImage(Draw(d), expected);
	Set(d, "mask_alpha_only", true);
	CheckImage(Draw(d), expected);
}
TEST_CASE("Flow seven source depths retain native typed storage", "[source_flow_noise]") {
	for (int64_t depth = 2; depth <= 8; ++depth) {
		auto d = Flow();
		Set(d, "attribute_color_depth", EnumValue{depth});
		const auto image = Draw(d);
		const auto format = DescribeSurfaceFormat(image.Format);
		REQUIRE(format);
		CHECK(image.Pixels.size() == 12 * format->BytesPerPixel);
		CHECK(image.Hash == SurfaceHash(image));
	}
}
TEST_CASE("Flow inherited depth resolves from the authored project", "[source_flow_noise]") {
	auto d = Flow();
	Set(d, "attribute_color_depth", EnumValue{1});
	d.Project = ProjectSettings{.ColorDepth = 3};
	CHECK(Draw(d).Format == SurfaceFormat::RGBA32Float);
}
TEST_CASE("Flow float mask preserves the RGBA8 intermediate precision boundary", "[source_flow_noise]") {
	auto d = Flow({1, 1});
	Set(d, "level_out", Vector2{.123456, .123456});
	Set(d, "attribute_color_depth", EnumValue{5});
	SurfacePixel pixel;
	REQUIRE(LoadSurfacePixel(Draw(d), 0, 0, pixel));
	CHECK(pixel[0] == Catch::Approx(.123456).margin(1e-7));
	d.Nodes.push_back(Solid("mask", {1, 1}, {255, 255, 255, 255}));
	d.Links = {{"mask", "surface_out", "generator", "mask"}};
	REQUIRE(LoadSurfacePixel(Draw(d), 0, 0, pixel));
	CHECK(pixel[0] == Catch::Approx(31. / 255).margin(1e-7));
}
TEST_CASE("Flow numeric Position links retain Reference units", "[source_flow_noise]") {
	auto d = Flow();
	Set(d, "position", Vector2{.25, .5});
	const auto expected = Draw(d);
	d.Nodes.push_back({"position", "pc.vector2", "", {}, {{"x", .25}, {"y", .5}}});
	d.Links = {{"position", "vector", "generator", "position"}};
	CheckImage(Draw(d), expected);
}
TEST_CASE("Flow linked scalar Detail expands to one two-component source tuple", "[source_flow_noise]") {
	auto d = Flow();
	d.Nodes.push_back({"count", "pc.number_simple", "", {}, {{"value", 3.0}}});
	d.Links = {{"count", "number", "generator", "detail"}};
	Rows(Draw(d), {{175, 226, 253, 249}, {168, 221, 251, 251}, {139, 198, 240, 255}});
}
TEST_CASE("Flow scalar surface tuple getters bypass Position units", "[source_flow_noise]") {
	for (const char *port : {"position", "scale", "detail", "level_in", "level_out"}) {
		auto d = Flow();
		d.Nodes.push_back(Solid("numeric", {2, 1}, {0, 0, 0, 0}));
		d.Links = {{"numeric", "surface_out", "generator", port}};
		auto expected = Flow();
		Set(expected, port, Vector2{2, 1});
		if (std::string_view(port) == "position") Set(expected, "position_unit", EnumValue{0});
		CheckImage(Draw(d), Draw(expected));
	}
}
TEST_CASE(
	"Flow scalar surface getters retain dimensions before processor row selection", "[source_flow_noise]"
) {
	for (const char *port : {"progress", "uv_mix"}) {
		auto d = Flow();
		d.Nodes.push_back(Solid("numeric", {2, 1}, {0, 0, 0, 0}));
		d.Links = {{"numeric", "surface_out", "generator", port}};
		Plan p;
		Diagnostic diag;
		REQUIRE(Compile(d, p, diag) == Status::Ok);
		ImageArray rows;
		auto status = EvaluateArray(d, p, "out", {}, rows, diag);
		INFO(diag.Message);
		REQUIRE(status == Status::Ok);
		REQUIRE(rows.Images.size() == 2);
		for (size_t i = 0; i < 2; ++i) {
			auto expected = Flow();
			Set(expected, port, i == 0 ? 2. : 1.);
			CheckImage(rows.Images[i], Draw(expected));
		}
	}
}
TEST_CASE("Flow whole-surface tuples use source nonsurface fallback", "[source_flow_noise]") {
	for (const char *port : {"position", "scale", "detail", "level_out"}) {
		auto d = Flow();
		SurfaceRows(d, port);
		auto expected = Flow();
		Set(expected, port, Vector2{1, 1});
		if (std::string_view(port) == "position") Set(expected, "position_unit", EnumValue{0});
		CheckImage(Draw(d), Draw(expected));
	}
	auto d = Flow();
	SurfaceRows(d, "level_in");
	Refuse(d, Status::UnsupportedExecution, "level_in");
}
TEST_CASE("Flow surface Dimension arrays preserve sizes and first-canvas references", "[source_flow_noise]") {
	auto d = Flow();
	Set(d, "position", Vector2{.25, .5});
	SurfaceRows(d, "dimension");
	Plan p;
	Diagnostic diag;
	REQUIRE(Compile(d, p, diag) == Status::Ok);
	ImageArray images;
	const auto status = EvaluateArray(d, p, "out", {}, images, diag);
	INFO(diag.Message);
	REQUIRE(status == Status::Ok);
	REQUIRE(images.Images.size() == 2);
	for (size_t i = 0; i < 2; ++i) {
		auto expected = Flow(i == 0 ? Vector2{2, 1} : Vector2{3, 2});
		Set(expected, "position", Vector2{.5, .5});
		Set(expected, "position_unit", EnumValue{0});
		CheckImage(images.Images[i], Draw(expected));
	}
}
TEST_CASE(
	"Flow heterogeneous numeric canvases retain first-prepared Position in all schedules",
	"[source_flow_noise]"
) {
	auto d = Flow();
	Set(d, "position", Vector2{.25, .5});
	d.Nodes.push_back(Array("dimensions", ValueType::Vector2, Vector2{4, 3}, Vector2{8, 3}));
	d.Links = {{"dimensions", "array", "generator", "dimension"}};
	for (int64_t mode = 0; mode < 4; ++mode) {
		Set(d, "attribute_array_process", EnumValue{mode});
		Plan p;
		Diagnostic diag;
		REQUIRE(Compile(d, p, diag) == Status::Ok);
		ImageArray images;
		REQUIRE(EvaluateArray(d, p, "out", {}, images, diag) == Status::Ok);
		REQUIRE(images.Images.size() == 2);
		for (size_t i = 0; i < 2; ++i) {
			auto expected = Flow(i == 0 ? Vector2{4, 3} : Vector2{8, 3});
			Set(expected, "position", Vector2{1, 1.5});
			Set(expected, "position_unit", EnumValue{0});
			CheckImage(images.Images[i], Draw(expected));
		}
	}
}
TEST_CASE(
	"Flow Detail arrays retain distinct source bounds across schedules and persistence", "[source_flow_noise]"
) {
	auto d = Flow();
	d.Nodes.push_back(Array("detail", ValueType::Vector2, Vector2{1, 1}, Vector2{1, 3}));
	d.Links = {{"detail", "array", "generator", "detail"}};
	for (int64_t mode = 0; mode < 4; ++mode) {
		Set(d, "attribute_array_process", EnumValue{mode});
		Plan p;
		Diagnostic diag;
		REQUIRE(Compile(d, p, diag) == Status::Ok);
		ImageArray images;
		REQUIRE(EvaluateArray(d, p, "out", {}, images, diag) == Status::Ok);
		REQUIRE(images.Images.size() == 2);
		for (size_t i = 0; i < 2; ++i) {
			auto expected = Flow();
			Set(expected, "detail", i == 0 ? Vector2{1, 1} : Vector2{1, 3});
			CheckImage(images.Images[i], Draw(expected));
		}
		Document restored;
		REQUIRE(Read(Write(d), restored, diag) == Status::Ok);
		REQUIRE(Compile(restored, p, diag) == Status::Ok);
		ImageArray replay;
		REQUIRE(EvaluateArray(restored, p, "out", {}, replay, diag) == Status::Ok);
		CHECK(replay.Images == images.Images);
		CHECK(replay.Items.size() == images.Items.size());
		for (size_t i = 0; i < images.Items.size(); ++i) {
			CHECK(std::get<size_t>(replay.Items[i].Data) == std::get<size_t>(images.Items[i].Data));
		}
	}
}
TEST_CASE("Flow original later Detail work refuses before small first output", "[source_flow_noise]") {
	auto d = Flow({1, 1});
	d.Nodes.push_back(Array("detail", ValueType::Vector2, Vector2{1, 1}, Vector2{1, 1e9}));
	d.Links = {{"detail", "array", "generator", "detail"}};
	Refuse(d, Status::LimitExceeded, "detail", 100000, "whole-array work");
}
TEST_CASE("Flow original later canvas work refuses before small first output", "[source_flow_noise]") {
	auto d = Flow({1, 1});
	d.Nodes.push_back(Array("dimensions", ValueType::Vector2, Vector2{1, 1}, Vector2{3000, 3000}));
	d.Links = {{"dimensions", "array", "generator", "dimension"}};
	Refuse(d, Status::LimitExceeded, "detail", 100000, "whole-array work");
}
TEST_CASE("Flow later output bytes are admitted as widest storage with metadata", "[source_flow_noise]") {
	auto d = Flow({1, 1});
	d.Nodes.push_back(Array("dimensions", ValueType::Vector2, Vector2{1, 1}, Vector2{32, 32}));
	d.Links = {{"dimensions", "array", "generator", "dimension"}};
	Refuse(d, Status::LimitExceeded, "surface_out", 30000, "byte budget");
}
TEST_CASE("Flow public byte-cap failure preserves the previous image", "[source_flow_noise]") {
	Refuse(Flow(), Status::LimitExceeded, "", 1);
}
TEST_CASE("Flow source instances preserve controls through native persistence", "[source_flow_noise]") {
	auto d = Flow();
	Set(d, "progress", 1.25);
	Set(d, "position", Vector2{.25, .5});
	const auto expected = Draw(d);
	Document restored;
	Diagnostic diag;
	REQUIRE(Read(Write(d), restored, diag) == Status::Ok);
	CheckImage(Draw(restored), expected);
	Node copy{"copy", "pc.flow_noise", "", {}, {}};
	copy.InstanceBase = "generator";
	restored.Nodes.push_back(copy);
	restored.Outputs[0].NodeId = "copy";
	CheckImage(Draw(restored), expected);
}
TEST_CASE("Flow keyframed Progress reaches its authored source instance", "[source_flow_noise]") {
	auto d = Flow();
	d.Keyframes = {{"generator", "progress", 0, 0.0}, {"generator", "progress", 2, 1.25}};
	d.Nodes[0].SourceAnimatedInputs = {"progress"};
	Node copy{"copy", "pc.flow_noise", "", {}, {}};
	copy.InstanceBase = "generator";
	d.Nodes.push_back(copy);
	d.Outputs[0].NodeId = "copy";
	Plan p;
	Diagnostic diag;
	REQUIRE(Compile(d, p, diag) == Status::Ok);
	Image actual;
	EvaluationRequest request;
	request.Tick = 2;
	REQUIRE(Evaluate(d, p, "out", request, actual, diag) == Status::Ok);
	auto expected = Flow();
	Set(expected, "progress", 1.25);
	CheckImage(actual, Draw(expected));
	request.Tick = 0;
	REQUIRE(Evaluate(d, p, "out", request, actual, diag) == Status::Ok);
	CheckImage(actual, Draw(Flow()));
}
