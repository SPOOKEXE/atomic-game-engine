#include "ComplexGeneratorFixture.hpp"

#include <engine/testing/Suite.hpp>

#include <catch2/catch_approx.hpp>
TEST_SUITE_ID("engine.imagegraph.source_cristal_noise")
using namespace complex_generator_test;
namespace {
	Document Cristal(Vector2 dimension = {4, 3}) {
		return Graph("pc.noise_cristal", dimension);
	}
	void Same(const Image &a, const Image &b) {
		CHECK(a.Width == b.Width);
		CHECK(a.Height == b.Height);
		CHECK(a.Format == b.Format);
		CHECK(a.Pixels == b.Pixels);
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
TEST_CASE("Cristal source defaults retain literal TAU and powered Oilnoise", "[source_cristal_noise]") {
	Rows(Draw(Cristal()), {{23, 30, 48, 98}, {15, 64, 34, 119}, {12, 43, 57, 29}});
}
TEST_CASE("Cristal one and three iterations preserve last q normalization", "[source_cristal_noise]") {
	auto d = Cristal();
	Set(d, "iteration", int64_t{1});
	Rows(Draw(d), {{255, 255, 144, 255}, {255, 34, 14, 57}, {102, 12, 6, 18}});
	Set(d, "iteration", int64_t{3});
	Rows(Draw(d), {{16, 14, 100, 255}, {15, 19, 165, 228}, {32, 35, 95, 35}});
}
TEST_CASE("Cristal Phase rotates pos repeatedly inside the recurrence", "[source_cristal_noise]") {
	auto d = Cristal();
	Set(d, "phase", 37.);
	Rows(Draw(d), {{28, 33, 63, 255}, {67, 33, 62, 29}, {82, 40, 255, 40}});
}
TEST_CASE(
	"Cristal Reference Position and Pixel Position agree before raw division", "[source_cristal_noise]"
) {
	auto d = Cristal();
	Set(d, "position", Vector2{.25, .5});
	Rows(Draw(d), {{31, 44, 102, 72}, {32, 29, 118, 52}, {51, 23, 30, 48}});
	const auto expected = Draw(d);
	Set(d, "position", Vector2{1, 1.5});
	Set(d, "position_unit", EnumValue{0});
	Same(Draw(d), expected);
}
TEST_CASE("Cristal anisotropic and zero Scale retain source aPos initialization", "[source_cristal_noise]") {
	auto d = Cristal();
	Set(d, "scale", Vector2{2, .5});
	Rows(Draw(d), {{57, 244, 57, 9}, {34, 176, 46, 7}, {17, 135, 29, 8}});
	Set(d, "scale", Vector2{0, 0});
	Rows(Draw(d), {{21, 21, 21, 21}, {21, 21, 21, 21}, {21, 21, 21, 21}});
}
TEST_CASE("Cristal signed seed uses literal floor modulo100000", "[source_cristal_noise]") {
	auto d = Cristal();
	Set(d, "seed", -17.);
	Rows(Draw(d), {{22, 32, 104, 98}, {13, 62, 38, 86}, {13, 35, 55, 42}});
	const auto expected = Draw(d);
	Set(d, "seed", 99983.);
	Same(Draw(d), expected);
}
TEST_CASE(
	"Cristal Color RGB and Gamma precede saturation while color alpha is inert", "[source_cristal_noise]"
) {
	auto d = Cristal();
	Set(d, "color", Colour{255, 128, 0, 17});
	Set(d, "gamma", .5);
	const auto image = Draw(d);
	REQUIRE(image.Width == 4);
	REQUIRE(image.Height == 3);
	const uint8_t expected[][3] = {
		{12, 6, 0},
		{15, 7, 0},
		{24, 12, 0},
		{49, 25, 0},
		{7, 4, 0},
		{32, 16, 0},
		{17, 9, 0},
		{59, 30, 0},
		{6, 3, 0},
		{21, 11, 0},
		{28, 14, 0},
		{14, 7, 0}
	};
	for (size_t i = 0; i < 12; ++i) {
		for (size_t j = 0; j < 3; ++j)
			CHECK(image.Pixels[i * 4 + j] == expected[i][j]);
		CHECK(image.Pixels[i * 4 + 3] == 255);
	}
	Set(d, "color", Colour{255, 128, 0, 255});
	Same(Draw(d), image);
}
TEST_CASE(
	"Cristal Gamma zero and negative values clamp to black without UI range enforcement",
	"[source_cristal_noise]"
) {
	for (double gamma : {0., -1.}) {
		auto d = Cristal();
		Set(d, "gamma", gamma);
		Rows(Draw(d), {{0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}});
	}
}
TEST_CASE("Cristal reversed output levels follow the initial RGB clamp", "[source_cristal_noise]") {
	auto d = Cristal();
	Set(d, "level_in", Vector2{.1, .8});
	Set(d, "level_out", Vector2{1, 0});
	Rows(Draw(d), {{255, 249, 223, 151}, {255, 201, 243, 122}, {255, 230, 210, 250}});
}
TEST_CASE("Cristal nonpositive Iteration is an undefined output divisor", "[source_cristal_noise]") {
	for (int64_t iterations : {0, -1, -15}) {
		auto d = Cristal();
		Set(d, "iteration", iterations);
		Refuse(d, Status::UnsupportedExecution, "iteration");
	}
}
TEST_CASE("Cristal recurrence overflow is diagnosed rather than hidden by clamp", "[source_cristal_noise]") {
	auto d = Cristal({1, 1});
	Set(d, "iteration", int64_t{4000});
	Refuse(d, Status::UnsupportedExecution, "iteration");
	d = Cristal();
	Set(d, "scale", Vector2{1e308, 1e308});
	Refuse(d, Status::UnsupportedExecution, "iteration");
}
TEST_CASE("Cristal zero raw Dimension and Level In divisors refuse atomically", "[source_cristal_noise]") {
	Refuse(Cristal({0, 1}), Status::UnsupportedExecution, "dimension");
	auto d = Cristal();
	Set(d, "level_in", Vector2{1, 1});
	Refuse(d, Status::UnsupportedExecution, "level_in");
}
TEST_CASE("Cristal requires a saved authored seed", "[source_cristal_noise]") {
	auto d = Cristal();
	std::erase_if(d.Nodes[0].Values, [](const auto &v) { return v.Port == "seed"; });
	Refuse(d, Status::UnsupportedExecution, "seed");
}
TEST_CASE(
	"Cristal seven explicit depths and inherited project depth remain typed", "[source_cristal_noise]"
) {
	for (int64_t depth = 2; depth <= 8; ++depth) {
		auto d = Cristal();
		Set(d, "attribute_color_depth", EnumValue{depth});
		const auto image = Draw(d);
		const auto format = DescribeSurfaceFormat(image.Format);
		REQUIRE(format);
		CHECK(image.Pixels.size() == 12 * format->BytesPerPixel);
		CHECK(image.Hash == SurfaceHash(image));
	}
	auto d = Cristal();
	Set(d, "attribute_color_depth", EnumValue{1});
	d.Project = ProjectSettings{.ColorDepth = 3};
	CHECK(Draw(d).Format == SurfaceFormat::RGBA32Float);
}
TEST_CASE("Cristal raw Project dimensions retain half-even pixel allocation", "[source_cristal_noise]") {
	auto d = Cristal({2.5, 3.5});
	const auto expected = Draw(d);
	CHECK(expected.Width == 2);
	CHECK(expected.Height == 4);
	d.Project = ProjectSettings{.SurfaceWidth = 2, .SurfaceHeight = 2};
	Set(d, "dimension", Vector2{1.25, 1.75});
	Set(d, "dimension_unit", EnumValue{1});
	Same(Draw(d), expected);
}
TEST_CASE("Cristal Mask Dimension units use actual linked dimensions", "[source_cristal_noise]") {
	auto d = Cristal({.5, .5});
	Set(d, "dimension_unit", EnumValue{2});
	d.Nodes.push_back(Solid("mask", {8, 6}, {255, 255, 255, 255}));
	d.Links = {{"mask", "surface_out", "generator", "mask"}};
	Same(Draw(d), Draw(Cristal()));
}
TEST_CASE("Cristal UV alpha survives zero Mix with complete hash identity", "[source_cristal_noise]") {
	auto d = Cristal();
	auto expected = Draw(d);
	for (size_t i = 3; i < expected.Pixels.size(); i += 4)
		expected.Pixels[i] = 128;
	expected.Hash = SurfaceHash(expected);
	d.Nodes.push_back(Solid("uv", {1, 1}, {255, 0, 0, 128}));
	d.Links = {{"uv", "surface_out", "generator", "uv_map"}};
	Set(d, "uv_mix", 0.);
	Same(Draw(d), expected);
}
TEST_CASE("Cristal full UV replacement retains source green inversion", "[source_cristal_noise]") {
	auto d = Cristal();
	d.Nodes.push_back(Solid("uv", {1, 1}, {255, 0, 0, 128}));
	d.Links = {{"uv", "surface_out", "generator", "uv_map"}};
	const auto image = Draw(d);
	REQUIRE(image.Width == 4);
	REQUIRE(image.Height == 3);
	for (size_t i = 0; i < image.Pixels.size(); i += 4) {
		CHECK(image.Pixels[i] == 22);
		CHECK(image.Pixels[i + 1] == 22);
		CHECK(image.Pixels[i + 2] == 22);
		CHECK(image.Pixels[i + 3] == 128);
	}
	CHECK(image.Hash == SurfaceHash(image));
}

TEST_CASE("Cristal mask uses the source RGBA8 intermediate and inert Alpha Only", "[source_cristal_noise]") {
	auto d = Cristal({1, 1});
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
TEST_CASE("Cristal linked numeric Position receives Reference units", "[source_cristal_noise]") {
	auto d = Cristal();
	Set(d, "position", Vector2{.25, .5});
	const auto expected = Draw(d);
	d.Nodes.push_back({"position", "pc.vector2", "", {}, {{"x", .25}, {"y", .5}}});
	d.Links = {{"position", "vector", "generator", "position"}};
	Same(Draw(d), expected);
}
TEST_CASE("Cristal scalar surface tuple getters bypass numeric Position units", "[source_cristal_noise]") {
	for (const char *port : {"position", "scale", "level_in", "level_out"}) {
		auto d = Cristal();
		d.Nodes.push_back(Solid("tuple", {2, 1}, {0, 0, 0, 0}));
		d.Links = {{"tuple", "surface_out", "generator", port}};
		auto expected = Cristal();
		Set(expected, port, Vector2{2, 1});
		if (std::string_view(port) == "position") Set(expected, "position_unit", EnumValue{0});
		Same(Draw(d), Draw(expected));
	}
}
TEST_CASE("Cristal whole-surface tuple arrays preserve nonsurface fallback", "[source_cristal_noise]") {
	for (const char *port : {"position", "scale", "level_out"}) {
		auto d = Cristal();
		SurfaceRows(d, port);
		auto expected = Cristal();
		Set(expected, port, Vector2{1, 1});
		if (std::string_view(port) == "position") Set(expected, "position_unit", EnumValue{0});
		Same(Draw(d), Draw(expected));
	}
	auto d = Cristal();
	SurfaceRows(d, "level_in");
	Refuse(d, Status::UnsupportedExecution, "level_in");
}
TEST_CASE(
	"Cristal Int and Slider surface getters preserve scalar dimensions as rows", "[source_cristal_noise]"
) {
	for (const char *port : {"iteration", "gamma", "uv_mix"}) {
		auto d = Cristal();
		d.Nodes.push_back(Solid("control", {2, 1}, {0, 0, 0, 0}));
		d.Links = {{"control", "surface_out", "generator", port}};
		auto rows = Batch(d);
		REQUIRE(rows.Images.size() == 2);
		for (size_t i = 0; i < 2; ++i) {
			auto expected = Cristal();
			Set(expected,
				port,
				std::string_view(port) == "iteration" ? Value{int64_t(i == 0 ? 2 : 1)}
													  : Value{i == 0 ? 2. : 1.});
			Same(rows.Images[i], Draw(expected));
		}
	}
}
TEST_CASE(
	"Cristal Dimension surface rows preserve first-canvas Reference Position", "[source_cristal_noise]"
) {
	auto d = Cristal();
	Set(d, "position", Vector2{.25, .5});
	SurfaceRows(d, "dimension");
	auto rows = Batch(d);
	REQUIRE(rows.Images.size() == 2);
	for (size_t i = 0; i < 2; ++i) {
		auto expected = Cristal(i == 0 ? Vector2{2, 1} : Vector2{3, 2});
		Set(expected, "position", Vector2{.5, .5});
		Set(expected, "position_unit", EnumValue{0});
		Same(rows.Images[i], Draw(expected));
	}
}
TEST_CASE("Cristal source scalar SliRange links duplicate components", "[source_cristal_noise]") {
	auto d = Cristal();
	d.Nodes.push_back({"level", "pc.number_simple", "", {}, {{"value", .3}}});
	d.Links = {{"level", "number", "generator", "level_out"}};
	auto expected = Cristal();
	Set(expected, "level_out", Vector2{.3, .3});
	Same(Draw(d), Draw(expected));
}
TEST_CASE("Cristal typed Color producer preserves scalar source getter", "[source_cristal_noise]") {
	auto d = Cristal();
	Set(d, "color", Colour{255, 128, 0, 17});
	const auto expected = Draw(d);
	d.Nodes.push_back({"color", "pc.color", "", {}, {{"color", Colour{255, 128, 0, 17}}}});
	d.Links = {{"color", "color", "generator", "color"}};
	Same(Draw(d), expected);
}
TEST_CASE("Cristal all array schedules retain source full-slot inverse repeats", "[source_cristal_noise]") {
	for (int64_t mode = 0; mode < 4; ++mode) {
		auto d = Cristal();
		Set(d, "attribute_array_process", EnumValue{mode});
		Set(d, "position", Vector2{.25, .5});
		d.Nodes.push_back(Array("size", ValueType::Vector2, Vector2{4, 3}, Vector2{8, 3}));
		d.Nodes.push_back(Array("count", ValueType::Integer, int64_t{1}, int64_t{3}));
		d.Links = {{"size", "array", "generator", "dimension"}, {"count", "array", "generator", "iteration"}};
		auto rows = Batch(d);
		REQUIRE(rows.Images.size() == (mode < 2 ? 2 : 4));
		for (size_t i = 0; i < rows.Images.size(); ++i) {
			const size_t sizeRow = mode == 2 ? i / 2 : i % 2, countRow = i % 2;
			CHECK(rows.Images[i].Width == (sizeRow == 0 ? 4 : 8));
			auto expected = Cristal({sizeRow == 0 ? 4. : 8., 3});
			Set(expected, "position", Vector2{1, 1.5});
			Set(expected, "position_unit", EnumValue{0});
			Set(expected, "iteration", int64_t(countRow == 0 ? 1 : 3));
			Same(rows.Images[i], Draw(expected));
		}
	}
}
TEST_CASE(
	"Cristal original later Iteration work precedes first small output admission", "[source_cristal_noise]"
) {
	auto d = Cristal();
	d.Nodes.push_back(Array("count", ValueType::Integer, int64_t{1}, int64_t{1000000}));
	d.Links = {{"count", "array", "generator", "iteration"}};
	Refuse(d, Status::LimitExceeded, "iteration", 100000, "work");
}
TEST_CASE("Cristal original later canvas work precedes first output", "[source_cristal_noise]") {
	auto d = Cristal();
	d.Nodes.push_back(Array("sizes", ValueType::Vector2, Vector2{1, 1}, Vector2{3000, 3000}));
	d.Links = {{"sizes", "array", "generator", "dimension"}};
	Refuse(d, Status::LimitExceeded, "iteration", 100000, "work");
}
TEST_CASE(
	"Cristal widest batch byte quote and public output refusal remain atomic", "[source_cristal_noise]"
) {
	auto d = Cristal();
	Set(d, "iteration", int64_t{1});
	d.Nodes.push_back(Array("sizes", ValueType::Vector2, Vector2{1, 1}, Vector2{32, 32}));
	d.Links = {{"sizes", "array", "generator", "dimension"}};
	Refuse(d, Status::LimitExceeded, "surface_out", 30000);
	Refuse(Cristal(), Status::LimitExceeded, "", 1);
}
TEST_CASE("Cristal native persistence and source instance regenerate full images", "[source_cristal_noise]") {
	auto d = Cristal();
	Set(d, "phase", 37.);
	Set(d, "color", Colour{255, 128, 0, 17});
	const auto expected = Draw(d);
	Document restored;
	Diagnostic diag;
	REQUIRE(Read(Write(d), restored, diag) == Status::Ok);
	Same(Draw(restored), expected);
	Node copy{"copy", "pc.noise_cristal", "", {}, {}};
	copy.InstanceBase = "generator";
	restored.Nodes.push_back(copy);
	restored.Outputs[0].NodeId = "copy";
	Same(Draw(restored), expected);
}
TEST_CASE("Cristal inherited keyframed Phase preserves source tick inputs", "[source_cristal_noise]") {
	auto d = Cristal();
	d.Keyframes = {{"generator", "phase", 0, 0.}, {"generator", "phase", 2, 37.}};
	d.Nodes[0].SourceAnimatedInputs = {"phase"};
	Node copy{"copy", "pc.noise_cristal", "", {}, {}};
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
	auto expected = Cristal();
	Set(expected, "phase", 37.);
	Same(actual, Draw(expected));
	request.Tick = 0;
	REQUIRE(Evaluate(d, p, "out", request, actual, diag) == Status::Ok);
	Same(actual, Draw(Cristal()));
}

TEST_CASE("Cristal linked Int getter rounds half even before loop and admission", "[source_cristal_noise]") {
	for (double count : {1.5, 2.5, 3.5}) {
		auto d = Cristal();
		d.Nodes.push_back({"count", "pc.number_simple", "", {}, {{"value", count}}});
		d.Links = {{"count", "number", "generator", "iteration"}};
		auto expected = Cristal();
		Set(expected, "iteration", int64_t(count == 3.5 ? 4 : 2));
		Same(Draw(d), Draw(expected));
	}
}
TEST_CASE(
	"Cristal Color array rows retain alpha-inert RGB and native serialization", "[source_cristal_noise]"
) {
	auto d = Cristal();
	d.Nodes.push_back(Array("colors", ValueType::Colour, Colour{255, 128, 0, 17}, Colour{0, 64, 255, 128}));
	d.Links = {{"colors", "array", "generator", "color"}};
	auto rows = Batch(d);
	REQUIRE(rows.Images.size() == 2);
	for (size_t i = 0; i < 2; ++i) {
		auto expected = Cristal();
		Set(expected, "color", i == 0 ? Colour{255, 128, 0, 17} : Colour{0, 64, 255, 128});
		Same(rows.Images[i], Draw(expected));
	}
	Document restored;
	Diagnostic diag;
	REQUIRE(Read(Write(d), restored, diag) == Status::Ok);
	auto replay = Batch(restored);
	CHECK(replay.Images == rows.Images);
	REQUIRE(replay.Items.size() == rows.Items.size());
	for (size_t i = 0; i < rows.Items.size(); ++i)
		CHECK(std::get<size_t>(replay.Items[i].Data) == std::get<size_t>(rows.Items[i].Data));
}
TEST_CASE(
	"Cristal source Vec2 and SliRange tuples clip excess numeric components and pad zeros",
	"[source_cristal_noise]"
) {
	for (const char *port : {"position", "scale", "level_out"})
		for (bool shortTuple : {false, true}) {
			auto d = Cristal();
			auto list = Array("tuple", ValueType::Scalar, .25, .5);
			if (shortTuple)
				list.DynamicInputs.pop_back();
			else
				list.DynamicInputs.push_back({"input_2", ValueType::Scalar, Value{1234.}});
			d.Nodes.push_back(std::move(list));
			d.Links = {{"tuple", "array", "generator", port}};
			auto expected = Cristal();
			Set(expected, port, Vector2{.25, shortTuple ? 0. : .5});
			Same(Draw(d), Draw(expected));
		}
}
TEST_CASE(
	"Cristal integral numeric Color links use packed little-channel source RGB", "[source_cristal_noise]"
) {
	for (double packed : {double(0x110080ff), -1.}) {
		auto d = Cristal();
		d.Nodes.push_back({"color", "pc.number_simple", "", {}, {{"value", packed}}});
		d.Links = {{"color", "number", "generator", "color"}};
		auto expected = Cristal();
		Set(expected, "color", packed < 0 ? Colour{255, 255, 255, 255} : Colour{255, 128, 0, 17});
		Same(Draw(d), Draw(expected));
	}
	auto d = Cristal();
	d.Nodes.push_back({"color", "pc.number_simple", "", {}, {{"value", 1.5}}});
	d.Links = {{"color", "number", "generator", "color"}};
	Refuse(d, Status::UnsupportedExecution, "color");
}
