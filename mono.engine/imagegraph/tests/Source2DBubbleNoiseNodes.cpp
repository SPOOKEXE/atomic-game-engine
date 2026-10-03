#include "ComplexGeneratorFixture.hpp"

#include <engine/testing/Suite.hpp>

#include <catch2/catch_approx.hpp>
TEST_SUITE_ID("engine.imagegraph.source_bubble_noise")
using namespace complex_generator_test;
namespace {
	Document Bubble(Vector2 size = {4, 3}) {
		return Graph("pc.noise_bubble", size);
	}
	void Same(const Image &a, const Image &b) {
		CHECK(a.Width == b.Width);
		CHECK(a.Height == b.Height);
		CHECK(a.Format == b.Format);
		CHECK(a.Hash == SurfaceHash(a));
		CHECK(a == b);
	}
	ImageArray Batch(const Document &d) {
		Plan p;
		Diagnostic diag;
		REQUIRE(Compile(d, p, diag) == Status::Ok);
		ImageArray out;
		const auto status = EvaluateArray(d, p, "out", {}, out, diag);
		INFO(diag.Message);
		REQUIRE(status == Status::Ok);
		return out;
	}
}
TEST_CASE("Bubble source default Line Max retains seed hashes and Y aspect", "[source_bubble_noise]") {
	Rows(Draw(Bubble()), {{154, 0, 38, 0}, {32, 0, 32, 0}, {77, 32, 0, 0}});
}
TEST_CASE("Bubble Fill uses increasing authored thickness edges", "[source_bubble_noise]") {
	auto d = Bubble();
	Set(d, "mode", EnumValue{1});
	Set(d, "thickness", .1);
	Rows(Draw(d), {{120, 174, 174, 174}, {172, 174, 174, 174}, {161, 174, 174, 174}});
}
TEST_CASE("Bubble Add preserves overlapping lines rather than maximum", "[source_bubble_noise]") {
	auto d = Bubble();
	Set(d, "blend_mode", EnumValue{1});
	Rows(Draw(d), {{154, 0, 38, 0}, {32, 0, 32, 0}, {115, 32, 0, 0}});
}
TEST_CASE("Bubble Line thickness overrides the minimum texel support", "[source_bubble_noise]") {
	auto d = Bubble();
	Set(d, "thickness", .2);
	Rows(Draw(d), {{165, 0, 42, 0}, {99, 5, 39, 0}, {128, 39, 0, 0}});
}
TEST_CASE("Bubble Density changes the authored raw-width iteration count", "[source_bubble_noise]") {
	auto d = Bubble();
	Set(d, "density", 1.5);
	Rows(Draw(d), {{209, 6, 119, 0}, {116, 0, 32, 68}, {237, 32, 1, 122}});
}
TEST_CASE("Bubble negative seed uses floor modulo rather than remainder", "[source_bubble_noise]") {
	auto d = Bubble();
	Set(d, "seed", -17.);
	Rows(Draw(d), {{2, 0, 0, 0}, {2, 0, 0, 13}, {8, 0, 18, 0}});
	auto expected = Draw(d);
	Set(d, "seed", 99983.);
	Same(Draw(d), expected);
}
TEST_CASE("Bubble Scale and reversed Opacity preserve source interpolation", "[source_bubble_noise]") {
	auto d = Bubble();
	Set(d, "scale", Vector2{.2, .4});
	Rows(Draw(d), {{0, 29, 156, 74}, {29, 105, 0, 0}, {0, 34, 1, 0}});
	d = Bubble();
	Set(d, "opacity", Vector2{1, .2});
	Rows(Draw(d), {{103, 0, 184, 0}, {22, 0, 156, 0}, {184, 156, 0, 0}});
}
TEST_CASE("Bubble reversed output level maps after blending", "[source_bubble_noise]") {
	auto d = Bubble();
	Set(d, "level_out", Vector2{1, 0});
	Rows(Draw(d), {{101, 255, 217, 255}, {223, 255, 223, 255}, {178, 223, 255, 255}});
}
TEST_CASE("Bubble subinteger and negative Density keep the literal empty loop", "[source_bubble_noise]") {
	for (double density : {0., -.5, .249}) {
		auto d = Bubble();
		Set(d, "density", density);
		Rows(Draw(d), {{0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}});
	}
	auto d = Bubble();
	Set(d, "density", 0.);
	Set(d, "mode", EnumValue{1});
	Set(d, "thickness", 0.);
	Rows(Draw(d), {{0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}});
}
TEST_CASE("Bubble density truncates toward zero before the shader loop", "[source_bubble_noise]") {
	auto d = Bubble();
	Set(d, "density", .4999);
	auto expected = Bubble();
	Set(expected, "density", .25);
	Same(Draw(d), Draw(expected));
}
TEST_CASE("Bubble undefined Fill smoothstep is refused atomically", "[source_bubble_noise]") {
	for (double thickness : {0., -.1, 1e-300}) {
		auto d = Bubble();
		Set(d, "mode", EnumValue{1});
		Set(d, "thickness", thickness);
		Refuse(d, Status::UnsupportedExecution, "thickness");
	}
}
TEST_CASE("Bubble zero raw canvas and Level In divisors are explicit", "[source_bubble_noise]") {
	Refuse(Bubble({0, 1}), Status::UnsupportedExecution, "dimension");
	auto d = Bubble();
	Set(d, "level_in", Vector2{1, 1});
	Refuse(d, Status::UnsupportedExecution, "level_in");
}
TEST_CASE(
	"Bubble authored seed is mandatory and native shader integer overflow is refused", "[source_bubble_noise]"
) {
	auto d = Bubble();
	std::erase_if(d.Nodes[0].Values, [](const auto &v) { return v.Port == "seed"; });
	Refuse(d, Status::UnsupportedExecution, "seed");
	d = Bubble();
	Set(d, "density", 1e20);
	Refuse(d, Status::UnsupportedExecution, "density");
}
TEST_CASE("Bubble seven source depths carry complete typed storage", "[source_bubble_noise]") {
	for (int64_t depth = 2; depth <= 8; ++depth) {
		auto d = Bubble();
		Set(d, "attribute_color_depth", EnumValue{depth});
		const auto image = Draw(d);
		const auto format = DescribeSurfaceFormat(image.Format);
		REQUIRE(format);
		CHECK(image.Pixels.size() == 12 * format->BytesPerPixel);
		CHECK(image.Hash == SurfaceHash(image));
	}
}
TEST_CASE("Bubble Project dimensions round half even without changing raw count", "[source_bubble_noise]") {
	auto d = Bubble({2.5, 3.5});
	const auto expected = Draw(d);
	CHECK(expected.Width == 2);
	CHECK(expected.Height == 4);
	d.Project = ProjectSettings{.SurfaceWidth = 2, .SurfaceHeight = 2};
	Set(d, "dimension", Vector2{1.25, 1.75});
	Set(d, "dimension_unit", EnumValue{1});
	Same(Draw(d), expected);
}
TEST_CASE("Bubble UV Map alpha is consumed at zero Mix", "[source_bubble_noise]") {
	auto d = Bubble();
	auto expected = Draw(d);
	for (size_t i = 3; i < expected.Pixels.size(); i += 4)
		expected.Pixels[i] = 128;
	expected.Hash = SurfaceHash(expected);
	d.Nodes.push_back(Solid("uv", {1, 1}, {255, 0, 0, 128}));
	d.Links = {{"uv", "surface_out", "generator", "uv_map"}};
	Set(d, "uv_mix", 0.);
	Same(Draw(d), expected);
}
TEST_CASE("Bubble UV Mix replaces coordinates including source Y inversion", "[source_bubble_noise]") {
	auto d = Bubble();
	d.Nodes.push_back(Solid("uv", {1, 1}, {255, 0, 0, 128}));
	d.Links = {{"uv", "surface_out", "generator", "uv_map"}};
	const auto image = Draw(d);
	REQUIRE(image.Pixels.size() == 48);
	for (size_t i = 0; i < image.Pixels.size(); i += 4) {
		CHECK(image.Pixels[i] == 0);
		CHECK(image.Pixels[i + 1] == 0);
		CHECK(image.Pixels[i + 2] == 0);
		CHECK(image.Pixels[i + 3] == 128);
	}
}
TEST_CASE("Bubble source mask has the RGBA8 intermediate and inert Alpha Only", "[source_bubble_noise]") {
	auto d = Bubble({1, 1});
	Set(d, "level_out", Vector2{.123456, .123456});
	Set(d, "attribute_color_depth", EnumValue{5});
	SurfacePixel pixel;
	REQUIRE(LoadSurfacePixel(Draw(d), 0, 0, pixel));
	CHECK(pixel[0] == Catch::Approx(.123456).margin(1e-7));
	d.Nodes.push_back(Solid("mask", {1, 1}, {128, 128, 128, 128}));
	d.Links = {{"mask", "surface_out", "generator", "mask"}};
	auto image = Draw(d);
	REQUIRE(LoadSurfacePixel(image, 0, 0, pixel));
	CHECK(pixel[0] == Catch::Approx(31. / 255).margin(1e-7));
	CHECK(pixel[3] == Catch::Approx(64. / 255).margin(1e-7));
	Set(d, "mask_alpha_only", true);
	Same(Draw(d), image);
}
TEST_CASE("Bubble SliRange surface getters project dimensions before processing", "[source_bubble_noise]") {
	for (const char *port : {"scale", "opacity", "level_in", "level_out"}) {
		auto d = Bubble();
		d.Nodes.push_back(Solid("tuple", {2, 1}, {0, 0, 0, 0}));
		d.Links = {{"tuple", "surface_out", "generator", port}};
		auto expected = Bubble();
		Set(expected, port, Vector2{2, 1});
		Same(Draw(d), Draw(expected));
	}
}
TEST_CASE("Bubble whole-surface arrays use source SliRange fallback", "[source_bubble_noise]") {
	for (const char *port : {"scale", "opacity", "level_out"}) {
		auto d = Bubble();
		SurfaceRows(d, port);
		auto expected = Bubble();
		Set(expected, port, Vector2{1, 1});
		Same(Draw(d), Draw(expected));
	}
	auto d = Bubble();
	SurfaceRows(d, "level_in");
	Refuse(d, Status::UnsupportedExecution, "level_in");
}
TEST_CASE("Bubble Slider surface getters retain dimension component rows", "[source_bubble_noise]") {
	for (const char *port : {"density", "thickness", "uv_mix"}) {
		auto d = Bubble();
		d.Nodes.push_back(Solid("control", {2, 1}, {0, 0, 0, 0}));
		d.Links = {{"control", "surface_out", "generator", port}};
		auto rows = Batch(d);
		REQUIRE(rows.Images.size() == 2);
		for (size_t i = 0; i < 2; ++i) {
			auto expected = Bubble();
			Set(expected, port, i == 0 ? 2. : 1.);
			Same(rows.Images[i], Draw(expected));
		}
	}
}
TEST_CASE("Bubble Dimension surface arrays preserve individual raw canvases", "[source_bubble_noise]") {
	auto d = Bubble();
	SurfaceRows(d, "dimension");
	auto rows = Batch(d);
	REQUIRE(rows.Images.size() == 2);
	Same(rows.Images[0], Draw(Bubble({2, 1})));
	Same(rows.Images[1], Draw(Bubble({3, 2})));
}
TEST_CASE("Bubble scalar Range link duplicates both components", "[source_bubble_noise]") {
	auto d = Bubble();
	d.Nodes.push_back({"range", "pc.number_simple", "", {}, {{"value", .4}}});
	d.Links = {{"range", "number", "generator", "scale"}};
	auto expected = Bubble();
	Set(expected, "scale", Vector2{.4, .4});
	Same(Draw(d), Draw(expected));
}
TEST_CASE("Bubble all source array schedules preserve Density and canvas rows", "[source_bubble_noise]") {
	for (int64_t mode = 0; mode < 4; ++mode) {
		auto d = Bubble();
		Set(d, "attribute_array_process", EnumValue{mode});
		d.Nodes.push_back(Array("canvases", ValueType::Vector2, Vector2{4, 3}, Vector2{8, 3}));
		d.Nodes.push_back(Array("density", ValueType::Scalar, .25, 1.));
		d.Links = {
			{"canvases", "array", "generator", "dimension"}, {"density", "array", "generator", "density"}
		};
		auto rows = Batch(d);
		REQUIRE(rows.Images.size() == (mode < 2 ? 2 : 4));
		for (size_t i = 0; i < rows.Images.size(); ++i) {
			auto expected = Bubble({double(rows.Images[i].Width), double(rows.Images[i].Height)});
			const size_t densityRow = mode < 2 ? i : i % 2;
			CHECK(rows.Images[i].Width == (mode == 2 ? (i / 2 == 0 ? 4 : 8) : (i % 2 == 0 ? 4 : 8)));
			Set(expected, "density", densityRow == 0 ? .25 : 1.);
			Same(rows.Images[i], Draw(expected));
		}
	}
}
TEST_CASE("Bubble expensive later Density is admitted before first small output", "[source_bubble_noise]") {
	auto d = Bubble();
	d.Nodes.push_back(Array("density", ValueType::Scalar, .25, 1000000.));
	d.Links = {{"density", "array", "generator", "density"}};
	Refuse(d, Status::LimitExceeded, "density", 100000, "work");
}
TEST_CASE("Bubble expensive later raw canvas is admitted before first output", "[source_bubble_noise]") {
	auto d = Bubble();
	d.Nodes.push_back(Array("sizes", ValueType::Vector2, Vector2{1, 1}, Vector2{3000, 3000}));
	d.Links = {{"sizes", "array", "generator", "dimension"}};
	Refuse(d, Status::LimitExceeded, "density", 100000, "work");
}
TEST_CASE("Bubble widest later output quote and public byte refusal remain atomic", "[source_bubble_noise]") {
	auto d = Bubble();
	Set(d, "density", 0.);
	d.Nodes.push_back(Array("sizes", ValueType::Vector2, Vector2{1, 1}, Vector2{32, 32}));
	d.Links = {{"sizes", "array", "generator", "dimension"}};
	Refuse(d, Status::LimitExceeded, "surface_out", 30000);
	Refuse(Bubble(), Status::LimitExceeded, "", 1);
}
TEST_CASE(
	"Bubble native persistence and inherited instance regenerate complete images", "[source_bubble_noise]"
) {
	auto d = Bubble();
	Set(d, "mode", EnumValue{1});
	Set(d, "thickness", .1);
	Set(d, "blend_mode", EnumValue{1});
	const auto expected = Draw(d);
	Document decoded;
	Diagnostic diag;
	REQUIRE(Read(Write(d), decoded, diag) == Status::Ok);
	Same(Draw(decoded), expected);
	Node instance{"instance", "pc.noise_bubble", "", {}, {}};
	instance.InstanceBase = "generator";
	decoded.Nodes.push_back(std::move(instance));
	decoded.Outputs[0].NodeId = "instance";
	Same(Draw(decoded), expected);
}
TEST_CASE(
	"Bubble Mask units use linked dimensions and inherited format resolves from project",
	"[source_bubble_noise]"
) {
	auto d = Bubble({.5, .5});
	Set(d, "dimension_unit", EnumValue{2});
	d.Nodes.push_back(Solid("mask", {8, 6}, {255, 255, 255, 255}));
	d.Links = {{"mask", "surface_out", "generator", "mask"}};
	Same(Draw(d), Draw(Bubble()));
	d = Bubble();
	Set(d, "attribute_color_depth", EnumValue{1});
	d.Project = ProjectSettings{.ColorDepth = 3};
	CHECK(Draw(d).Format == SurfaceFormat::RGBA32Float);
}
TEST_CASE("Bubble inherited keyframed Density retains authored tick values", "[source_bubble_noise]") {
	auto d = Bubble();
	d.Keyframes = {{"generator", "density", 0, .5}, {"generator", "density", 2, 1.5}};
	d.Nodes[0].SourceAnimatedInputs = {"density"};
	Node copy{"copy", "pc.noise_bubble", "", {}, {}};
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
	auto expected = Bubble();
	Set(expected, "density", 1.5);
	Same(actual, Draw(expected));
	request.Tick = 0;
	REQUIRE(Evaluate(d, p, "out", request, actual, diag) == Status::Ok);
	Same(actual, Draw(Bubble()));
}
TEST_CASE("Bubble authored range processor rows preserve native save load", "[source_bubble_noise]") {
	auto d = Bubble();
	d.Nodes.push_back(Array("scale", ValueType::Vector2, Vector2{.2, .4}, Vector2{.5, .8}));
	d.Links = {{"scale", "array", "generator", "scale"}};
	auto images = Batch(d);
	REQUIRE(images.Images.size() == 2);
	auto expected = Bubble();
	Set(expected, "scale", Vector2{.2, .4});
	Same(images.Images[0], Draw(expected));
	Same(images.Images[1], Draw(Bubble()));
	Document decoded;
	Diagnostic diag;
	REQUIRE(Read(Write(d), decoded, diag) == Status::Ok);
	auto replay = Batch(decoded);
	CHECK(replay.Images == images.Images);
	REQUIRE(replay.Items.size() == images.Items.size());
	for (size_t i = 0; i < images.Items.size(); ++i)
		CHECK(std::get<size_t>(replay.Items[i].Data) == std::get<size_t>(images.Items[i].Data));
}
