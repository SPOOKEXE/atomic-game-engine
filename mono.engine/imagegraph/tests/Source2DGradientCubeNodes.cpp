#include "ComplexGeneratorFixture.hpp"

#include <engine/imagegraph/StatefulReplay.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_approx.hpp>
TEST_SUITE_ID("engine.imagegraph.source_gradient_cube")
using namespace complex_generator_test;
namespace {
	Document Cube(Vector2 size = {4, 4}, std::string output = "surface_out") {
		Document d;
		d.FormatVersion = 9;
		d.Nodes = {
			{"generator",
			 "pc.gradient_cube",
			 "",
			 {},
			 {{"dimension", size}, {"dimension_unit", EnumValue{0}}, {"attribute_color_depth", EnumValue{3}}}}
		};
		d.Outputs = {{"out", "generator", std::move(output)}};
		return d;
	}
	void Same(const Image &a, const Image &b) {
		CHECK(a.Width == b.Width);
		CHECK(a.Height == b.Height);
		CHECK(a.Format == b.Format);
		CHECK(a.Pixels == b.Pixels);
		CHECK(a.Hash == SurfaceHash(a));
		CHECK(a == b);
	}
	ArrayValue Corners() {
		ArrayValue a;
		a.ElementType = ValueType::Colour;
		for (int i = 0; i < 8; ++i)
			a.Elements.emplace_back(
				Colour{uint8_t(i & 4 ? 255 : 0), uint8_t(i & 2 ? 255 : 0), uint8_t(i & 1 ? 255 : 0), 255}
			);
		return a;
	}
	ImageArray Batch(const Document &d) {
		Plan p;
		Diagnostic diag;
		REQUIRE(Compile(d, p, diag) == Status::Ok);
		ImageArray images;
		auto status = EvaluateArray(d, p, "out", {}, images, diag);
		INFO(diag.Message);
		REQUIRE(status == Status::Ok);
		return images;
	}
	void Pixels(const Image &image, std::initializer_list<int> expected) {
		REQUIRE(image.Pixels.size() == expected.size());
		size_t i = 0;
		for (int p : expected)
			CHECK(image.Pixels[i++] == p);
		CHECK(image.Hash == SurfaceHash(image));
	}
	void Uniform(const Image &image, Colour color) {
		for (uint32_t y = 0; y < image.Height; ++y)
			for (uint32_t x = 0; x < image.Width; ++x) {
				SurfacePixel p;
				REQUIRE(LoadSurfacePixel(image, x, y, p));
				CHECK(p[0] == Catch::Approx(color.Red / 255.).margin(1e-6));
				CHECK(p[1] == Catch::Approx(color.Green / 255.).margin(1e-6));
				CHECK(p[2] == Catch::Approx(color.Blue / 255.).margin(1e-6));
				CHECK(p[3] == Catch::Approx(color.Alpha / 255.).margin(1e-6));
			}
	}
}
TEST_CASE("Gradient Cube source-equation golden default_cube", "[source_gradient_cube]") {
	auto d = Cube();
	Pixels(Draw(d), {0, 0, 0, 0, 0,	  0,   0,	0,	 0,	  0,   0,	0,	 0, 0, 0, 0,
					 0, 0, 0, 0, 239, 239, 239, 255, 148, 148, 148, 255, 0, 0, 0, 0,
					 0, 0, 0, 0, 255, 255, 255, 255, 165, 165, 165, 255, 0, 0, 0, 0,
					 0, 0, 0, 0, 0,	  0,   0,	0,	 0,	  0,   0,	0,	 0, 0, 0, 0});
}
TEST_CASE("Gradient Cube source-equation golden default_sphere", "[source_gradient_cube]") {
	auto d = Cube();
	Set(d, "shape", EnumValue{1});
	Pixels(Draw(d), {0, 0, 0, 0, 0,	  0,   0,	0,	 0,	  0,   0,	0,	 0, 0, 0, 0,
					 0, 0, 0, 0, 205, 205, 205, 255, 115, 115, 115, 255, 0, 0, 0, 0,
					 0, 0, 0, 0, 250, 250, 250, 255, 160, 160, 160, 255, 0, 0, 0, 0,
					 0, 0, 0, 0, 0,	  0,   0,	0,	 0,	  0,   0,	0,	 0, 0, 0, 0});
}
TEST_CASE("Gradient Cube source-equation golden zero_camera", "[source_gradient_cube]") {
	auto d = Cube();
	Set(d, "rotation", Vector3{});
	Pixels(Draw(d), {0, 0, 0, 0, 0,	  0,   0,	0,	 0,	  0,   0,	0,	 0, 0, 0, 0,
					 0, 0, 0, 0, 255, 255, 255, 255, 255, 255, 255, 255, 0, 0, 0, 0,
					 0, 0, 0, 0, 255, 255, 255, 255, 255, 255, 255, 255, 0, 0, 0, 0,
					 0, 0, 0, 0, 0,	  0,   0,	0,	 0,	  0,   0,	0,	 0, 0, 0, 0});
}
TEST_CASE("Gradient Cube source-equation golden axis_y", "[source_gradient_cube]") {
	auto d = Cube();
	Set(d, "axis", EnumValue{1});
	Pixels(Draw(d), {0, 0, 0, 0, 0,	  0,   0,	0,	 0,	  0,   0,	0,	 0, 0, 0, 0,
					 0, 0, 0, 0, 148, 148, 148, 255, 239, 239, 239, 255, 0, 0, 0, 0,
					 0, 0, 0, 0, 165, 165, 165, 255, 255, 255, 255, 255, 0, 0, 0, 0,
					 0, 0, 0, 0, 0,	  0,   0,	0,	 0,	  0,   0,	0,	 0, 0, 0, 0});
}
TEST_CASE("Gradient Cube source-equation golden wide", "[source_gradient_cube]") {
	auto d = Cube({6, 3});
	Pixels(Draw(d), {0,	  0,   0,	0,	 0,	  0,   0,	0,	 73,  73,  73,	255, 13,  13,  13,	255, 0,	  0,
					 0,	  0,   0,	0,	 0,	  0,   0,	0,	 0,	  0,   255, 255, 255, 255, 255, 255, 255, 255,
					 195, 195, 195, 255, 75,  75,  75,	255, 0,	  0,   0,	0,	 0,	  0,   0,	0,	 0,	  0,
					 0,	  0,   255, 255, 255, 255, 195, 195, 195, 255, 0,	0,	 0,	  0,   0,	0,	 0,	  0});
}
TEST_CASE("Gradient Cube cross planes interpolate all eight corners", "[source_gradient_cube]") {
	for (int64_t axis = 0; axis < 3; ++axis) {
		auto d = Cube({4, 2}, "cross_section");
		Set(d, "colors", Corners());
		Set(d, "axis_2", EnumValue{axis});
		Set(d, "position", .5);
		auto image = Draw(d);
		for (uint32_t y = 0; y < 2; ++y)
			for (uint32_t x = 0; x < 4; ++x) {
				const uint8_t u[] = {32, 96, 159, 223}, v[] = {64, 191};
				const std::array<uint8_t, 3> expected = axis == 0	? std::array<uint8_t, 3>{128, u[x], v[y]}
														: axis == 1 ? std::array<uint8_t, 3>{u[x], 128, v[y]}
																	: std::array<uint8_t, 3>{u[x], v[y], 128};
				for (size_t k = 0; k < 3; ++k)
					CHECK(image.Pixels[(y * 4 + x) * 4 + k] == expected[k]);
				CHECK(image.Pixels[(y * 4 + x) * 4 + 3] == 255);
			}
	}
}
TEST_CASE(
	"Gradient Cube object transform keeps float extrapolation and source rotation sign",
	"[source_gradient_cube]"
) {
	auto d = Cube({2, 2}, "cross_section");
	Set(d, "colors", Corners());
	Set(d, "axis_2", EnumValue{2});
	Set(d, "position", .5);
	Set(d, "rotation_2", Vector3{0, 0, 90});
	Set(d, "scale_2", Vector3{2, 3, 4});
	Set(d, "attribute_color_depth", EnumValue{5});
	auto image = Draw(d);
	for (uint32_t y = 0; y < 2; ++y)
		for (uint32_t x = 0; x < 2; ++x) {
			SurfacePixel p;
			REQUIRE(LoadSurfacePixel(image, x, y, p));
			CHECK(p[0] == Catch::Approx(2 * (y + .5) / 2).margin(1e-6));
			CHECK(p[1] == Catch::Approx(-3 * (x + .5) / 2).margin(1e-6));
			CHECK(p[2] == Catch::Approx(2.).margin(1e-6));
			CHECK(p[3] == Catch::Approx(1.).margin(1e-6));
		}
}
TEST_CASE("Gradient Cube camera and shape do not change cross section", "[source_gradient_cube]") {
	auto d = Cube({4, 4}, "cross_section");
	Set(d, "colors", Corners());
	Set(d, "position", .25);
	const auto expected = Draw(d);
	Set(d, "rotation", Vector3{77, 32, 12});
	Set(d, "scale", -2.);
	Set(d, "shape", EnumValue{1});
	Set(d, "axis", EnumValue{2});
	Same(Draw(d), expected);
}
TEST_CASE(
	"Gradient Cube object scale changes colors without changing hit coverage", "[source_gradient_cube]"
) {
	auto d = Cube();
	Set(d, "colors", Corners());
	const auto a = Draw(d);
	Set(d, "scale_2", Vector3{2, 3, -1});
	auto b = Draw(d);
	REQUIRE(a.Pixels.size() == b.Pixels.size());
	for (size_t i = 3; i < a.Pixels.size(); i += 4)
		CHECK(a.Pixels[i] == b.Pixels[i]);
	CHECK(a.Pixels != b.Pixels);
}
TEST_CASE("Gradient Cube main misses retain palette RGB and zero alpha", "[source_gradient_cube]") {
	auto d = Cube();
	Set(d, "colors", ArrayValue{ValueType::Colour, {Colour{128, 64, 32, 17}}});
	auto image = Draw(d);
	REQUIRE(image.Pixels.size() == 64);
	for (size_t i = 0; i < 64; i += 4) {
		CHECK(image.Pixels[i] == 128);
		CHECK(image.Pixels[i + 1] == 64);
		CHECK(image.Pixels[i + 2] == 32);
		CHECK(image.Pixels[i + 3] == (i == 20 || i == 24 || i == 36 || i == 40 ? 255 : 0));
	}
	d.Outputs[0].Port = "cross_section";
	Uniform(Draw(d), {128, 64, 32, 17});
}
TEST_CASE(
	"Gradient Cube numeric real palette defaults alpha while typed integer keeps packed alpha",
	"[source_gradient_cube]"
) {
	auto d = Cube({2, 2}, "cross_section");
	Set(d, "colors", ArrayValue{ValueType::Scalar, {double(0x11204080)}});
	Uniform(Draw(d), {128, 64, 32, 255});
	Set(d, "colors", ArrayValue{ValueType::Integer, {int64_t{0x11204080}}});
	Uniform(Draw(d), {128, 64, 32, 17});
}
TEST_CASE("Gradient Cube palettes wrap their first eight corner indices", "[source_gradient_cube]") {
	auto d = Cube({2, 2}, "cross_section");
	Set(d, "axis_2", EnumValue{2});
	Set(d, "position", .5);
	Set(d,
		"colors",
		ArrayValue{ValueType::Colour, {Colour{0, 0, 0, 0}, Colour{255, 255, 255, 255}, Colour{0, 0, 0, 0}}});
	const auto image = Draw(d);
	// At each cell, a three-color cyclic palette is the literal trilinear weighted sum.
	Pixels(image, {104, 104, 104, 104, 120, 120, 120, 120, 56, 56, 56, 56, 104, 104, 104, 104});
}
TEST_CASE("Gradient Cube default cross palette follows V instead of camera", "[source_gradient_cube]") {
	auto d = Cube({4, 2}, "cross_section");
	Rows(Draw(d), {{64, 64, 64, 64}, {191, 191, 191, 191}});
}
TEST_CASE("Gradient Cube scalar choices clamp before shader selection", "[source_gradient_cube]") {
	auto d = Cube();
	Set(d, "shape", EnumValue{99});
	auto expected = Cube();
	Set(expected, "shape", EnumValue{1});
	Same(Draw(d), Draw(expected));
	d = Cube({2, 2}, "cross_section");
	Set(d, "axis_2", EnumValue{99});
	expected = Cube({2, 2}, "cross_section");
	Set(expected, "axis_2", EnumValue{2});
	Same(Draw(d), Draw(expected));
}
TEST_CASE(
	"Gradient Cube selected out of range source choices keep initialized branches", "[source_gradient_cube]"
) {
	auto d = Cube({2, 2}, "cross_section");
	d.Nodes.push_back(Array("axes", ValueType::Scalar, 99., -1.));
	d.Links = {{"axes", "array", "generator", "axis_2"}};
	auto images = Batch(d);
	REQUIRE(images.Images.size() == 2);
	for (auto &image : images.Images)
		Uniform(image, {0, 0, 0, 255});
	d = Cube({2, 2});
	d.Nodes.push_back(Array("shapes", ValueType::Scalar, 99., -1.));
	d.Links = {{"shapes", "array", "generator", "shape"}};
	images = Batch(d);
	REQUIRE(images.Images.size() == 2);
	for (auto &image : images.Images)
		for (size_t i = 3; i < image.Pixels.size(); i += 4)
			CHECK(image.Pixels[i] == 255);
}
TEST_CASE("Gradient Cube scalar numeric Vec3 link replicates all components", "[source_gradient_cube]") {
	auto d = Cube({2, 2}, "cross_section");
	d.Nodes.push_back({"value", "pc.number_simple", "", {}, {{"value", .25}}});
	d.Links = {{"value", "number", "generator", "scale_2"}};
	auto expected = Cube({2, 2}, "cross_section");
	Set(expected, "scale_2", Vector3{.25, .25, .25});
	Same(Draw(d), Draw(expected));
}
TEST_CASE(
	"Gradient Cube numeric tuple truncation and padding use three source components", "[source_gradient_cube]"
) {
	struct Case {
		const char *Port;
		ArrayValue Raw;
		Vector3 Expected;
	};
	const std::array cases{
		Case{"rotation", {ValueType::Scalar, {12., 23.}}, {12, 23, 0}},
		Case{"rotation", {ValueType::Scalar, {12., 23., 34., 99.}}, {12, 23, 34}},
		Case{"rotation_2", {ValueType::Integer, {int64_t{12}, int64_t{23}}}, {12, 23, 0}},
		Case{"scale_2", {ValueType::Integer, {int64_t{1}, int64_t{2}, int64_t{3}, int64_t{99}}}, {1, 2, 3}}
	};
	for (const auto &test : cases) {
		auto d = Cube({3, 2}, std::string_view(test.Port) == "rotation" ? "surface_out" : "cross_section");
		Set(d, "colors", Corners());
		Set(d, test.Port, test.Raw);
		auto expected = d;
		Set(expected, test.Port, test.Expected);
		Same(Draw(d), Draw(expected));
	}
}
TEST_CASE(
	"Gradient Cube scalar surface Vec3 controls resize dimensions before processing", "[source_gradient_cube]"
) {
	for (const char *port : {"rotation", "rotation_2", "scale_2"}) {
		auto d = Cube({2, 2}, "cross_section");
		d.Nodes.push_back(Solid("tuple", {2, 1}, {0, 0, 0, 0}));
		d.Links = {{"tuple", "surface_out", "generator", port}};
		auto expected = Cube({2, 2}, "cross_section");
		Set(expected, port, Vector3{2, 1, 0});
		Same(Draw(d), Draw(expected));
	}
}
TEST_CASE(
	"Gradient Cube whole surface array Vec3 uses source nonsurface fallback", "[source_gradient_cube]"
) {
	for (const char *port : {"rotation", "rotation_2", "scale_2"}) {
		auto d = Cube({2, 2}, "cross_section");
		SurfaceRows(d, port);
		auto expected = Cube({2, 2}, "cross_section");
		Set(expected, port, Vector3{1, 1, 0});
		Same(Draw(d), Draw(expected));
	}
}
TEST_CASE(
	"Gradient Cube Float and Slider surface getters expand width then height", "[source_gradient_cube]"
) {
	for (const char *port : {"scale", "position"}) {
		auto d = Cube({2, 2}, "cross_section");
		d.Nodes.push_back(Solid("tuple", {2, 1}, {0, 0, 0, 0}));
		d.Links = {{"tuple", "surface_out", "generator", port}};
		auto images = Batch(d);
		REQUIRE(images.Images.size() == 2);
		for (size_t i = 0; i < 2; ++i) {
			auto expected = Cube({2, 2}, "cross_section");
			Set(expected, port, i == 0 ? 2. : 1.);
			Same(images.Images[i], Draw(expected));
		}
	}
}
TEST_CASE("Gradient Cube linked surface canvas bypasses Project units", "[source_gradient_cube]") {
	auto d = Cube({1, 1}, "cross_section");
	Set(d, "dimension_unit", EnumValue{1});
	d.Nodes.push_back(Solid("size", {3, 2}, {0, 0, 0, 0}));
	d.Links = {{"size", "surface_out", "generator", "dimension"}};
	Same(Draw(d), Draw(Cube({3, 2}, "cross_section")));
}
TEST_CASE("Gradient Cube surface canvas rows are projected before selection", "[source_gradient_cube]") {
	auto d = Cube({1, 1}, "cross_section");
	SurfaceRows(d, "dimension");
	auto images = Batch(d);
	REQUIRE(images.Images.size() == 2);
	Same(images.Images[0], Draw(Cube({2, 1}, "cross_section")));
	Same(images.Images[1], Draw(Cube({3, 2}, "cross_section")));
}
TEST_CASE(
	"Gradient Cube both output depths include inherited and seven explicit formats", "[source_gradient_cube]"
) {
	for (const char *port : {"surface_out", "cross_section"})
		for (int64_t depth = 2; depth <= 8; ++depth) {
			auto d = Cube({2, 2}, port);
			Set(d, "attribute_color_depth", EnumValue{depth});
			auto image = Draw(d);
			auto format = DescribeSurfaceFormat(image.Format);
			REQUIRE(format);
			CHECK(image.Pixels.size() == 4 * format->BytesPerPixel);
			CHECK(image.Hash == SurfaceHash(image));
		}
	auto d = Cube({2, 2}, "cross_section");
	Set(d, "attribute_color_depth", EnumValue{1});
	d.Project = ProjectSettings{.ColorDepth = 3};
	CHECK(Draw(d).Format == SurfaceFormat::RGBA32Float);
}
TEST_CASE("Gradient Cube Project and Pixel dimension allocation round half even", "[source_gradient_cube]") {
	auto d = Cube({2.5, 3.5}, "cross_section");
	auto expected = Draw(d);
	CHECK(expected.Width == 2);
	CHECK(expected.Height == 4);
	Set(d, "dimension", Vector2{1.25, 1.75});
	Set(d, "dimension_unit", EnumValue{1});
	d.Project = ProjectSettings{.SurfaceWidth = 2, .SurfaceHeight = 2};
	Same(Draw(d), expected);
}
TEST_CASE(
	"Gradient Cube nonpositive raw quad refuses without inventing retained target", "[source_gradient_cube]"
) {
	Refuse(Cube({0, 1}), Status::UnsupportedExecution, "dimension");
	Refuse(Cube({-1, -1}), Status::UnsupportedExecution, "dimension");
}
TEST_CASE(
	"Gradient Cube all four schedules keep both selected output row identities", "[source_gradient_cube]"
) {
	for (const char *port : {"surface_out", "cross_section"})
		for (int64_t mode = 0; mode < 4; ++mode) {
			auto d = Cube({4, 4}, port);
			Set(d, "attribute_array_process", EnumValue{mode});
			d.Nodes.push_back(Array("sizes", ValueType::Vector2, Vector2{2, 2}, Vector2{4, 2}));
			d.Nodes.push_back(Array("scales", ValueType::Scalar, .5, 1.));
			d.Links = {
				{"sizes", "array", "generator", "dimension"}, {"scales", "array", "generator", "scale"}
			};
			auto images = Batch(d);
			REQUIRE(images.Images.size() == (mode < 2 ? 2 : 4));
			for (size_t i = 0; i < images.Images.size(); ++i) {
				size_t sizeRow = mode == 2 ? i / 2 : i % 2, scaleRow = i % 2;
				auto expected = Cube(sizeRow == 0 ? Vector2{2, 2} : Vector2{4, 2}, port);
				Set(expected, "scale", scaleRow == 0 ? .5 : 1.);
				Same(images.Images[i], Draw(expected));
			}
		}
}
TEST_CASE("Gradient Cube later expensive canvas is admitted before first target", "[source_gradient_cube]") {
	auto d = Cube();
	d.Nodes.push_back(Array("sizes", ValueType::Vector2, Vector2{1, 1}, Vector2{50, 50}));
	d.Links = {{"sizes", "array", "generator", "dimension"}};
	Refuse(d, Status::LimitExceeded, "dimension", 100000, "work");
}
TEST_CASE(
	"Gradient Cube two widest targets and row metadata are quoted before first output",
	"[source_gradient_cube]"
) {
	auto d = Cube();
	d.Nodes.push_back(Array("sizes", ValueType::Vector2, Vector2{1, 1}, Vector2{20, 20}));
	d.Links = {{"sizes", "array", "generator", "dimension"}};
	Refuse(d, Status::LimitExceeded, "surface_out", 23000);
	Refuse(Cube(), Status::LimitExceeded, "", 1);
}
TEST_CASE(
	"Gradient Cube selected image results retain metadata and reject value-only extraction",
	"[source_gradient_cube]"
) {
	for (const char *port : {"surface_out", "cross_section"}) {
		auto d = Cube({2, 2}, port);
		Plan p;
		Diagnostic diag;
		REQUIRE(Compile(d, p, diag) == Status::Ok);
		StatefulEvaluationResult result;
		const auto status = EvaluateStateful(d, p, "out", {}, result, diag);
		INFO(diag.Message);
		REQUIRE(status == Status::Ok);
		const auto *image = std::get_if<Image>(&result.Output);
		REQUIRE(image);
		Same(*image, Draw(d));
		EvaluatedValue value{"prior", 42., std::nullopt};
		const auto prior = value;
		REQUIRE(EvaluateValue(d, p, "out", {}, value, diag) == Status::InvalidOutput);
		CHECK(diag.NodeId == "generator");
		CHECK(diag.Port == port);
		CHECK(diag.Message == "selected output is an image");
		CHECK(value == prior);
		Same(*image, Draw(d));
	}
}
TEST_CASE(
	"Gradient Cube native persistence and inherited instance regenerate both outputs",
	"[source_gradient_cube]"
) {
	for (const char *port : {"surface_out", "cross_section"}) {
		auto d = Cube({3, 2}, port);
		Set(d, "rotation_2", Vector3{12, 23, 34});
		Set(d, "colors", Corners());
		const auto expected = Draw(d);
		Document decoded;
		Diagnostic diag;
		REQUIRE(Read(Write(d), decoded, diag) == Status::Ok);
		Same(Draw(decoded), expected);
		Node instance{"instance", "pc.gradient_cube", "", {}, {}};
		instance.InstanceBase = "generator";
		decoded.Nodes.push_back(instance);
		decoded.Outputs[0].NodeId = "instance";
		Same(Draw(decoded), expected);
	}
}
TEST_CASE(
	"Gradient Cube inherited animation resolves actual camera rotation ticks", "[source_gradient_cube]"
) {
	auto d = Cube();
	d.Keyframes = {{"generator", "rotation", 0, Vector3{}}, {"generator", "rotation", 2, Vector3{30, 45, 0}}};
	d.Nodes[0].SourceAnimatedInputs = {"rotation"};
	Node instance{"copy", "pc.gradient_cube", "", {}, {}};
	instance.InstanceBase = "generator";
	d.Nodes.push_back(instance);
	d.Outputs[0].NodeId = "copy";
	Plan p;
	Diagnostic diag;
	REQUIRE(Compile(d, p, diag) == Status::Ok);
	for (uint64_t tick : {0, 2}) {
		EvaluationRequest request;
		request.Tick = tick;
		Image out;
		REQUIRE(Evaluate(d, p, "out", request, out, diag) == Status::Ok);
		auto expected = Cube();
		if (tick == 0) Set(expected, "rotation", Vector3{});
		Same(out, Draw(expected));
	}
}
TEST_CASE(
	"Gradient Cube uploaded colors beyond corner seven do not affect shader sampling",
	"[source_gradient_cube]"
) {
	auto d = Cube({2, 2}, "cross_section");
	auto palette = Corners();
	Set(d, "colors", palette);
	auto expected = Draw(d);
	palette.Elements.emplace_back(Colour{128, 19, 81, 4});
	Set(d, "colors", palette);
	Same(Draw(d), expected);
}
TEST_CASE(
	"Gradient Cube cross output is retained as actual downstream typed surface", "[source_gradient_cube]"
) {
	auto d = Cube({2, 2}, "cross_section");
	Set(d, "colors", ArrayValue{ValueType::Colour, {Colour{128, 64, 32, 255}}});
	d.Nodes.push_back({"invert", "pc.invert", "", {}, {}});
	d.Links = {{"generator", "cross_section", "invert", "surface_in"}};
	d.Outputs[0] = {"out", "invert", "surface_out"};
	Uniform(Draw(d), {127, 191, 223, 255});
}
TEST_CASE(
	"Gradient Cube unobserved empty upload and numeric conversion fail atomically", "[source_gradient_cube]"
) {
	auto d = Cube();
	Set(d, "colors", ArrayValue{ValueType::Colour, {}});
	Refuse(d, Status::UnsupportedExecution, "colors");
	d = Cube();
	Set(d, "colors", ArrayValue{ValueType::Scalar, {.5}});
	Refuse(d, Status::UnsupportedExecution, "colors");
}
TEST_CASE(
	"Gradient Cube negative and zero camera Scale preserve literal orthographic projection",
	"[source_gradient_cube]"
) {
	auto d = Cube();
	const auto a = Draw(d);
	Set(d, "scale", -1.);
	auto b = Draw(d);
	REQUIRE(a.Pixels.size() == b.Pixels.size());
	for (size_t i = 0; i < 16; ++i)
		for (size_t channel = 0; channel < 4; ++channel)
			CHECK(b.Pixels[i * 4 + channel] == a.Pixels[(15 - i) * 4 + channel]);
	Set(d, "scale", 0.);
	b = Draw(d);
	for (size_t i = 4; i < b.Pixels.size(); ++i)
		CHECK(b.Pixels[i] == b.Pixels[i % 4]);
}
