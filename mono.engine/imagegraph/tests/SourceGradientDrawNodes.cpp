#include <engine/imagegraph/Document.hpp>
#include <engine/imagegraph/Surface.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

TEST_SUITE_ID("engine.imagegraph.source_draw_gradient")
using namespace engine::imagegraph;
namespace {
	void Set(Node &n, std::string port, Value value) {
		for (auto &p : n.Values)
			if (p.Port == port) {
				p.Data = std::move(value);
				return;
			}
		n.Values.push_back({std::move(port), std::move(value)});
	}
	Document Fixture(int64_t type = 0) {
		Document d;
		d.FormatVersion = 9;
		d.Nodes = {
			{"field",
			 "pc.gradient",
			 "",
			 {},
			 {{"dimension", Vector2{2, 2}},
			  {"dimension_unit", EnumValue{0}},
			  {"attribute_color_depth", EnumValue{3}},
			  {"type", EnumValue{type}}}}
		};
		d.Outputs = {{"image", "field", "surface_out"}};
		return d;
	}
	Curve Line(double from = 0, double to = 1) {
		Curve c;
		c.Header = {0, 1, 0, 0, 1};
		c.Anchors = {{0, 0, 0, from, 0, 0}, {0, 0, 1, to, 0, 0}};
		return c;
	}
	Plan Compiled(const Document &d) {
		Plan p;
		Diagnostic diag;
		const auto status = Compile(d, p, diag);
		INFO(diag.NodeId << " " << diag.Port << " " << diag.Message);
		REQUIRE(status == Status::Ok);
		return p;
	}
	Image Run(const Document &d, EvaluationRequest req = {}) {
		const auto p = Compiled(d);
		Diagnostic diag;
		Image image;
		const auto status = Evaluate(d, p, "image", req, image, diag);
		INFO(diag.NodeId << " " << diag.Port << " " << diag.Message);
		REQUIRE(status == Status::Ok);
		return image;
	}
	void Pixel(const Image &im, uint32_t x, uint32_t y, Colour p) {
		std::array<double, 4> actual{};
		REQUIRE(LoadSurfacePixel(im, x, y, actual));
		const std::array expected{p.Red / 255., p.Green / 255., p.Blue / 255., p.Alpha / 255.};
		for (size_t i = 0; i < 4; ++i)
			CHECK(actual[i] == Catch::Approx(expected[i]).margin(1e-6));
	}
	void Refused(
		const Document &d,
		Status wanted,
		std::string_view port,
		uint64_t bytes = Limits::MaximumOutputBytes,
		EvaluationRequest req = {}
	) {
		const auto p = Compiled(d);
		Image out{1, 1, {17, 18, 19, 20}, 0};
		const auto prior = out;
		Diagnostic diag;
		const auto status = Evaluate(d, p, "image", req, out, diag, bytes);
		INFO(diag.Message);
		CHECK(status == wanted);
		CHECK(diag.Port == port);
		CHECK(out == prior);
	}
	void Captured(Document &d, std::string port, std::string id) {
		d.Nodes.push_back({id, "image.captured", "", {}, {{"source_id", id}}});
		d.Links.push_back({id, "image", "field", std::move(port)});
	}
	EvaluationRequest Sources(std::span<const RequestImageSource> sources) {
		EvaluationRequest r;
		r.ImageSources = sources;
		return r;
	}
}
TEST_CASE("Draw Gradient default Linear shader has literal grayscale columns", "[draw_gradient]") {
	auto d = Fixture();
	auto image = Run(d);
	Pixel(image, 0, 0, {64, 64, 64, 255});
	Pixel(image, 1, 1, {191, 191, 191, 255});
	d.Nodes[0].Values.clear();
	d.Project = ProjectSettings{};
	d.Project->SurfaceWidth = 2;
	d.Project->SurfaceHeight = 2;
	CHECK(Run(d) == image);
}
TEST_CASE("Draw Gradient four source shapes have independent pixel-center goldens", "[draw_gradient]") {
	const std::array<uint8_t, 4> expected{191, 128, 32, 180};
	for (int type = 0; type < 4; ++type) {
		const auto im = Run(Fixture(type));
		const auto p = expected[size_t(type)];
		Pixel(im, 1, 1, {p, p, p, 255});
	}
}
TEST_CASE("Draw Gradient angle is source degrees with Linear perpendicular inverse axis", "[draw_gradient]") {
	auto d = Fixture();
	Set(d.Nodes[0], "angle", 90.);
	Pixel(Run(d), 1, 1, {64, 64, 64, 255});
	Set(d.Nodes[0], "angle", 0.);
	Set(d.Nodes[0], "inverse_axis", 1.);
	Set(d.Nodes[0], "inverse_curve", Line(.25, .25));
	auto im = Run(d);
	Pixel(im, 0, 0, {128, 128, 128, 255});
	Pixel(im, 1, 1, {255, 255, 255, 255});
}
TEST_CASE("Draw Gradient Circular uniform ratio and shape use raw non-square dimensions", "[draw_gradient]") {
	auto d = Fixture(1);
	Set(d.Nodes[0], "dimension", Vector2{4, 2});
	Pixel(Run(d), 3, 1, {255, 255, 255, 255});
	Set(d.Nodes[0], "uniform_ratio", false);
	Pixel(Run(d), 3, 1, {163, 163, 163, 255});
	Set(d.Nodes[0], "uniform_ratio", true);
	Set(d.Nodes[0], "shape", Vector2{2, 1});
	Pixel(Run(d), 3, 1, {163, 163, 163, 255});
}
TEST_CASE(
	"Draw Gradient shift scale and all three loops preserve source operation order", "[draw_gradient]"
) {
	auto d = Fixture();
	Set(d.Nodes[0], "shift", 1.);
	Pixel(Run(d), 0, 0, {255, 255, 255, 255});
	Set(d.Nodes[0], "loop", EnumValue{1});
	Pixel(Run(d), 0, 0, {64, 64, 64, 255});
	Set(d.Nodes[0], "loop", EnumValue{2});
	auto im = Run(d);
	Pixel(im, 0, 0, {191, 191, 191, 255});
	Pixel(im, 1, 0, {64, 64, 64, 255});
	Set(d.Nodes[0], "shift", 0.);
	Set(d.Nodes[0], "loop", EnumValue{0});
	Set(d.Nodes[0], "scale", 2.);
	im = Run(d);
	Pixel(im, 0, 0, {96, 96, 96, 255});
	Pixel(im, 1, 0, {159, 159, 159, 255});
}
TEST_CASE(
	"Draw Gradient progress curve and output brightness curve are separate shader stages", "[draw_gradient]"
) {
	auto d = Fixture();
	Curve c = Line();
	c.Anchors.insert(c.Anchors.begin() + 1, {0, 0, .5, 0, 0, 0});
	Set(d.Nodes[0], "progress_remap", c);
	auto im = Run(d);
	Pixel(im, 0, 0, {0, 0, 0, 255});
	Pixel(im, 1, 0, {128, 128, 128, 255});
	Set(d.Nodes[0], "curve", Line(.25, .25));
	im = Run(d);
	Pixel(im, 0, 0, {64, 64, 64, 255});
	Pixel(im, 1, 0, {64, 64, 64, 255});
}
TEST_CASE("Draw Gradient level input and output apply before the brightness curve", "[draw_gradient]") {
	auto d = Fixture();
	Set(d.Nodes[0], "level_in", Vector2{.25, .75});
	auto im = Run(d);
	Pixel(im, 0, 0, {0, 0, 0, 255});
	Pixel(im, 1, 0, {255, 255, 255, 255});
	Set(d.Nodes[0], "level_out", Vector2{1, 0});
	im = Run(d);
	Pixel(im, 0, 0, {255, 255, 255, 255});
	Pixel(im, 1, 0, {0, 0, 0, 255});
}
TEST_CASE(
	"Draw Gradient seven color blends follow source RGB HSV LMS gamma and CMYK formulas", "[draw_gradient]"
) {
	const std::array<Colour, 7> colors{
		Colour{128, 0, 128, 96},
		Colour{255, 0, 0, 64},
		Colour{255, 0, 255, 96},
		Colour{139, 84, 161, 96},
		Colour{186, 0, 186, 96},
		Colour{0, 255, 0, 96},
		Colour{128, 0, 128, 96}
	};
	for (int mode = 0; mode < 7; ++mode) {
		auto d = Fixture();
		Set(d.Nodes[0], "dimension", Vector2{1, 1});
		Set(d.Nodes[0], "gradient", Gradient{uint8_t(mode), {{0, {255, 0, 0, 64}}, {1, {0, 0, 255, 128}}}});
		Pixel(Run(d), 0, 0, colors[size_t(mode)]);
	}
}
TEST_CASE(
	"Draw Gradient mapped gradient samples filtered source range rather than gradient keys", "[draw_gradient]"
) {
	auto d = Fixture();
	Captured(d, "gradient_map", "map");
	Set(d.Nodes[0], "gradient_mapped", true);
	Set(d.Nodes[0], "gradient_map_range", Vector4{0, 0, 1, 0});
	std::array captures{RequestImageSource{"map", Image{2, 1, {255, 0, 0, 128, 0, 0, 255, 64}, 0}}};
	auto req = Sources(captures);
	auto im = Run(d, req);
	Pixel(im, 0, 0, {255, 0, 0, 128});
	Pixel(im, 1, 0, {0, 0, 255, 64});
	Set(d.Nodes[0], "gradient_map_range", Vector4{1, 0, 0, 0});
	im = Run(d, req);
	Pixel(im, 0, 0, {0, 0, 255, 64});
	Pixel(im, 1, 0, {255, 0, 0, 128});
}
TEST_CASE(
	"Draw Gradient four mapped scalar controls consume nearest RGB means and ignore map alpha",
	"[draw_gradient]"
) {
	for (const auto port : {"angle", "radius", "shift", "scale"}) {
		auto d = Fixture(port == std::string_view("radius") ? 1 : 0);
		Captured(d, std::string(port) + "_map", "map");
		Set(d.Nodes[0], std::string(port) + "_mapped", true);
		const Vector2 range = port == std::string_view("angle")	   ? Vector2{0, 90}
							  : port == std::string_view("radius") ? Vector2{0, .5}
							  : port == std::string_view("shift")  ? Vector2{0, .25}
																   : Vector2{0, 2};
		Set(d.Nodes[0], std::string(port) + "_map_range", range);
		std::array captures{RequestImageSource{"map", Image{1, 1, {255, 255, 255, 0}, 0}}};
		const auto im = Run(d, Sources(captures));
		const auto expected = port == std::string_view("angle")	   ? 64
							  : port == std::string_view("radius") ? 128
							  : port == std::string_view("shift")  ? 255
																   : 159;
		Pixel(im, 1, 1, {uint8_t(expected), uint8_t(expected), uint8_t(expected), 255});
	}
}
TEST_CASE(
	"Draw Gradient mapped primary endpoint pair stays one image and preserves explicit scalar repetition",
	"[draw_gradient]"
) {
	auto d = Fixture();
	Set(d.Nodes[0], "scale_mapped", true);
	Set(d.Nodes[0], "scale", 2.);
	Pixel(Run(d), 0, 0, {96, 96, 96, 255});
	ArrayValue pair;
	pair.ElementType = ValueType::Scalar;
	pair.Elements = {2., 4.};
	Set(d.Nodes[0], "scale", pair);
	const auto plan = Compiled(d);
	ImageArray images;
	Diagnostic diag;
	CHECK(EvaluateArray(d, plan, "image", {}, images, diag) == Status::InvalidOutput);
	Pixel(Run(d), 0, 0, {96, 96, 96, 255});
}
TEST_CASE(
	"Draw Gradient UV alpha and RGBA mask alpha multiply without consuming mask RGB", "[draw_gradient]"
) {
	auto d = Fixture();
	Captured(d, "mask", "mask");
	Captured(d, "uv_map", "uv");
	Set(d.Nodes[0], "uv_mix", 0.);
	std::array captures{
		RequestImageSource{"mask", Image{1, 1, {0, 0, 0, 128}, 0}},
		RequestImageSource{"uv", Image{1, 1, {255, 0, 0, 128}, 0}}
	};
	auto im = Run(d, Sources(captures));
	Pixel(im, 0, 0, {64, 64, 64, 64});
	Set(d.Nodes[0], "mask_alpha_only", true);
	CHECK(Run(d, Sources(captures)) == im);
}
TEST_CASE(
	"Draw Gradient safe R mask replaces the source shader and ignores unused divisions", "[draw_gradient]"
) {
	auto d = Fixture();
	Captured(d, "mask", "mask");
	Set(d.Nodes[0], "scale", 0.);
	Set(d.Nodes[0], "level_in", Vector2{0, 0});
	for (auto format : {SurfaceFormat::R8Unorm, SurfaceFormat::R16Float, SurfaceFormat::R32Float}) {
		Image m;
		const auto layout = CheckedSurfaceLayout(1, 1, format, Limits::MaximumEvaluationBytes);
		REQUIRE(layout);
		m.Width = m.Height = 1;
		m.Format = format;
		m.Pixels.resize(layout->Bytes);
		REQUIRE(StoreSurfacePixel(m, 0, 0, {.25, 0, 0, 1}));
		std::array captures{RequestImageSource{"mask", m}};
		const auto im = Run(d, Sources(captures));
		Pixel(im, 0, 0, {64, 64, 64, 255});
	}
}
TEST_CASE(
	"Draw Gradient Mask dimension multiplies authored raw dimension and persists captured shape",
	"[draw_gradient]"
) {
	auto d = Fixture();
	Set(d.Nodes[0], "dimension", Vector2{2, .5});
	Set(d.Nodes[0], "dimension_unit", EnumValue{2});
	Captured(d, "mask", "mask");
	std::array captures{RequestImageSource{
		"mask",
		Image{2, 2, {255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255}, 0}
	}};
	auto req = Sources(captures);
	auto im = Run(d, req);
	REQUIRE(im.Width == 4);
	REQUIRE(im.Height == 1);
	Pixel(im, 0, 0, {32, 32, 32, 255});
	Pixel(im, 3, 0, {223, 223, 223, 255});
	Document restored;
	Diagnostic diag;
	REQUIRE(Read(Write(d), restored, diag) == Status::Ok);
	CHECK(restored == d);
	CHECK(Run(restored, req) == im);
}
TEST_CASE(
	"Draw Gradient Reference center and raw fractional dimensions agree with physical authored inputs",
	"[draw_gradient]"
) {
	auto d = Fixture();
	Set(d.Nodes[0], "dimension", Vector2{1.5, 2.5});
	Set(d.Nodes[0], "center", Vector2{.25, .75});
	auto im = Run(d);
	REQUIRE(im.Width == 2);
	REQUIRE(im.Height == 2);
	Set(d.Nodes[0], "center", Vector2{.375, 1.875});
	Set(d.Nodes[0], "center_unit", EnumValue{0});
	CHECK(Run(d) == im);
}
TEST_CASE(
	"Draw Gradient dimension and shape row batches retain literal per-row observations", "[draw_gradient]"
) {
	auto d = Fixture();
	ArrayValue dimensions;
	dimensions.ElementType = ValueType::Vector2;
	dimensions.Elements = {Vector2{1, 1}, Vector2{2, 1}};
	Set(d.Nodes[0], "dimension", dimensions);
	auto p = Compiled(d);
	ImageArray out;
	Diagnostic diag;
	REQUIRE(EvaluateArray(d, p, "image", {}, out, diag) == Status::Ok);
	REQUIRE(out.Images.size() == 2);
	Pixel(out.Images[0], 0, 0, {128, 128, 128, 255});
	Pixel(out.Images[1], 0, 0, {64, 64, 64, 255});
	Pixel(out.Images[1], 1, 0, {191, 191, 191, 255});
	Document restored;
	REQUIRE(Read(Write(d), restored, diag) == Status::Ok);
	auto rp = Compiled(restored);
	ImageArray again;
	REQUIRE(EvaluateArray(restored, rp, "image", {}, again, diag) == Status::Ok);
	CHECK(out.Images == again.Images);
	CHECK(out.Items == again.Items);
}
TEST_CASE(
	"Draw Gradient linked Curve rows use genuine array nodes and survive persistence", "[draw_gradient]"
) {
	auto d = Fixture();
	Node curves{"curves", "pc.array", "", {}, {{"type", EnumValue{0}}}};
	curves.DynamicInputs = {{"input_0", ValueType::Any, Line()}, {"input_1", ValueType::Any, Line(1, 0)}};
	d.Nodes.push_back(std::move(curves));
	d.Links.push_back({"curves", "array", "field", "progress_remap"});
	auto plan = Compiled(d);
	ImageArray out;
	Diagnostic diag;
	const auto status = EvaluateArray(d, plan, "image", {}, out, diag);
	INFO(diag.Message);
	REQUIRE(status == Status::Ok);
	REQUIRE(out.Images.size() == 2);
	Pixel(out.Images[0], 0, 0, {64, 64, 64, 255});
	Pixel(out.Images[1], 0, 0, {191, 191, 191, 255});
	Document restored;
	REQUIRE(Read(Write(d), restored, diag) == Status::Ok);
	auto restoredPlan = Compiled(restored);
	ImageArray again;
	REQUIRE(EvaluateArray(restored, restoredPlan, "image", {}, again, diag) == Status::Ok);
	CHECK(again.Images == out.Images);
	CHECK(again.Items == out.Items);
}
TEST_CASE("Draw Gradient linked Gradient rows preserve colors and alpha", "[draw_gradient]") {
	auto d = Fixture();
	Node rows{"gradients", "pc.array", "", {}, {{"type", EnumValue{0}}}};
	rows.DynamicInputs = {
		{"input_0", ValueType::Any, Gradient{0, {{0, {255, 0, 0, 128}}, {1, {255, 0, 0, 128}}}}},
		{"input_1", ValueType::Any, Gradient{0, {{0, {0, 0, 255, 64}}, {1, {0, 0, 255, 64}}}}}
	};
	d.Nodes.push_back(std::move(rows));
	d.Links.push_back({"gradients", "array", "field", "gradient"});
	const auto plan = Compiled(d);
	ImageArray out;
	Diagnostic diag;
	const auto status = EvaluateArray(d, plan, "image", {}, out, diag);
	INFO(diag.Message);
	REQUIRE(status == Status::Ok);
	REQUIRE(out.Images.size() == 2);
	Pixel(out.Images[0], 0, 0, {255, 0, 0, 128});
	Pixel(out.Images[1], 1, 1, {0, 0, 255, 64});
	Document restored;
	REQUIRE(Read(Write(d), restored, diag) == Status::Ok);
	const auto rp = Compiled(restored);
	ImageArray again;
	REQUIRE(EvaluateArray(restored, rp, "image", {}, again, diag) == Status::Ok);
	CHECK(again.Images == out.Images);
}
TEST_CASE("Draw Gradient linked physical Dimension bypasses consumer Mask units", "[draw_gradient]") {
	auto d = Fixture();
	Set(d.Nodes[0], "dimension_unit", EnumValue{2});
	Node dimensions{"dimensions", "pc.array", "", {}, {{"type", EnumValue{0}}}};
	dimensions.DynamicInputs = {{"input_0", ValueType::Any, Vector2{2, 1}}};
	d.Nodes.push_back(std::move(dimensions));
	d.Links.push_back({"dimensions", "array", "field", "dimension"});
	const auto image = Run(d);
	REQUIRE(image.Width == 2);
	REQUIRE(image.Height == 1);
	Pixel(image, 0, 0, {64, 64, 64, 255});
	Pixel(image, 1, 0, {191, 191, 191, 255});
}
TEST_CASE(
	"Draw Gradient scalar animation seek preserves source progress and stored controls", "[draw_gradient]"
) {
	auto d = Fixture();
	d.Keyframes = {{"field", "shift", 0, 0., "linear"}, {"field", "shift", 10, .5, "linear"}};
	EvaluationRequest req;
	auto first = Run(d, req);
	req.Tick = 10;
	Pixel(Run(d, req), 0, 0, {191, 191, 191, 255});
	req.Tick = 5;
	Pixel(Run(d, req), 0, 0, {128, 128, 128, 255});
	req.Tick = 0;
	CHECK(Run(d, req) == first);
}
TEST_CASE("Draw Gradient native radial center and unused divisions have literal results", "[draw_gradient]") {
	auto d = Fixture(2);
	Set(d.Nodes[0], "dimension", Vector2{1, 1});
	Set(d.Nodes[0], "radius", 0.);
	Set(d.Nodes[0], "shape", Vector2{0, 0});
	Pixel(Run(d), 0, 0, {0, 0, 0, 255});
	d = Fixture();
	Set(d.Nodes[0], "radius", 0.);
	Set(d.Nodes[0], "shape", Vector2{0, 0});
	Pixel(Run(d), 0, 0, {64, 64, 64, 255});
}
TEST_CASE(
	"Draw Gradient source zero denominators and GLSL uniform bounds refuse atomically", "[draw_gradient]"
) {
	auto d = Fixture();
	Set(d.Nodes[0], "scale", 0.);
	Refused(d, Status::UnsupportedExecution, "scale");
	d = Fixture();
	Set(d.Nodes[0], "level_in", Vector2{0, 0});
	Refused(d, Status::UnsupportedExecution, "level_in");
	d = Fixture();
	auto c = Line();
	c.Header[1] = 0;
	Set(d.Nodes[0], "curve", c);
	Refused(d, Status::UnsupportedExecution, "curve");
	d = Fixture();
	c = Line();
	for (int i = 1; i <= 8; ++i)
		c.Anchors.insert(c.Anchors.end() - 1, {0, 0, i / 9., i / 9., 0, 0});
	Set(d.Nodes[0], "progress_remap", c);
	Refused(d, Status::UnsupportedExecution, "progress_remap");
	d = Fixture();
	Set(d.Nodes[0], "gradient", Gradient{6, {{0, {0, 0, 0, 255}}, {1, {255, 0, 0, 255}}}});
	Refused(d, Status::UnsupportedExecution, "surface_out");
}
TEST_CASE(
	"Draw Gradient expensive later dimension row is rejected before byte allocation", "[draw_gradient]"
) {
	auto d = Fixture();
	ArrayValue dimensions;
	dimensions.ElementType = ValueType::Vector2;
	dimensions.Elements = {Vector2{1, 1}, Vector2{1024, 1024}};
	Set(d.Nodes[0], "dimension", dimensions);
	for (uint64_t bytes : {uint64_t{512 * 1024}, Limits::MaximumOutputBytes})
		Refused(d, Status::LimitExceeded, "dimension", bytes);
}
TEST_CASE("Draw Gradient float output byte admission preserves prior image on refusal", "[draw_gradient]") {
	auto d = Fixture();
	Set(d.Nodes[0], "dimension", Vector2{256, 256});
	Set(d.Nodes[0], "attribute_color_depth", EnumValue{5});
	Refused(d, Status::LimitExceeded, "surface_out", 64 * 1024);
}
