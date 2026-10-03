#include "../src/nodes/Source2DComplexGenerator.hpp"
#include "ComplexGeneratorFixture.hpp"

#include <engine/testing/Suite.hpp>

#include <catch2/catch_approx.hpp>

TEST_SUITE_ID("engine.imagegraph.source_gabor_noise")
using namespace complex_generator_test;
namespace {
	Document Gabor() {
		return Graph("pc.gabor_noise", {4, 3});
	}
}
TEST_CASE("Gabor fixed twenty-five-cell kernel retains literal seeded pixels", "[source_gabor_noise]") {
	Rows(Draw(Gabor()), {{145, 93, 162, 106}, {236, 136, 91, 214}, {143, 185, 197, 43}});
}
TEST_CASE("Gabor zero Density is an analytic constant white wave", "[source_gabor_noise]") {
	auto d = Gabor();
	Set(d, "density", 0.0);
	Rows(Draw(d), {{255, 255, 255, 255}, {255, 255, 255, 255}, {255, 255, 255, 255}});
}
TEST_CASE("Gabor Phase is converted to radians and occurs twice in the carrier", "[source_gabor_noise]") {
	auto d = Gabor();
	Set(d, "phase", 90.0);
	Rows(Draw(d), {{134, 142, 90, 107}, {49, 51, 168, 50}, {184, 135, 73, 208}});
}
TEST_CASE("Gabor rotates the aspect-corrected translated coordinates before Scale", "[source_gabor_noise]") {
	auto d = Gabor();
	Set(d, "rotation", 30.0);
	Rows(Draw(d), {{160, 147, 76, 173}, {81, 227, 30, 31}, {94, 240, 28, 57}});
}
TEST_CASE("Gabor negative seeds use floor-based shader modulo", "[source_gabor_noise]") {
	auto d = Gabor();
	Set(d, "seed", -17.0);
	Rows(Draw(d), {{158, 186, 196, 113}, {189, 54, 44, 122}, {113, 245, 58, 199}});
}
TEST_CASE("Gabor zero Sharpness keeps all twenty-five weights", "[source_gabor_noise]") {
	auto d = Gabor();
	Set(d, "sharpness", 0.0);
	Rows(Draw(d), {{107, 111, 129, 138}, {142, 146, 125, 108}, {164, 119, 109, 122}});
}
TEST_CASE("Gabor zero Scale is defined and samples one noise location", "[source_gabor_noise]") {
	auto d = Gabor();
	Set(d, "scale", Vector2{0, 0});
	Rows(Draw(d), {{242, 242, 242, 242}, {242, 242, 242, 242}, {242, 242, 242, 242}});
}
TEST_CASE(
	"Gabor levels preserve the source affine remap before grayscale centering", "[source_gabor_noise]"
) {
	auto d = Gabor();
	Set(d, "level_in", Vector2{-1, 1});
	Set(d, "level_out", Vector2{-1, 1});
	CHECK(Draw(d) == Draw(Gabor()));
	Set(d, "level_out", Vector2{0, 0});
	Rows(Draw(d), {{128, 128, 128, 128}, {128, 128, 128, 128}, {128, 128, 128, 128}});
}
TEST_CASE("Gabor scalar numeric maps read mean RGB at original pixel coordinates", "[source_gabor_noise]") {
	for (const char *name : {"density", "sharpness", "phase"}) {
		const std::string port = name;
		auto d = Gabor();
		Set(d, port + "_mapped", true);
		Set(d, port + "_map_range", Vector2{1, 3});
		d.Nodes.push_back(Solid("map", {1, 1}, {255, 0, 0, 0}));
		d.Links = {{"map", "surface_out", "generator", port + "_map"}};
		auto expected = Gabor();
		Set(expected, port, 1 + 2.0 / 3);
		CHECK(Draw(d) == Draw(expected));
	}
}
TEST_CASE("Gabor linked fixed map endpoints remain one shader uniform", "[source_gabor_noise]") {
	auto d = Gabor();
	Set(d, "density_mapped", true);
	d.Nodes.push_back(Array("range", ValueType::Scalar, 1.0, 3.0));
	d.Nodes.push_back(Solid("map", {1, 1}, {255, 0, 0, 0}));
	d.Links = {
		{"range", "array", "generator", "density"}, {"map", "surface_out", "generator", "density_map"}
	};
	auto expected = Gabor();
	Set(expected, "density", 1 + 2.0 / 3);
	CHECK(Draw(d) == Draw(expected));
}
TEST_CASE("Gabor missing numeric maps observe the first endpoint", "[source_gabor_noise]") {
	for (const char *name : {"density", "sharpness", "phase"}) {
		const std::string port = name;
		auto d = Gabor();
		Set(d, port + "_mapped", true);
		Set(d, port + "_map_range", Vector2{2, 9});
		auto expected = Gabor();
		Set(expected, port, 2.0);
		CHECK(Draw(d) == Draw(expected));
	}
}
TEST_CASE("Gabor authored scalar mapped controls retain physical-slot precedence", "[source_gabor_noise]") {
	auto d = Gabor();
	Set(d, "density", 1.5);
	Set(d, "density_mapped", true);
	Set(d, "density_map_range", Vector2{9, 9});
	auto expected = Gabor();
	Set(expected, "density", 1.5);
	CHECK(Draw(d) == Draw(expected));
}
TEST_CASE(
	"Gabor Scale Map sampler is inert while its two uniform components remain anisotropic",
	"[source_gabor_noise]"
) {
	auto d = Gabor();
	Set(d, "scale", Vector2{3, 7});
	Set(d, "scale_mapped", true);
	d.Nodes.push_back(Solid("map", {1, 1}, {255, 255, 255, 128}));
	d.Links = {{"map", "surface_out", "generator", "scale_map"}};
	auto expected = Gabor();
	Set(expected, "scale", Vector2{3, 7});
	CHECK(Draw(d) == Draw(expected));
	d.Nodes[1] = Solid("map", {1, 1}, {0, 0, 0, 0});
	CHECK(Draw(d) == Draw(expected));
}
TEST_CASE(
	"Gabor mapped Scale uses two source endpoints rather than an invented Vec4", "[source_gabor_noise]"
) {
	auto d = Gabor();
	Set(d, "scale_mapped", true);
	Set(d, "scale_map_range", Vector2{3, 7});
	auto expected = Gabor();
	Set(expected, "scale", Vector2{3, 7});
	CHECK(Draw(d) == Draw(expected));
	Set(d, "scale_map_range", Vector4{0, 0, 3, 7});
	Plan p;
	Diagnostic diag;
	CHECK(Compile(d, p, diag) == Status::TypeMismatch);
}
TEST_CASE("Gabor unobserved automatic source seed is an explicit boundary", "[source_gabor_noise]") {
	auto d = Gabor();
	std::erase_if(d.Nodes[0].Values, [](const auto &v) { return v.Port == "seed"; });
	Refuse(d, Status::UnsupportedExecution, "seed");
}
TEST_CASE("Gabor zero Level In span refuses atomically", "[source_gabor_noise]") {
	auto d = Gabor();
	Set(d, "level_in", Vector2{.5, .5});
	Refuse(d, Status::UnsupportedExecution, "level_in");
}
TEST_CASE(
	"Gabor underflowed weight normalization refuses instead of inventing a flat noise", "[source_gabor_noise]"
) {
	auto d = Gabor();
	Set(d, "sharpness", 1e300);
	Refuse(d, Status::UnsupportedExecution, "sharpness");
}
TEST_CASE("Gabor raw zero canvas refuses even though storage clamps to one", "[source_gabor_noise]") {
	Refuse(Graph("pc.gabor_noise", {0, 1}), Status::UnsupportedExecution, "dimension");
}
TEST_CASE(
	"Gabor typed color depths preserve native storage and bounded finite grayscale", "[source_gabor_noise]"
) {
	for (int64_t depth = 2; depth <= 8; ++depth) {
		auto d = Gabor();
		Set(d, "attribute_color_depth", EnumValue{depth});
		const auto image = Draw(d);
		const auto format = DescribeSurfaceFormat(image.Format);
		REQUIRE(format);
		CHECK(image.Pixels.size() == 4 * 3 * format->BytesPerPixel);
		SurfacePixel p;
		REQUIRE(LoadSurfacePixel(image, 0, 0, p));
		CHECK(p[0] >= 0);
		CHECK(p[0] <= 1);
	}
}
TEST_CASE("Gabor floating masked output preserves the RGBA8 intermediate boundary", "[source_gabor_noise]") {
	auto d = Graph("pc.gabor_noise", {1, 1});
	Set(d, "density", 0.0);
	Set(d, "level_out", Vector2{.123456, .123456});
	Set(d, "attribute_color_depth", EnumValue{5});
	d.Nodes.push_back(Solid("mask", {1, 1}, {255, 255, 255, 255}));
	d.Links = {{"mask", "surface_out", "generator", "mask"}};
	SurfacePixel pixel;
	REQUIRE(LoadSurfacePixel(Draw(d), 0, 0, pixel));
	CHECK(pixel[0] == Catch::Approx(143.0 / 255).epsilon(1e-6));
}
TEST_CASE("Gabor UV Mix zero still retains UV alpha", "[source_gabor_noise]") {
	auto d = Gabor();
	Set(d, "uv_mix", 0.0);
	d.Nodes.push_back(Solid("uv", {1, 1}, {0, 255, 0, 128}));
	d.Links = {{"uv", "surface_out", "generator", "uv_map"}};
	auto expected = Draw(Gabor());
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
TEST_CASE("Gabor source Position numeric links retain Reference conversion", "[source_gabor_noise]") {
	auto d = Gabor();
	Set(d, "position", Vector2{.25, .5});
	const auto expected = Draw(d);
	d.Nodes.push_back({"position", "pc.vector2", "", {}, {{"x", .25}, {"y", .5}}});
	d.Links = {{"position", "vector", "generator", "position"}};
	CHECK(Draw(d) == expected);
}
TEST_CASE(
	"Gabor Vec2 surface getters preserve source dimensions for Position Scale and Augment",
	"[source_gabor_noise]"
) {
	for (const char *name : {"position", "scale", "augment"}) {
		const std::string port = name;
		auto d = Gabor();
		d.Nodes.push_back(Solid("vector", {2, 1}, {255, 255, 255, 255}));
		d.Links = {{"vector", "surface_out", "generator", port}};
		auto expected = Gabor();
		Set(expected, port, Vector2{2, 1});
		if (port == "position") Set(expected, "position_unit", EnumValue{0});
		CHECK(Draw(d) == Draw(expected));
	}
}
TEST_CASE(
	"Gabor heterogeneous canvases retain first-prepared Position in all schedules", "[source_gabor_noise]"
) {
	auto d = Gabor();
	Set(d, "position", Vector2{.25, .5});
	d.Nodes.push_back(Array("dimensions", ValueType::Vector2, Vector2{4, 3}, Vector2{8, 3}));
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
		auto first = Gabor();
		Set(first, "position", Vector2{1, 1.5});
		Set(first, "position_unit", EnumValue{0});
		CHECK(images.Images[0] == Draw(first));
		auto later = Graph("pc.gabor_noise", {8, 3});
		Set(later, "position", Vector2{1, 1.5});
		Set(later, "position_unit", EnumValue{0});
		CHECK(images.Images[1] == Draw(later));
	}
}
TEST_CASE(
	"Gabor original later stencil work refuses before allocating a small first output", "[source_gabor_noise]"
) {
	auto d = Graph("pc.gabor_noise", {1, 1});
	d.Nodes.push_back(Array("dimensions", ValueType::Vector2, Vector2{1, 1}, Vector2{3000, 3000}));
	d.Links = {{"dimensions", "array", "generator", "dimension"}};
	Refuse(d, Status::LimitExceeded, "dimension", 100000, "whole-array work");
}
TEST_CASE("Gabor byte-cap refusal preserves the prior output", "[source_gabor_noise]") {
	Refuse(Gabor(), Status::LimitExceeded, "", 1);
}
TEST_CASE("Gabor native persistence and source instances preserve mapped controls", "[source_gabor_noise]") {
	auto d = Gabor();
	Set(d, "phase_mapped", true);
	Set(d, "phase_map_range", Vector2{30, 60});
	const auto expected = Draw(d);
	Document restored;
	Diagnostic diag;
	REQUIRE(Read(Write(d), restored, diag) == Status::Ok);
	CHECK(Draw(restored) == expected);
	Node copy{"copy", "pc.gabor_noise", "", {}, {}};
	copy.InstanceBase = "generator";
	restored.Nodes.push_back(std::move(copy));
	restored.Outputs[0].NodeId = "copy";
	CHECK(Draw(restored) == expected);
}

TEST_CASE("Gabor SliRange getters project linked surface dimensions", "[source_gabor_noise]") {
	for (const char *port : {"level_in", "level_out"}) {
		auto d = Gabor();
		d.Nodes.push_back(Solid("levels", {2, 1}, {255, 255, 255, 255}));
		d.Links.push_back({"levels", "surface_out", "generator", port});
		auto expected = Gabor();
		Set(expected, port, Vector2{2, 1});
		CHECK(Draw(d) == Draw(expected));
	}
}

TEST_CASE(
	"Gabor whole-surface Vec2 and SliRange getters retain source nonsurface fallback", "[source_gabor_noise]"
) {
	for (const char *port : {"position", "scale", "augment", "level_out"}) {
		auto d = Gabor();
		SurfaceRows(d, port);
		auto expected = Gabor();
		Set(expected, port, Vector2{1, 1});
		if (std::string_view(port) == "position") Set(expected, "position_unit", EnumValue{0});
		CHECK(Draw(d) == Draw(expected));
	}
	auto d = Gabor();
	SurfaceRows(d, "level_in");
	Refuse(d, Status::UnsupportedExecution, "level_in");
}
TEST_CASE("Gabor seed period is the source floor modulo 10000", "[source_gabor_noise]") {
	auto d = Gabor();
	auto expected = Draw(d);
	Set(d, "seed", 10017.0);
	CHECK(Draw(d) == expected);
}

TEST_CASE(
	"pc.gabor_noise scalar surface getters preserve original dimensions as processor rows",
	"[source_gabor_noise]"
) {
	for (const char *port : {"density", "sharpness", "uv_mix"}) {
		auto d = Graph("pc.gabor_noise");
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
			auto expected = Graph("pc.gabor_noise");
			if (std::string_view(port) == "max_iteration")
				Set(expected, port, int64_t(i == 0 ? 2 : 1));
			else
				Set(expected, port, i == 0 ? 2. : 1.);
			CHECK(rows.Images[i] == Draw(expected));
		}
	}
}

TEST_CASE("Gabor later canvas byte quote refuses before first-row publication", "[source_gabor_noise]") {
	auto d = Graph("pc.gabor_noise", {1, 1});
	d.Nodes.push_back(Array("dimensions", ValueType::Vector2, Vector2{1, 1}, Vector2{32, 32}));
	d.Links = {{"dimensions", "array", "generator", "dimension"}};
	Refuse(d, Status::LimitExceeded, "surface_out", 30000, "byte budget");
}
TEST_CASE(
	"Gabor typed later-row workspace quotes widest storage before first publication", "[source_gabor_noise]"
) {
	auto d = Graph("pc.gabor_noise", {32, 32});
	const auto *entry = FindCatalogueEntry("pc.gabor_noise");
	REQUIRE(entry);
	EvaluationRequest request;
	detail::NodeContext context(d.Nodes[0], *entry, request);
	context.Values = {{"dimension", Vector2{32, 32}}, {"dimension_unit", EnumValue{0}}};
	context.ProcessorCount = 2;
	context.ByteBudget = 15000;
	Value depths = ArrayValue{ValueType::Scalar, {3.0, 5.0}, {}};
	const std::array original{std::pair<std::string_view, const Value *>{"attribute_color_depth", &depths}};
	context.ProcessorOriginalValues = original;
	CHECK_FALSE(
		detail::source2d::ComplexBatchAdmission(
			context, detail::source2d::GABOR_PIXEL_WORK, "surface_out", "dimension"
		)
	);
	CHECK(context.FailureCode == Status::LimitExceeded);
	CHECK(context.FailurePort == "surface_out");
	CHECK(context.OutputImages.empty());
	context.ByteBudget = 50000;
	context.FailureCode = Status::Ok;
	CHECK(
		detail::source2d::ComplexBatchAdmission(
			context, detail::source2d::GABOR_PIXEL_WORK, "surface_out", "dimension"
		)
	);
	CHECK(context.OutputImages.empty());
}
TEST_CASE(
	"Gabor public static depth-array refusals preserve the accepted plan and image", "[source_gabor_noise]"
) {
	auto d = Gabor();
	Plan plan;
	Diagnostic diagnostic;
	REQUIRE(Compile(d, plan, diagnostic) == Status::Ok);
	auto output = Draw(d);
	const auto prior = output;
	const auto accepted = d;
	SECTION("authored array") {
		Set(d, "attribute_color_depth", ArrayValue{ValueType::Scalar, {3.0, 5.0}, {}});
		CHECK(Compile(d, plan, diagnostic) == Status::TypeMismatch);
	}
	SECTION("linked array") {
		d.Nodes.push_back(Array("depths", ValueType::Scalar, 3.0, 5.0));
		d.Links = {{"depths", "array", "generator", "attribute_color_depth"}};
		CHECK(Compile(d, plan, diagnostic) == Status::UnsupportedExecution);
	}
	CHECK(diagnostic.Port == "attribute_color_depth");
	CHECK(Evaluate(accepted, plan, "out", {}, output, diagnostic) == Status::Ok);
	CHECK(output == prior);
}
