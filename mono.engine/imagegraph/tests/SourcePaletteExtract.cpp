#include <engine/imagegraph/Document.hpp>
#include <engine/imagegraph/SimulationReplay.hpp>
#include <engine/imagegraph/SourceBuiltinRandom.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
TEST_SUITE_ID("engine.imagegraph.source_palette_extract")
using namespace engine::imagegraph;
namespace {
	Image Pixels(uint32_t w, uint32_t h, std::initializer_list<Colour> colors) {
		Image image{w, h, {}, 0};
		image.Pixels.reserve(size_t(w) * h * 4);
		for (auto c : colors)
			for (auto b : {c.Red, c.Green, c.Blue, c.Alpha})
				image.Pixels.push_back(b);
		return image;
	}
	Document Fixture(std::vector<AuthoredValue> controls = {}, bool mask = false) {
		Document d;
		d.FormatVersion = 9;
		d.Nodes = {
			{"input", "image.captured", "", {}, {{"source_id", std::string("input")}}},
			{"extract", "pc.palette_extract", "", {}, std::move(controls)}
		};
		d.Links = {{"input", "image", "extract", "surface_in"}};
		if (mask) {
			d.Nodes.push_back({"mask", "image.captured", "", {}, {{"source_id", std::string("mask")}}});
			d.Links.push_back({"mask", "image", "extract", "mask"});
		}
		d.Outputs = {{"colors", "extract", "palette"}};
		return d;
	}
	Plan Compiled(const Document &d) {
		Plan p;
		Diagnostic diag;
		auto s = Compile(d, p, diag);
		INFO(diag.Message);
		REQUIRE(s == Status::Ok);
		return p;
	}
	std::vector<Colour> Colors(const EvaluatedValue &out) {
		const auto &a = std::get<ArrayValue>(out.Data);
		std::vector<Colour> r;
		for (const auto &e : a.Elements)
			r.push_back(std::get<Colour>(e));
		return r;
	}
	EvaluatedValue Run(const Document &d, const EvaluationRequest &req) {
		auto p = Compiled(d);
		Diagnostic diag;
		EvaluatedValue out;
		auto s = EvaluateValue(d, p, "colors", req, out, diag);
		INFO(diag.Message);
		REQUIRE(s == Status::Ok);
		return out;
	}
}
TEST_CASE(
	"Palette Extract All Colors keeps encounter order and alpha identity under its native RGBA profile",
	"[palette_extract]"
) {
	const Colour red{255, 0, 0, 255}, green{0, 255, 0, 255}, half{255, 0, 0, 128};
	std::array sources{
		RequestImageSource{"input", Pixels(6, 1, {green, red, {1, 2, 3, 0}, half, green, red})}
	};
	EvaluationRequest req;
	req.ImageSources = sources;
	auto d = Fixture({{"algorithm", EnumValue{2}}, {"max_colors", int64_t{-1}}});
	CHECK(Colors(Run(d, req)) == std::vector<Colour>{green, red, half});
	std::array masked{
		RequestImageSource{"input", Pixels(2, 1, {{100, 80, 60, 128}, red})},
		RequestImageSource{"mask", Pixels(1, 1, {{128, 255, 0, 128}})}
	};
	req.ImageSources = masked;
	d = Fixture({{"algorithm", EnumValue{2}}, {"mask_alpha_only", true}}, true);
	CHECK(Colors(Run(d, req)) == std::vector<Colour>{{50, 80, 0, 64}, red});
	masked[0].Data = Pixels(2, 1, {{0, 0, 0, 0}, {4, 5, 6, 0}});
	CHECK(Colors(Run(d, req)).empty());
}
TEST_CASE(
	"Palette Extract Frequency caps sampling and uses verified HTML5 priority tie order", "[palette_extract]"
) {
	const Colour red{255, 0, 0, 255}, green{0, 255, 0, 255}, blue{0, 0, 255, 255};
	std::array sources{RequestImageSource{"input", Pixels(4, 1, {red, green, blue, red})}};
	EvaluationRequest req;
	req.ImageSources = sources;
	auto d = Fixture({{"algorithm", EnumValue{1}}, {"max_colors", int64_t{3}}});
	CHECK(Colors(Run(d, req)) == std::vector<Colour>{red, blue, green});
	d.Nodes[1].Values[1].Data = int64_t{0};
	CHECK(Colors(Run(d, req)) == std::vector<Colour>{red});
	Image large{256, 1, {}, 0};
	for (int i = 0; i < 256; ++i)
		for (auto b : {uint8_t(i % 2 ? 0 : 255), uint8_t(0), uint8_t(i % 2 ? 255 : 0), uint8_t(255)})
			large.Pixels.push_back(b);
	sources[0].Data = large;
	CHECK(Colors(Run(d, req)) == std::vector<Colour>{blue});
}
TEST_CASE(
	"Palette Extract K-mean requires observed calls and preserves source representative center order",
	"[palette_extract]"
) {
	const Colour red{255, 0, 0, 255}, blue{0, 0, 255, 255};
	std::array sources{RequestImageSource{"input", Pixels(3, 1, {red, blue, blue})}};
	EvaluationRequest req;
	req.ImageSources = sources;
	auto d = Fixture({{"color_space", EnumValue{0}}, {"max_colors", int64_t{2}}});
	auto p = Compiled(d);
	Diagnostic diag;
	EvaluatedValue out;
	out.Data = std::string("previous");
	CHECK(EvaluateValue(d, p, "colors", req, out, diag) == Status::UnsupportedExecution);
	CHECK(out.Data == Value{std::string("previous")});
	SourceBuiltinRandomCapture capture;
	auto s = PrepareSourceBuiltinRandomCapture(d, p, "extract", req, capture, diag);
	INFO(diag.Message);
	REQUIRE(s == Status::Ok);
	for (double v : {.9, .1, .1, .1, .1, .9})
		capture.Draws.push_back({SourceBuiltinRandomOperation::Random, 0, 1, v});
	req.BuiltinRandomCaptures = {&capture, 1};
	s = EvaluateValue(d, p, "colors", req, out, diag);
	INFO(diag.Message);
	REQUIRE(s == Status::Ok);
	CHECK(Colors(out) == std::vector<Colour>{red, blue});
	const auto previous = out.Data;
	capture.Draws.back().Upper = 2;
	CHECK(EvaluateValue(d, p, "colors", req, out, diag) == Status::InvalidValue);
	CHECK(out.Data == previous);
	capture.Draws.back().Upper = 1;
	sources[0].Data.Pixels[0] = 254;
	CHECK(EvaluateValue(d, p, "colors", req, out, diag) == Status::InvalidValue);
	CHECK(diag.Port == "surface_in");
	CHECK(out.Data == previous);
	sources[0].Data.Pixels[0] = 255;
	capture.Draws.pop_back();
	CHECK(EvaluateValue(d, p, "colors", req, out, diag) == Status::InvalidValue);
	CHECK(out.Data == previous);
}
TEST_CASE("Palette Extract defaults use HSV five centers and bounded32texel sampling", "[palette_extract]") {
	const Colour color{100, 50, 25, 128};
	std::array sources{RequestImageSource{"input", Pixels(1, 1, {color})}};
	auto d = Fixture();
	auto p = Compiled(d);
	EvaluationRequest req;
	req.ImageSources = sources;
	SourceBuiltinRandomCapture capture;
	Diagnostic diag;
	REQUIRE(PrepareSourceBuiltinRandomCapture(d, p, "extract", req, capture, diag) == Status::Ok);
	for (int i = 0; i < 15; ++i)
		capture.Draws.push_back({SourceBuiltinRandomOperation::Random, 0, 1, .25});
	req.BuiltinRandomCaptures = {&capture, 1};
	CHECK(Colors(Run(d, req)) == std::vector<Colour>{{100, 50, 25, 255}});
	Image large{64, 1, {}, 0};
	for (int i = 0; i < 64; ++i)
		for (auto b : {uint8_t(i % 2 ? 0 : 255), uint8_t(0), uint8_t(i % 2 ? 255 : 0), uint8_t(255)})
			large.Pixels.push_back(b);
	sources[0].Data = large;
	capture = {};
	req.BuiltinRandomCaptures = {};
	REQUIRE(PrepareSourceBuiltinRandomCapture(d, p, "extract", req, capture, diag) == Status::Ok);
	for (int i = 0; i < 15; ++i)
		capture.Draws.push_back({SourceBuiltinRandomOperation::Random, 0, 1, .25});
	req.BuiltinRandomCaptures = {&capture, 1};
	CHECK(Colors(Run(d, req)) == std::vector<Colour>{{0, 0, 255, 255}});
}
TEST_CASE(
	"Palette Extract preadmits workspace and refuses excessive unique palette without replacing output",
	"[palette_extract]"
) {
	Image image{65, 65, {}, 0};
	for (int i = 0; i < 65 * 65; ++i)
		for (auto b : {uint8_t(i), uint8_t(i >> 8), uint8_t(0), uint8_t(255)})
			image.Pixels.push_back(b);
	std::array sources{RequestImageSource{"input", image}};
	EvaluationRequest req;
	req.ImageSources = sources;
	auto d = Fixture({{"algorithm", EnumValue{2}}});
	auto p = Compiled(d);
	Diagnostic diag;
	EvaluatedValue out;
	out.Data = std::string("previous");
	CHECK(EvaluateValue(d, p, "colors", req, out, diag) == Status::LimitExceeded);
	CHECK(diag.Port == "palette");
	CHECK(out.Data == Value{std::string("previous")});
	d = Fixture({{"algorithm", EnumValue{1}}});
	p = Compiled(d);
	SimulationEvaluationResult bounded;
	bounded.Output = out;
	CHECK(EvaluateSimulation(d, p, "colors", req, bounded, diag, 65536) == Status::LimitExceeded);
	CHECK(std::get<EvaluatedValue>(bounded.Output).Data == out.Data);
	CHECK(diag.Port == "surface_in");
	CHECK(out.Data == Value{std::string("previous")});
}
TEST_CASE(
	"Palette Extract RGB and HSV choose different actual nearest representatives", "[palette_extract]"
) {
	const Colour red{255, 0, 0, 255}, blue{0, 0, 255, 255}, magenta{255, 0, 255, 255};
	std::array sources{RequestImageSource{"input", Pixels(3, 1, {red, magenta, blue})}};
	for (int64_t space : {0, 1}) {
		auto d = Fixture({{"color_space", EnumValue{space}}, {"max_colors", int64_t{1}}});
		auto p = Compiled(d);
		EvaluationRequest req;
		req.ImageSources = sources;
		Diagnostic diag;
		SourceBuiltinRandomCapture c;
		REQUIRE(PrepareSourceBuiltinRandomCapture(d, p, "extract", req, c, diag) == Status::Ok);
		c.Draws = {
			{SourceBuiltinRandomOperation::Random, 0, 1, .1},
			{SourceBuiltinRandomOperation::Random, 0, 1, .1},
			{SourceBuiltinRandomOperation::Random, 0, 1, .1}
		};
		req.BuiltinRandomCaptures = {&c, 1};
		CHECK(Colors(Run(d, req)) == std::vector<Colour>{space ? blue : magenta});
	}
}
TEST_CASE(
	"Palette Extract batches its main image input and rejects array-valued mask and controls",
	"[palette_extract]"
) {
	const Colour red{255, 0, 0, 255}, blue{0, 0, 255, 255};
	std::array sources{
		RequestImageSource{"input", Pixels(1, 1, {red})}, RequestImageSource{"mask", Pixels(1, 1, {blue})}
	};
	auto d = Fixture({{"algorithm", EnumValue{2}}}, true);
	Node rows{"rows", "pc.array", "", {}, {{"type", EnumValue{1}}}};
	rows.DynamicInputs = {
		{"input_0", ValueType::Image, std::nullopt}, {"input_1", ValueType::Image, std::nullopt}
	};
	d.Nodes.push_back(rows);
	d.Links = {
		{"input", "image", "rows", "input_0"},
		{"mask", "image", "rows", "input_1"},
		{"rows", "array", "extract", "surface_in"}
	};
	EvaluationRequest req;
	req.ImageSources = sources;
	auto out = Run(d, req);
	const auto &a = std::get<ArrayValue>(out.Data);
	REQUIRE(a.Nested.size() == 2);
	CHECK(a.Nested[0] == std::vector<ElementValue>{red});
	CHECK(a.Nested[1] == std::vector<ElementValue>{blue});
	d.Links = {
		{"input", "image", "rows", "input_0"},
		{"mask", "image", "rows", "input_1"},
		{"input", "image", "extract", "surface_in"},
		{"rows", "array", "extract", "mask"}
	};
	auto p = Compiled(d);
	Diagnostic diag;
	auto prior = out.Data;
	CHECK(EvaluateValue(d, p, "colors", req, out, diag) == Status::UnsupportedExecution);
	CHECK(diag.Port == "mask");
	CHECK(out.Data == prior);
	d = Fixture({{"algorithm", EnumValue{1}}});
	Node counts{"counts", "pc.array", "", {}, {{"type", EnumValue{2}}}};
	counts.DynamicInputs = {
		{"input_0", ValueType::Integer, Value{int64_t{1}}}, {"input_1", ValueType::Integer, Value{int64_t{2}}}
	};
	d.Nodes.push_back(counts);
	d.Links.push_back({"counts", "array", "extract", "max_colors"});
	p = Compiled(d);
	CHECK(EvaluateValue(d, p, "colors", req, out, diag) == Status::UnsupportedExecution);
	CHECK(diag.Port == "max_colors");
	CHECK(out.Data == prior);
}
TEST_CASE(
	"Palette Extract mask draw clips to mask dimensions and transparent fragments erase only covered alpha",
	"[palette_extract]"
) {
	const Colour a{100, 80, 60, 255}, b{10, 20, 30, 255}, c{40, 50, 60, 255}, d{70, 80, 90, 255},
		e{20, 30, 40, 255}, f{50, 60, 70, 255};
	std::array sources{
		RequestImageSource{"input", Pixels(3, 2, {a, b, c, d, e, f})},
		RequestImageSource{"mask", Pixels(2, 1, {{128, 255, 0, 128}, {255, 255, 255, 0}})}
	};
	auto doc = Fixture({{"algorithm", EnumValue{2}}}, true);
	EvaluationRequest req;
	req.ImageSources = sources;
	CHECK(Colors(Run(doc, req)) == std::vector<Colour>{{50, 80, 0, 128}, c, d, e, f});
	sources[1].Data = Pixels(1, 1, {{255, 255, 255, 0}});
	CHECK(Colors(Run(doc, req)) == std::vector<Colour>{b, c, d, e, f});
	Image larger{4, 3, std::vector<uint8_t>(4 * 3 * 4, 0), 0};
	for (size_t k = 44; k < 48; ++k)
		larger.Pixels[k] = 255;
	sources[1].Data = larger;
	CHECK(Colors(Run(doc, req)).empty());
}
TEST_CASE(
	"Palette Extract byte temporaries quantize HDR inputs and mask copy before multiplication",
	"[palette_extract]"
) {
	Image hdr{1, 1, std::vector<uint8_t>(16), 0, SurfaceFormat::RGBA32Float};
	REQUIRE(StoreSurfacePixel(hdr, 0, 0, {2, -1, .5, .5}));
	std::array sources{
		RequestImageSource{"input", hdr}, RequestImageSource{"mask", Pixels(1, 1, {{128, 255, 255, 255}})}
	};
	EvaluationRequest req;
	req.ImageSources = sources;
	for (int64_t algorithm : {1, 2}) {
		auto d = Fixture({{"algorithm", EnumValue{algorithm}}, {"attribute_color_depth", EnumValue{5}}});
		CHECK(Colors(Run(d, req)) == std::vector<Colour>{{255, 0, 128, 128}});
		d = Fixture({{"algorithm", EnumValue{algorithm}}, {"attribute_color_depth", EnumValue{5}}}, true);
		CHECK(Colors(Run(d, req)) == std::vector<Colour>{{128, 0, 128, 128}});
	}
	auto d = Fixture({{"attribute_color_depth", EnumValue{5}}});
	auto p = Compiled(d);
	EvaluatedValue out;
	out.Data = std::string("previous");
	Diagnostic diag;
	CHECK(EvaluateValue(d, p, "colors", req, out, diag) == Status::UnsupportedExecution);
	CHECK(diag.Port == "attribute_color_depth");
	CHECK(out.Data == Value{std::string("previous")});
}
TEST_CASE(
	"Palette Extract refuses source K-mean empty-color dereference and preadmits whole-batch clustering work",
	"[palette_extract]"
) {
	std::array sources{RequestImageSource{"input", Pixels(1, 1, {{100, 50, 25, 0}})}};
	auto d = Fixture({{"max_colors", int64_t{1}}});
	auto p = Compiled(d);
	EvaluationRequest req;
	req.ImageSources = sources;
	SourceBuiltinRandomCapture c;
	Diagnostic diag;
	REQUIRE(PrepareSourceBuiltinRandomCapture(d, p, "extract", req, c, diag) == Status::Ok);
	for (int i = 0; i < 3; ++i)
		c.Draws.push_back({SourceBuiltinRandomOperation::Random, 0, 1, .2});
	req.BuiltinRandomCaptures = {&c, 1};
	EvaluatedValue out;
	out.Data = std::string("previous");
	CHECK(EvaluateValue(d, p, "colors", req, out, diag) == Status::UnsupportedExecution);
	CHECK(diag.Port == "surface_in");
	CHECK(out.Data == Value{std::string("previous")});
	sources[0].Data = Image{32, 32, std::vector<uint8_t>(32 * 32 * 4, 255), 0};
	d = Fixture({{"max_colors", int64_t{4096}}});
	p = Compiled(d);
	req.BuiltinRandomCaptures = {};
	CHECK(EvaluateValue(d, p, "colors", req, out, diag) == Status::LimitExceeded);
	CHECK(diag.Port == "max_colors");
	CHECK(out.Data == Value{std::string("previous")});
}
TEST_CASE("Extracted palette drives an actual native gradient image consumer", "[palette_extract]") {
	const Colour red{255, 0, 0, 255}, blue{0, 0, 255, 255};
	std::array sources{RequestImageSource{"input", Pixels(4, 1, {red, red, blue, blue})}};
	auto d = Fixture({{"algorithm", EnumValue{1}}, {"max_colors", int64_t{2}}});
	d.Nodes.push_back({"gradient", "pc.gradient_palette", "", {}, {}});
	d.Nodes.push_back({"sample", "pc.gradient_out", "", {}, {{"sample", .25}}});
	d.Nodes.push_back(
		{"image", "pc.solid", "", {}, {{"dimension", Vector2{8, 1}}, {"dimension_unit", EnumValue{0}}}}
	);
	d.Links.push_back({"extract", "palette", "gradient", "palette"});
	d.Links.push_back({"gradient", "gradient", "sample", "gradient"});
	d.Links.push_back({"sample", "color", "image", "color"});
	d.Outputs = {{"image", "image", "surface_out"}};
	auto p = Compiled(d);
	EvaluationRequest req;
	req.ImageSources = sources;
	Diagnostic diag;
	Image actual;
	auto s = Evaluate(d, p, "image", req, actual, diag);
	INFO(diag.Message);
	REQUIRE(s == Status::Ok);
	CHECK(actual.Width == 8);
	CHECK(actual.Height == 1);
	ArrayValue expected;
	expected.ElementType = ValueType::Colour;
	expected.Elements = {blue, red};
	Document reference;
	reference.FormatVersion = 9;
	reference.Nodes = {
		{"gradient", "pc.gradient_palette", "", {}, {{"palette", expected}}},
		d.Nodes[d.Nodes.size() - 2],
		d.Nodes.back()
	};
	reference.Links = {{"gradient", "gradient", "sample", "gradient"}, {"sample", "color", "image", "color"}};
	reference.Outputs = d.Outputs;
	auto referencePlan = Compiled(reference);
	Image referenceImage;
	s = Evaluate(reference, referencePlan, "image", {}, referenceImage, diag);
	INFO(diag.Message);
	REQUIRE(s == Status::Ok);
	CHECK(actual == referenceImage);
}
