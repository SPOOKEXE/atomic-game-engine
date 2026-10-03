#include "ComplexGeneratorFixture.hpp"

#include <engine/testing/Suite.hpp>

TEST_SUITE_ID("engine.imagegraph.source_julia_set")
using namespace complex_generator_test;
TEST_CASE("Julia authored-eight recurrence returns literal escape fractions", "[source_julia_set]") {
	Rows(Draw(Graph("pc.julia_set")), {{0, 32, 32, 0}, {64, 96, 96, 64}, {64, 96, 96, 64}, {0, 32, 32, 0}});
}
TEST_CASE("Julia applies row-vector rotation before the recurrence", "[source_julia_set]") {
	auto d = Graph("pc.julia_set");
	Set(d, "rotation", 30.0);
	Rows(Draw(d), {{0, 32, 32, 0}, {32, 128, 223, 64}, {64, 223, 128, 32}, {0, 32, 32, 0}});
}
TEST_CASE("Julia C Reference conversion precedes division by raw dimensions", "[source_julia_set]") {
	auto d = Graph("pc.julia_set");
	Set(d, "c", Vector2{-.4, .6});
	Rows(Draw(d), {{0, 32, 32, 0}, {32, 96, 255, 64}, {64, 255, 96, 32}, {0, 32, 32, 0}});
	Set(d, "c", Vector2{-1.6, 2.4});
	Set(d, "c_unit", EnumValue{0});
	Rows(Draw(d), {{0, 32, 32, 0}, {32, 96, 255, 64}, {64, 255, 96, 32}, {0, 32, 32, 0}});
}
TEST_CASE("Julia unused MAX_ITERATIONS constant does not clamp an authored 256", "[source_julia_set]") {
	auto d = Graph("pc.julia_set");
	Set(d, "max_iteration", int64_t{256});
	Rows(Draw(d), {{0, 1, 1, 0}, {2, 3, 3, 2}, {2, 3, 3, 2}, {0, 1, 1, 0}});
}
TEST_CASE("Julia negative iterations preserve the source empty-loop ratio", "[source_julia_set]") {
	auto d = Graph("pc.julia_set");
	Set(d, "max_iteration", int64_t{-7});
	Rows(Draw(d), {{255, 255, 255, 255}, {255, 255, 255, 255}, {255, 255, 255, 255}, {255, 255, 255, 255}});
}
TEST_CASE("Julia negative and zero divergence thresholds escape immediately", "[source_julia_set]") {
	for (double threshold : {0., -1.}) {
		auto d = Graph("pc.julia_set");
		Set(d, "diverge_threshold", threshold);
		Rows(Draw(d), {{0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}});
	}
}
TEST_CASE("Julia zero Max Iteration refuses the undefined result atomically", "[source_julia_set]") {
	auto d = Graph("pc.julia_set");
	Set(d, "max_iteration", int64_t{0});
	Refuse(d, Status::UnsupportedExecution, "max_iteration");
}
TEST_CASE("Julia zero Scale refuses the shader division atomically", "[source_julia_set]") {
	auto d = Graph("pc.julia_set");
	Set(d, "scale", Vector2{0, 1});
	Refuse(d, Status::UnsupportedExecution, "scale");
}
TEST_CASE("Julia raw zero canvas is not replaced by its minimum-one allocation", "[source_julia_set]") {
	Refuse(Graph("pc.julia_set", {0, 1}), Status::UnsupportedExecution, "dimension");
}
TEST_CASE("Julia raw Pixel and Project dimensions use half-even surface rounding", "[source_julia_set]") {
	auto d = Graph("pc.julia_set", {2.5, 3.5});
	const auto pixels = Draw(d);
	CHECK(pixels.Width == 2);
	CHECK(pixels.Height == 4);
	d.Project = ProjectSettings{.SurfaceWidth = 2, .SurfaceHeight = 2};
	Set(d, "dimension", Vector2{1.25, 1.75});
	Set(d, "dimension_unit", EnumValue{1});
	CHECK(Draw(d) == pixels);
}
TEST_CASE("Julia Mask dimensions preserve authored scaling", "[source_julia_set]") {
	auto d = Graph("pc.julia_set", {.5, .5});
	Set(d, "dimension_unit", EnumValue{2});
	d.Nodes.push_back(Solid("mask", {8, 4}, {255, 255, 255, 255}));
	d.Links = {{"mask", "surface_out", "generator", "mask"}};
	const auto image = Draw(d);
	CHECK(image.Width == 4);
	CHECK(image.Height == 2);
	CHECK(image == Draw(Graph("pc.julia_set", {4, 2})));
}
TEST_CASE("Julia UV Mix zero still multiplies source UV alpha", "[source_julia_set]") {
	auto d = Graph("pc.julia_set");
	Set(d, "uv_mix", 0.0);
	d.Nodes.push_back(Solid("uv", {1, 1}, {0, 255, 0, 128}));
	d.Links = {{"uv", "surface_out", "generator", "uv_map"}};
	auto expected = Draw(Graph("pc.julia_set"));
	for (size_t i = 3; i < expected.Pixels.size(); i += 4)
		expected.Pixels[i] = 128;
	expected.Hash = SurfaceHash(expected);
	const auto actual = Draw(d);
	CHECK(actual.Width == expected.Width);
	CHECK(actual.Height == expected.Height);
	CHECK(actual.Format == expected.Format);
	CHECK(actual.Pixels == expected.Pixels);
	CHECK(actual.Hash == SurfaceHash(actual));
	CHECK(actual == expected);
}
TEST_CASE("Julia empty mask composes mean RGB and alpha through RGBA8", "[source_julia_set]") {
	auto d = Graph("pc.julia_set");
	d.Nodes.push_back(Solid("mask", {1, 1}, {128, 128, 128, 128}));
	d.Links = {{"mask", "surface_out", "generator", "mask"}};
	auto expected = Draw(Graph("pc.julia_set"));
	for (size_t i = 3; i < expected.Pixels.size(); i += 4)
		expected.Pixels[i] = 64;
	expected.Hash = SurfaceHash(expected);
	const auto actual = Draw(d);
	CHECK(actual.Width == expected.Width);
	CHECK(actual.Height == expected.Height);
	CHECK(actual.Format == expected.Format);
	CHECK(actual.Pixels == expected.Pixels);
	CHECK(actual.Hash == SurfaceHash(actual));
	CHECK(actual == expected);
	Set(d, "mask_alpha_only", true);
	CHECK(Draw(d) == expected);
}
TEST_CASE("Julia numeric Position links still apply Reference units", "[source_julia_set]") {
	auto d = Graph("pc.julia_set");
	d.Nodes.push_back({"position", "pc.vector2", "", {}, {{"x", .5}, {"y", .5}}});
	d.Links = {{"position", "vector", "generator", "position"}};
	CHECK(Draw(d) == Draw(Graph("pc.julia_set")));
}
TEST_CASE(
	"Julia surface Vec2 getters preserve pixel dimensions without unit conversion", "[source_julia_set]"
) {
	for (const char *name : {"c", "position", "scale"}) {
		const std::string port = name;
		auto d = Graph("pc.julia_set");
		d.Nodes.push_back(Solid("vector", {2, 1}, {255, 255, 255, 255}));
		d.Links = {{"vector", "surface_out", "generator", port}};
		auto expected = Graph("pc.julia_set");
		Set(expected, port, Vector2{2, 1});
		if (port != "scale") Set(expected, port + "_unit", EnumValue{0});
		CHECK(Draw(d) == Draw(expected));
	}
}
TEST_CASE("Julia batched canvases retain first-prepared C and Position references", "[source_julia_set]") {
	auto d = Graph("pc.julia_set");
	d.Nodes.push_back(Array("dimensions", ValueType::Vector2, Vector2{4, 4}, Vector2{8, 4}));
	d.Links = {{"dimensions", "array", "generator", "dimension"}};
	for (int64_t mode = 0; mode < 4; ++mode) {
		Set(d, "attribute_array_process", EnumValue{mode});
		Plan p;
		Diagnostic diag;
		REQUIRE(Compile(d, p, diag) == Status::Ok);
		ImageArray images;
		const auto status = EvaluateArray(d, p, "out", {}, images, diag);
		INFO(diag.Message);
		REQUIRE(status == Status::Ok);
		REQUIRE(images.Images.size() == 2);
		CHECK(images.Images[0] == Draw(Graph("pc.julia_set")));
		auto later = Graph("pc.julia_set", {8, 4});
		Set(later, "c", Vector2{-4, 0});
		Set(later, "c_unit", EnumValue{0});
		Set(later, "position", Vector2{2, 2});
		Set(later, "position_unit", EnumValue{0});
		CHECK(images.Images[1] == Draw(later));
	}
}
TEST_CASE("Julia linked iteration rows match independent scalar recurrences", "[source_julia_set]") {
	auto d = Graph("pc.julia_set");
	d.Nodes.push_back(Array("iterations", ValueType::Integer, int64_t{2}, int64_t{8}));
	d.Links = {{"iterations", "array", "generator", "max_iteration"}};
	Plan p;
	Diagnostic diag;
	REQUIRE(Compile(d, p, diag) == Status::Ok);
	ImageArray images;
	REQUIRE(EvaluateArray(d, p, "out", {}, images, diag) == Status::Ok);
	REQUIRE(images.Images.size() == 2);
	auto first = Graph("pc.julia_set");
	Set(first, "max_iteration", int64_t{2});
	CHECK(images.Images[0] == Draw(first));
	CHECK(images.Images[1] == Draw(Graph("pc.julia_set")));
}
TEST_CASE(
	"Julia original later iteration work refuses before the first output allocation", "[source_julia_set]"
) {
	auto d = Graph("pc.julia_set", {1, 1});
	d.Nodes.push_back(Array("iterations", ValueType::Integer, int64_t{1}, int64_t{1000000000}));
	d.Links = {{"iterations", "array", "generator", "max_iteration"}};
	Refuse(d, Status::LimitExceeded, "max_iteration", 100000, "whole-array work");
}
TEST_CASE(
	"Julia original later canvas work refuses before the first output allocation", "[source_julia_set]"
) {
	auto d = Graph("pc.julia_set", {1, 1});
	d.Nodes.push_back(Array("dimensions", ValueType::Vector2, Vector2{1, 1}, Vector2{3000, 3000}));
	d.Links = {{"dimensions", "array", "generator", "dimension"}};
	Refuse(d, Status::LimitExceeded, "max_iteration", 100000, "whole-array work");
}
TEST_CASE("Julia public byte refusal preserves the previous image", "[source_julia_set]") {
	Refuse(Graph("pc.julia_set"), Status::LimitExceeded, "", 1);
}
TEST_CASE("Julia source node stays RGBA8 without an invented Color Depth control", "[source_julia_set]") {
	auto d = Graph("pc.julia_set");
	CHECK(Draw(d).Format == SurfaceFormat::RGBA8Unorm);
	Set(d, "attribute_color_depth", EnumValue{5});
	Plan p;
	Diagnostic diag;
	CHECK(Compile(d, p, diag) == Status::UnknownPort);
}
TEST_CASE("Julia persistent source instances retain recurrence and controls", "[source_julia_set]") {
	auto d = Graph("pc.julia_set");
	Set(d, "c", Vector2{-.4, .6});
	const auto expected = Draw(d);
	Document restored;
	Diagnostic diag;
	REQUIRE(Read(Write(d), restored, diag) == Status::Ok);
	CHECK(Draw(restored) == expected);
	Node copy{"copy", "pc.julia_set", "", {}, {}};
	copy.InstanceBase = "generator";
	restored.Nodes.push_back(std::move(copy));
	restored.Outputs[0].NodeId = "copy";
	CHECK(Draw(restored) == expected);
}

TEST_CASE("Julia nonfinite transformed coordinates refuse atomically", "[source_julia_set]") {
	auto d = Graph("pc.julia_set");
	Set(d, "scale", Vector2{1e-310, 1});
	Refuse(d, Status::UnsupportedExecution, "scale");
}
TEST_CASE("Julia nonfinite recurrence refuses atomically", "[source_julia_set]") {
	auto d = Graph("pc.julia_set");
	Set(d, "c", Vector2{1e200, 1e200});
	Refuse(d, Status::UnsupportedExecution, "c");
}

TEST_CASE("Julia out-of-profile negative shader integer refuses atomically", "[source_julia_set]") {
	auto d = Graph("pc.julia_set");
	Set(d, "max_iteration", int64_t{-2147483649LL});
	Refuse(d, Status::UnsupportedExecution, "max_iteration");
}

TEST_CASE("Julia whole-surface Vec2 getters bypass units before processor selection", "[source_julia_set]") {
	for (const char *port : {"c", "position", "scale"}) {
		auto d = Graph("pc.julia_set");
		SurfaceRows(d, port);
		auto expected = Graph("pc.julia_set");
		Set(expected, port, Vector2{1, 1});
		if (std::string_view(port) != "scale") Set(expected, std::string(port) + "_unit", EnumValue{0});
		CHECK(Draw(d) == Draw(expected));
	}
}

TEST_CASE(
	"pc.julia_set scalar surface getters preserve original dimensions as processor rows", "[source_julia_set]"
) {
	for (const char *port : {"max_iteration", "diverge_threshold", "uv_mix"}) {
		auto d = Graph("pc.julia_set");
		d.Nodes.push_back(Solid("numeric", {2, 1}, {0, 0, 0, 0}));
		d.Links.push_back({"numeric", "surface_out", "generator", port});
		Plan plan;
		Diagnostic diag;
		REQUIRE(Compile(d, plan, diag) == Status::Ok);
		ImageArray rows;
		auto status = EvaluateArray(d, plan, "out", {}, rows, diag);
		INFO(diag.Message);
		REQUIRE(status == Status::Ok);
		REQUIRE(rows.Images.size() == 2);
		for (size_t i = 0; i < 2; ++i) {
			auto expected = Graph("pc.julia_set");
			if (std::string_view(port) == "max_iteration")
				Set(expected, port, int64_t(i == 0 ? 2 : 1));
			else
				Set(expected, port, i == 0 ? 2. : 1.);
			CHECK(rows.Images[i] == Draw(expected));
		}
	}
}
