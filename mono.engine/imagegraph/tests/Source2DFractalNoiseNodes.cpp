#include "../src/NodeExecutors.hpp"

#include <engine/imagegraph/Document.hpp>
#include <engine/imagegraph/HostCapture.hpp>
#include <engine/imagegraph/StatefulReplay.hpp>
#include <engine/imagegraph/Surface.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cmath>

TEST_SUITE_ID("engine.imagegraph.source_fractal_noise")
using namespace engine::imagegraph;
namespace {
	Image NoiseSolid(uint32_t width, uint32_t height, Colour c) {
		Image image{width, height, {}, 0};
		image.Pixels.resize(size_t(width) * height * 4);
		for (size_t i = 0; i < image.Pixels.size(); i += 4) {
			image.Pixels[i] = c.Red;
			image.Pixels[i + 1] = c.Green;
			image.Pixels[i + 2] = c.Blue;
			image.Pixels[i + 3] = c.Alpha;
		}
		return image;
	}
	struct NoiseGraph {
		Document Doc;
		Plan Compiled;
		Diagnostic Error;
		EvaluationRequest Request;
		std::vector<std::pair<std::string, Image>> Sources;
		std::vector<HostNodeCapture> Captures;
		explicit NoiseGraph(std::string type) {
			Doc.FormatVersion = 9;
			Doc.Nodes = {
				{"noise",
				 std::move(type),
				 "",
				 {},
				 {{"dimension", Vector2{3, 2}}, {"dimension_unit", EnumValue{0}}, {"seed", 123.}}}
			};
			Doc.Outputs = {{"image", "noise", "surface_out"}};
		}
		void Set(std::string id, Value value) {
			for (auto &v : Doc.Nodes[0].Values)
				if (v.Port == id) {
					v.Data = std::move(value);
					return;
				}
			Doc.Nodes[0].Values.push_back({std::move(id), std::move(value)});
		}
		void Input(std::string port, Image image) {
			const auto id = port + "_source";
			Doc.Nodes.push_back({id, "pc.image", "", {}, {}});
			Doc.Links.push_back({id, "surface_out", "noise", std::move(port)});
			Sources.emplace_back(id, std::move(image));
		}
		void CompileNow() {
			const auto status = Compile(Doc, Compiled, Error);
			INFO(Error.Message);
			REQUIRE(status == Status::Ok);
			Request.HostCaptures = {};
			Captures.clear();
			Captures.reserve(Sources.size());
			for (const auto &[id, image] : Sources) {
				HostNodeCapture capture;
				REQUIRE(PrepareHostCapture(Doc, Compiled, id, Request, capture, Error) == Status::Ok);
				capture.Outputs = {
					{"path", std::string{}}, {"dimension", Vector2{double(image.Width), double(image.Height)}}
				};
				capture.Images = {{"surface_out", image}};
				capture.Images.back().Data.Hash = SurfaceHash(image);
				Captures.push_back(std::move(capture));
			}
			Request.HostCaptures = Captures;
		}
		Image Run() {
			CompileNow();
			Image out;
			const auto status = Evaluate(Doc, Compiled, "image", Request, out, Error);
			INFO(Error.Message);
			REQUIRE(status == Status::Ok);
			return out;
		}
		void Roundtrip() {
			Document restored;
			REQUIRE(Read(Write(Doc), restored, Error) == Status::Ok);
			Doc = std::move(restored);
		}
	};
	const std::vector<uint8_t> SIMPLEX_RAW{47,	47,	 47,  255, 171, 171, 171, 255, 152, 152, 152, 255,
										   182, 182, 182, 255, 25,	25,	 25,  255, 190, 190, 190, 255};
	const std::vector<uint8_t> SIMPLEX_TILED{114, 114, 114, 255, 98, 98, 98, 255, 140, 140, 140, 255,
											 151, 151, 151, 255, 66, 66, 66, 255, 173, 173, 173, 255};
} // namespace
TEST_CASE(
	"Simplex uses source IQ kernel with tiling default and explicit seed",
	"[imagegraph][source_fractal_noise]"
) {
	NoiseGraph g("pc.noise_simplex");
	CHECK(g.Run().Pixels == SIMPLEX_TILED);
	g.Set("tile", false);
	CHECK(g.Run().Pixels == SIMPLEX_RAW);
	g.Roundtrip();
	CHECK(g.Run().Pixels == SIMPLEX_RAW);
}
TEST_CASE(
	"Simplex accumulates all source octaves with normalized amplitude", "[imagegraph][source_fractal_noise]"
) {
	NoiseGraph g("pc.noise_simplex");
	g.Set("tile", false);
	g.Set("iteration", int64_t{3});
	CHECK(g.Run().Pixels == std::vector<uint8_t>{126, 126, 126, 255, 171, 171, 171, 255, 133, 133, 133, 255,
												 165, 165, 165, 255, 103, 103, 103, 255, 169, 169, 169, 255});
}
TEST_CASE(
	"Simplex channel offsets ranges and HSV conversion have independent "
	"source goldens",
	"[imagegraph][source_fractal_noise]"
) {
	const std::array<std::vector<uint8_t>, 2> expected{
		{{96,  145, 188, 255, 153, 112, 163, 255, 125, 146, 138, 255,
		  156, 119, 136, 255, 87,  116, 160, 255, 155, 167, 147, 255},
		 {81, 188, 108, 255, 92, 121, 163, 255, 59, 138, 134, 255,
		  72, 93,  136, 255, 87, 160, 91,  255, 50, 84,	 147, 255}}
	};
	for (int64_t mode = 1; mode <= 2; mode++) {
		NoiseGraph g("pc.noise_simplex");
		g.Set("tile", false);
		g.Set("color_mode", EnumValue{mode});
		g.Set("iteration", int64_t{2});
		g.Set("color_r_range", Vector2{.1, .8});
		g.Set("color_g_range", Vector2{.2, .7});
		g.Set("color_b_range", Vector2{.3, .9});
		CHECK(g.Run().Pixels == expected[size_t(mode - 1)]);
	}
}
TEST_CASE(
	"Simplex rotates source coordinates and applies reference positions "
	"before octaves",
	"[imagegraph][source_fractal_noise]"
) {
	NoiseGraph g("pc.noise_simplex");
	g.Set("tile", false);
	g.Set("iteration", int64_t{2});
	g.Set("position", Vector2{.1, -.2});
	g.Set("rotation", 37.);
	const auto expected = std::vector<uint8_t>{255, 255, 255, 255, 164, 164, 164, 255, 156, 156, 156, 255,
											   217, 217, 217, 255, 85,	85,	 85,  255, 132, 132, 132, 255};
	CHECK(g.Run().Pixels == expected);
	g.Set("position_unit", EnumValue{0});
	g.Set("position", Vector2{.3, -.4});
	CHECK(g.Run().Pixels == expected);
}
TEST_CASE("Simplex source untiled branch ignores level controls", "[imagegraph][source_fractal_noise]") {
	NoiseGraph g("pc.noise_simplex");
	g.Set("tile", false);
	g.Set("level_in", Vector2{.5, .5});
	g.Set("level_out", Vector2{-2, 2});
	CHECK(g.Run().Pixels == SIMPLEX_RAW);
	g.Set("tile", true);
	g.CompileNow();
	Image sentinel = NoiseSolid(1, 1, {7, 8, 9, 10});
	const auto before = sentinel;
	CHECK(Evaluate(g.Doc, g.Compiled, "image", g.Request, sentinel, g.Error) == Status::UnsupportedExecution);
	CHECK(sentinel == before);
	CHECK(g.Error.Port == "level_in");
}
TEST_CASE(
	"Simplex preserves UV source alpha and the mask empty shader ignores "
	"alpha-only flag",
	"[imagegraph][source_fractal_noise]"
) {
	NoiseGraph g("pc.noise_simplex");
	g.Set("tile", false);
	g.Input("uv_map", NoiseSolid(1, 1, {128, 128, 0, 128}));
	g.Input("mask", NoiseSolid(3, 2, {64, 128, 192, 128}));
	g.Set("dimension_unit", EnumValue{2});
	g.Set("dimension", Vector2{1, 1});
	const auto out = g.Run();
	CHECK(out.Width == 3);
	CHECK(out.Height == 2);
	CHECK(out.Pixels == NoiseSolid(3, 2, {186, 186, 186, 32}).Pixels);
	g.Set("mask_alpha_only", true);
	CHECK(g.Run().Pixels == out.Pixels);
	g.Set("uv_mix", 0.);
	auto raw = SIMPLEX_RAW;
	for (size_t i = 3; i < raw.size(); i += 4)
		raw[i] = 32;
	CHECK(g.Run().Pixels == raw);
}
TEST_CASE(
	"Simplex mapped source iteration endpoints are rounded before "
	"fractional shader octaves",
	"[imagegraph][source_fractal_noise]"
) {
	NoiseGraph g("pc.noise_simplex");
	g.Set("tile", false);
	g.Set("iteration_mapped", true);
	g.Set("iteration_map_range", Vector2{1.5, 3.5});
	g.Input("iteration_map", NoiseSolid(1, 1, {128, 128, 128, 255}));
	const auto out = g.Run();
	REQUIRE(out.Pixels.size() == 24);
	CHECK(out.Pixels != SIMPLEX_RAW);
	g.Roundtrip();
	CHECK(g.Run().Pixels == out.Pixels);
	// A source range remains a single mapped control, rather than two processor
	// rows.
	g.CompileNow();
	ImageArray frames;
	frames.Images.push_back(NoiseSolid(1, 1, {7, 6, 5, 4}));
	const auto previous = frames.Images;
	CHECK(EvaluateArray(g.Doc, g.Compiled, "image", g.Request, frames, g.Error) == Status::InvalidOutput);
	CHECK(g.Error.Message == "selected output is one image, not an image array");
	CHECK(frames.Images == previous);
}
TEST_CASE(
	"Simplex mapped scale uses two endpoints and a nearest "
	"aspect-coordinate map",
	"[imagegraph][source_fractal_noise]"
) {
	NoiseGraph g("pc.noise_simplex");
	g.Set("tile", false);
	g.Set("scale_mapped", true);
	g.Set("scale_map_range", Vector2{.5, .75});
	g.Set("scale_unit", EnumValue{0});
	g.Input("scale_map", NoiseSolid(1, 1, {0, 0, 0, 255}));
	const auto mapped = g.Run();
	NoiseGraph reference("pc.noise_simplex");
	reference.Set("tile", false);
	reference.Set("scale_unit", EnumValue{0});
	reference.Set("scale", Vector2{.5, .5});
	CHECK(mapped.Pixels == reference.Run().Pixels);
	g.Set("scale_mapped", false);
	g.Set("scale_unit", EnumValue{1});
	CHECK(g.Run().Pixels == SIMPLEX_RAW);
}
TEST_CASE(
	"Simplex undefined normalization and zero scale refuse atomically", "[imagegraph][source_fractal_noise]"
) {
	for (const auto &[port, value] : std::array<std::pair<std::string, Value>, 3>{
			 {{"amplitude", 1.}, {"amplitude", 0.}, {"scale", Vector2{0, 1}}}
		 }) {
		NoiseGraph g("pc.noise_simplex");
		g.Set(port, value);
		g.CompileNow();
		Image out = NoiseSolid(1, 1, {1, 2, 3, 4});
		const auto before = out;
		CHECK(Evaluate(g.Doc, g.Compiled, "image", g.Request, out, g.Error) == Status::UnsupportedExecution);
		CHECK(out == before);
		CHECK(g.Error.Port == port);
	}
}
TEST_CASE(
	"Simplex linked processor arrays are admitted as an entire batch "
	"before output growth",
	"[imagegraph][source_fractal_noise]"
) {
	NoiseGraph g("pc.noise_simplex");
	g.Set("dimension", Vector2{513, 513});
	g.Set("iteration", int64_t{4});
	g.Set("color_mode", EnumValue{1});
	ArrayValue scales{ValueType::Vector2, {Vector2{.25, .25}, Vector2{.5, .5}}};
	g.Set("scale", scales);
	g.CompileNow();
	ImageArray out;
	out.Images.push_back(NoiseSolid(1, 1, {1, 2, 3, 4}));
	const auto before = out;
	CHECK(EvaluateArray(g.Doc, g.Compiled, "image", g.Request, out, g.Error) == Status::LimitExceeded);
	CHECK(out.Images == before.Images);
	CHECK(g.Error.Message.find("entire processor batch") != std::string::npos);
}
TEST_CASE(
	"Noise animated transform and authored instance seek reproduce "
	"stateless pixels",
	"[imagegraph][source_fractal_noise]"
) {
	for (const auto type : {"pc.noise_simplex", "pc.ridge_noise"}) {
		NoiseGraph g(type);
		if (std::string_view(type) == "pc.noise_simplex")
			g.Set("tile", false);
		else
			g.Set("iteration", int64_t{0});
		g.Doc.Keyframes = {
			{"noise", "position", 0, Vector2{0, 0}}, {"noise", "position", 2, Vector2{.2, .1}}
		};
		g.Doc.Nodes[0].SourceAnimatedInputs = {"position"};
		Node leaf{"leaf", type, "", {}, {}};
		leaf.InstanceBase = "noise";
		g.Doc.Nodes.push_back(leaf);
		g.Doc.Outputs[0].NodeId = "leaf";
		const auto first = g.Run();
		g.Request.Tick = 2;
		const auto last = g.Run();
		CHECK(last.Pixels != first.Pixels);
		g.Request.Tick = 0;
		CHECK(g.Run().Pixels == first.Pixels);
		g.Roundtrip();
		g.Request.Tick = 2;
		CHECK(g.Run().Pixels == last.Pixels);
	}
}
TEST_CASE(
	"Ridge default pipeline includes source cell search Gaussian "
	"blending and pass quantization",
	"[imagegraph][source_fractal_noise]"
) {
	NoiseGraph g("pc.ridge_noise");
	g.Set("dimension", Vector2{1, 1});
	g.Set("seed", 0.);
	CHECK(g.Run().Pixels == std::vector<uint8_t>{255, 255, 255, 255});
	g.Roundtrip();
	CHECK(g.Run().Pixels == std::vector<uint8_t>{255, 255, 255, 255});
}
TEST_CASE(
	"Ridge zero iterations publish IQ initial height without a ridge pass",
	"[imagegraph][source_fractal_noise]"
) {
	NoiseGraph g("pc.ridge_noise");
	g.Set("dimension", Vector2{2, 2});
	g.Set("iteration", int64_t{0});
	CHECK(
		g.Run().Pixels ==
		std::vector<uint8_t>{105, 105, 105, 255, 180, 180, 180, 255, 98, 98, 98, 255, 176, 176, 176, 255}
	);
}
TEST_CASE(
	"Ridge heightmap Sine Sharp Mix Overlay and gradient multiplier "
	"follow source equations",
	"[imagegraph][source_fractal_noise]"
) {
	for (int64_t mode = 0; mode < 2; mode++)
		for (int64_t blend = 0; blend < 2; blend++) {
			NoiseGraph g("pc.ridge_noise");
			g.Set("dimension", Vector2{1, 1});
			g.Set("cell_scale", 0.);
			g.Set("ridge_scale", 0.);
			g.Set("blending", 0.);
			g.Set("mode", EnumValue{mode});
			g.Set("blend_mode", EnumValue{blend});
			g.Input("heightmap", NoiseSolid(1, 1, {102, 51, 25, 128}));
			const auto colour = blend ? Colour{253, 253, 253, 255} : Colour{186, 135, 109, 255};
			CHECK(g.Run().Pixels == NoiseSolid(1, 1, colour).Pixels);
			g.Set("ridge_multiply", true);
			const auto multiplied = g.Run();
			CHECK(multiplied.Pixels == NoiseSolid(1, 1, {102, 51, 25, 255}).Pixels);
		}
}
TEST_CASE(
	"Ridge applies UV alpha to ridge passes but heightmap-only copy ignores UV",
	"[imagegraph][source_fractal_noise]"
) {
	NoiseGraph g("pc.ridge_noise");
	g.Set("dimension", Vector2{1, 1});
	g.Set("cell_scale", 0.);
	g.Set("ridge_scale", 0.);
	g.Set("blending", 0.);
	g.Input("heightmap", NoiseSolid(1, 1, {102, 51, 25, 128}));
	g.Input("uv_map", NoiseSolid(1, 1, {128, 128, 0, 128}));
	CHECK(g.Run().Pixels == NoiseSolid(1, 1, {186, 135, 109, 192}).Pixels);
	g.Set("iteration", int64_t{0});
	CHECK(g.Run().Pixels == NoiseSolid(1, 1, {102, 51, 25, 128}).Pixels);
}
TEST_CASE(
	"Ridge undefined level and evolving factor fail without replacing output",
	"[imagegraph][source_fractal_noise]"
) {
	for (bool level : {false, true}) {
		NoiseGraph g("pc.ridge_noise");
		g.Set("iteration", int64_t{2});
		g.Set(level ? "level" : "itr_factor", level ? Value{Vector2{1, 1}} : Value{0.});
		g.CompileNow();
		Image out = NoiseSolid(1, 1, {1, 2, 3, 4});
		const auto before = out;
		CHECK(Evaluate(g.Doc, g.Compiled, "image", g.Request, out, g.Error) == Status::UnsupportedExecution);
		CHECK(out == before);
	}
}
TEST_CASE(
	"Ridge admits blur taps over every processor row before scratch allocation",
	"[imagegraph][source_fractal_noise]"
) {
	NoiseGraph g("pc.ridge_noise");
	g.Set("dimension", Vector2{513, 513});
	g.Set("iteration", int64_t{5});
	g.Set("itr_factor", 1.);
	g.Set("scale", ArrayValue{ValueType::Vector2, {Vector2{2, 2}, Vector2{3, 3}}});
	g.CompileNow();
	ImageArray out;
	out.Images.push_back(NoiseSolid(1, 1, {1, 2, 3, 4}));
	const auto before = out;
	CHECK(EvaluateArray(g.Doc, g.Compiled, "image", g.Request, out, g.Error) == Status::LimitExceeded);
	CHECK(out.Images == before.Images);
	CHECK(g.Error.Message.find("entire processor batch") != std::string::npos);
}
TEST_CASE(
	"Noise honors finite float surfaces and the mask RGBA8 intermediate", "[imagegraph][source_fractal_noise]"
) {
	NoiseGraph g("pc.noise_simplex");
	g.Set("tile", false);
	g.Set("attribute_color_depth", EnumValue{5});
	auto out = g.Run();
	CHECK(out.Format == SurfaceFormat::RGBA32Float);
	SurfacePixel pixel;
	REQUIRE(LoadSurfacePixel(out, 0, 0, pixel));
	CHECK(pixel[0] == Catch::Approx(.18348156241493502).epsilon(1e-6));
	g.Input("mask", NoiseSolid(1, 1, {255, 255, 255, 255}));
	out = g.Run();
	REQUIRE(LoadSurfacePixel(out, 0, 0, pixel));
	CHECK(pixel[0] == Catch::Approx(47. / 255).epsilon(1e-6));
}
TEST_CASE(
	"Noise seed absence and byte-cap failure preserve the caller candidate",
	"[imagegraph][source_fractal_noise]"
) {
	for (const auto type : {"pc.noise_simplex", "pc.ridge_noise"}) {
		NoiseGraph g(type);
		g.CompileNow();
		Image out = NoiseSolid(1, 1, {7, 8, 9, 10});
		const auto before = out;
		CHECK(Evaluate(g.Doc, g.Compiled, "image", g.Request, out, g.Error, 1) == Status::LimitExceeded);
		CHECK(out == before);
		auto &values = g.Doc.Nodes[0].Values;
		values.erase(
			std::remove_if(
				values.begin(), values.end(), [](const AuthoredValue &v) { return v.Port == "seed"; }
			),
			values.end()
		);
		g.CompileNow();
		CHECK(Evaluate(g.Doc, g.Compiled, "image", g.Request, out, g.Error) == Status::InvalidValue);
		CHECK(g.Error.Port == "seed");
		CHECK(out == before);
	}
}

TEST_CASE(
	"Simplex linked mapped integer range remains one shader input", "[imagegraph][source_fractal_noise]"
) {
	NoiseGraph g("pc.noise_simplex");
	g.Set("tile", false);
	g.Set("iteration_mapped", true);
	g.Input("iteration_map", NoiseSolid(1, 1, {128, 128, 128, 255}));
	Node ranges{"ranges", "pc.array", "", {}, {{"type", EnumValue{0}}}};
	ranges.DynamicInputs = {{"input_0", ValueType::Scalar, 1.5}, {"input_1", ValueType::Scalar, 3.5}};
	g.Doc.Nodes.push_back(ranges);
	g.Doc.Links.push_back({"ranges", "array", "noise", "iteration"});
	const auto linked = g.Run();
	NoiseGraph literal("pc.noise_simplex");
	literal.Set("tile", false);
	literal.Set("iteration_mapped", true);
	literal.Set("iteration_map_range", Vector2{1.5, 3.5});
	literal.Input("iteration_map", NoiseSolid(1, 1, {128, 128, 128, 255}));
	CHECK(linked.Pixels == literal.Run().Pixels);
}
TEST_CASE(
	"Ridge transposed gradient rotation contrast multiplier and "
	"iterative scale have source goldens",
	"[imagegraph][source_fractal_noise]"
) {
	const std::array<std::vector<uint8_t>, 2> expected{
		{{151, 87, 87, 255, 131, 3, 3, 255, 202, 10, 10, 255, 255, 5, 5, 255},
		 {223, 223, 223, 255, 128, 88, 88, 255, 192, 33, 33, 255, 255, 255, 255, 255}}
	};
	for (int64_t mode = 0; mode <= 1; mode++) {
		NoiseGraph g("pc.ridge_noise");
		g.Set("dimension", Vector2{2, 2});
		g.Set("iteration", int64_t{2});
		g.Set("itr_factor", 2.);
		g.Set("cell_scale", 0.);
		g.Set("blending", 0.);
		g.Set("ridge_scale", 4.);
		g.Set("ridge_rotation", 37.);
		g.Set("ridge_contrast", .8);
		g.Set("ridge_multiplier", 2.);
		g.Set("ridge_multiply", mode == 0);
		g.Set("mode", EnumValue{mode});
		g.Set("blend_mode", EnumValue{mode});
		g.Input("heightmap", Image{2, 2, {64, 0, 0, 255, 128, 0, 0, 255, 192, 0, 0, 255, 255, 0, 0, 255}, 0});
		CHECK(g.Run().Pixels == expected[size_t(mode)]);
		g.Roundtrip();
		CHECK(g.Run().Pixels == expected[size_t(mode)]);
	}
}
TEST_CASE(
	"Ridge nine-cell search and source sinusoidal seed displacement are "
	"observable",
	"[imagegraph][source_fractal_noise]"
) {
	NoiseGraph g("pc.ridge_noise");
	g.Set("dimension", Vector2{2, 2});
	g.Set("iteration", int64_t{2});
	g.Set("itr_factor", 2.);
	g.Set("cell_scale", 1.);
	g.Set("blending", 0.);
	g.Set("ridge_scale", 4.);
	g.Set("ridge_rotation", 37.);
	g.Set("ridge_contrast", .8);
	g.Set("ridge_multiplier", 2.);
	g.Set("ridge_multiply", true);
	g.Input("heightmap", Image{2, 2, {64, 0, 0, 255, 128, 0, 0, 255, 192, 0, 0, 255, 255, 0, 0, 255}, 0});
	CHECK(
		g.Run().Pixels ==
		std::vector<uint8_t>{163, 99, 99, 255, 200, 72, 72, 255, 255, 142, 142, 255, 255, 67, 67, 255}
	);
	g.Set("seed", 321.);
	CHECK(
		g.Run().Pixels ==
		std::vector<uint8_t>{174, 110, 110, 255, 240, 112, 112, 255, 255, 159, 159, 255, 255, 187, 187, 255}
	);
}
TEST_CASE(
	"Ridge Gaussian pass uses oversampling independently of unused "
	"interpolation attribute",
	"[imagegraph][source_fractal_noise]"
) {
	const std::array<std::vector<uint8_t>, 2> expected{
		{{163, 43, 43, 255, 167, 21, 21, 255, 191, 19, 19, 255, 206, 11, 11, 255},
		 {193, 17, 17, 255, 187, 22, 22, 255, 175, 23, 23, 255, 172, 32, 32, 255}}
	};
	for (int64_t oversample = 3; oversample <= 4; oversample++) {
		NoiseGraph g("pc.ridge_noise");
		g.Set("dimension", Vector2{2, 2});
		g.Set("iteration", int64_t{1});
		g.Set("cell_scale", 0.);
		g.Set("blending", 2.);
		g.Set("ridge_scale", 4.);
		g.Set("ridge_rotation", 37.);
		g.Set("ridge_contrast", .8);
		g.Set("ridge_multiplier", 2.);
		g.Set("ridge_multiply", true);
		g.Set("oversample", EnumValue{oversample});
		g.Input("heightmap", Image{2, 2, {64, 0, 0, 255, 128, 0, 0, 255, 192, 0, 0, 255, 255, 0, 0, 255}, 0});
		const auto out = g.Run();
		CHECK(out.Pixels == expected[size_t(oversample - 3)]);
		g.Set("interpolate", EnumValue{4});
		CHECK(g.Run().Pixels == out.Pixels);
	}
}

TEST_CASE(
	"Single-channel noise safe draws replace the mask shader but retain "
	"its RGBA8 quantization",
	"[imagegraph][source_fractal_noise]"
) {
	for (int64_t depth : {6, 7, 8}) {
		NoiseGraph g("pc.noise_simplex");
		g.Set("tile", false);
		g.Set("attribute_color_depth", EnumValue{depth});
		const auto initial = g.Run();
		CHECK(initial.Format == *SourceSurfaceFormat(depth));
		g.Input("mask", NoiseSolid(1, 1, {0, 0, 0, 0}));
		const auto masked = g.Run();
		SurfacePixel pixel;
		REQUIRE(LoadSurfacePixel(masked, 0, 0, pixel));
		CHECK(pixel[0] == Catch::Approx(47. / 255).epsilon(.001));
		g.Sources.back().second = NoiseSolid(1, 1, {255, 255, 255, 255});
		CHECK(g.Run() == masked);
		g.Roundtrip();
		CHECK(g.Run() == masked);
	}
}
TEST_CASE(
	"Ridge single-channel Gaussian safe draws preserve the unblurred result",
	"[imagegraph][source_fractal_noise]"
) {
	for (int64_t depth : {6, 7, 8}) {
		NoiseGraph g("pc.ridge_noise");
		g.Set("dimension", Vector2{2, 2});
		g.Set("attribute_color_depth", EnumValue{depth});
		g.Set("cell_scale", 0.);
		g.Set("ridge_scale", 4.);
		g.Set("ridge_rotation", 37.);
		g.Set("ridge_contrast", .8);
		g.Set("ridge_multiply", true);
		g.Set("blending", 0.);
		g.Input("heightmap", Image{2, 2, {64, 0, 0, 255, 128, 0, 0, 255, 192, 0, 0, 255, 255, 0, 0, 255}, 0});
		const auto unblurred = g.Run();
		g.Set("blending", 2.);
		CHECK(g.Run() == unblurred);
		g.Roundtrip();
		CHECK(g.Run() == unblurred);
	}
}

TEST_CASE(
	"Noise processor scale rows publish independent surfaces in source array order",
	"[imagegraph][source_fractal_noise]"
) {
	const std::array<Vector2, 2> scales{Vector2{.5, .5}, Vector2{.75, .75}};
	for (const auto type : {"pc.noise_simplex", "pc.ridge_noise"}) {
		NoiseGraph g(type);
		if (std::string_view(type) == "pc.noise_simplex") {
			g.Set("tile", false);
			g.Set("scale_unit", EnumValue{0});
		} else
			g.Set("iteration", int64_t{0});
		g.Set("scale", ArrayValue{ValueType::Vector2, {scales[0], scales[1]}});
		g.CompileNow();
		ImageArray rows;
		const auto status = EvaluateArray(g.Doc, g.Compiled, "image", g.Request, rows, g.Error);
		INFO(g.Error.Message);
		REQUIRE(status == Status::Ok);
		REQUIRE(rows.Images.size() == 2);
		REQUIRE(rows.Images[0].Pixels != rows.Images[1].Pixels);
		for (size_t i = 0; i < scales.size(); i++) {
			NoiseGraph literal(type);
			if (std::string_view(type) == "pc.noise_simplex") {
				literal.Set("tile", false);
				literal.Set("scale_unit", EnumValue{0});
			} else
				literal.Set("iteration", int64_t{0});
			literal.Set("scale", scales[i]);
			CHECK(rows.Images[i] == literal.Run());
		}
		g.Roundtrip();
		g.CompileNow();
		ImageArray restored;
		REQUIRE(EvaluateArray(g.Doc, g.Compiled, "image", g.Request, restored, g.Error) == Status::Ok);
		CHECK(restored.Images == rows.Images);
	}
}

TEST_CASE(
	"Ridge zero-pass single-channel heightmaps expand red and need no unused seed observation",
	"[imagegraph][source_fractal_noise]"
) {
	for (int64_t depth : {6, 7, 8}) {
		NoiseGraph g("pc.ridge_noise");
		g.Set("dimension", Vector2{1, 1});
		g.Set("iteration", int64_t{0});
		auto &values = g.Doc.Nodes[0].Values;
		values.erase(
			std::remove_if(
				values.begin(), values.end(), [](const AuthoredValue &v) { return v.Port == "seed"; }
			),
			values.end()
		);
		const auto format = *SourceSurfaceFormat(depth);
		const auto layout = CheckedSurfaceLayout(1, 1, format, 4);
		REQUIRE(layout);
		Image height;
		height.Width = height.Height = 1;
		height.Format = format;
		height.Pixels.resize(layout->Bytes);
		REQUIRE(StoreSurfacePixel(height, 0, 0, {.4, 0, 0, 0}));
		g.Input("heightmap", std::move(height));
		CHECK(g.Run().Pixels == std::vector<uint8_t>{102, 102, 102, 255});
		g.Roundtrip();
		CHECK(g.Run().Pixels == std::vector<uint8_t>{102, 102, 102, 255});
	}
}

TEST_CASE(
	"Noise admits expensive later dimensions and controls before the first cheap row allocation",
	"[imagegraph][source_fractal_noise]"
) {
	for (const auto type : {"pc.noise_simplex", "pc.ridge_noise"}) {
		for (bool differentDimension : {false, true}) {
			NoiseGraph g(type);
			if (std::string_view(type) == "pc.noise_simplex") {
				g.Set("color_mode", EnumValue{1});
				if (differentDimension) {
					g.Set("dimension", ArrayValue{ValueType::Vector2, {Vector2{1, 1}, Vector2{513, 513}}});
					g.Set("iteration", int64_t{4});
				} else {
					g.Set("dimension", Vector2{513, 513});
					g.Set("iteration", ArrayValue{ValueType::Integer, {int64_t{1}, int64_t{4}}});
				}
			} else {
				g.Set("itr_factor", 1.);
				if (differentDimension) {
					g.Set("dimension", ArrayValue{ValueType::Vector2, {Vector2{1, 1}, Vector2{513, 513}}});
					g.Set("iteration", int64_t{5});
				} else {
					g.Set("dimension", Vector2{513, 513});
					g.Set("iteration", ArrayValue{ValueType::Integer, {int64_t{0}, int64_t{5}}});
				}
			}
			g.CompileNow();
			for (uint64_t budget : {uint64_t{512 * 1024}, Limits::MaximumOutputBytes}) {
				Image out = NoiseSolid(1, 1, {9, 8, 7, 6});
				const auto before = out;
				CHECK(
					Evaluate(g.Doc, g.Compiled, "image", g.Request, out, g.Error, budget) ==
					Status::LimitExceeded
				);
				INFO(g.Error.Message);
				CHECK(g.Error.Message.find("entire processor batch") != std::string::npos);
				CHECK(out == before);
			}
		}
	}
}

TEST_CASE(
	"Noise Mask Size admission scans every upstream image frame before noise output allocation",
	"[imagegraph][source_fractal_noise]"
) {
	NoiseGraph g("pc.noise_simplex");
	g.Set("dimension_unit", EnumValue{2});
	g.Set("iteration", int64_t{4});
	g.Set("color_mode", EnumValue{1});
	g.Doc.Nodes.push_back({"small", "pc.image", "", {}, {}});
	g.Doc.Nodes.push_back({"large", "pc.image", "", {}, {}});
	g.Sources.emplace_back("small", NoiseSolid(1, 1, {255, 255, 255, 255}));
	g.Sources.emplace_back("large", NoiseSolid(513, 513, {255, 255, 255, 255}));
	Node masks{"masks", "pc.array", "", {}, {{"type", EnumValue{1}}}};
	masks.DynamicInputs = {{"input_0", ValueType::Image, {}}, {"input_1", ValueType::Image, {}}};
	g.Doc.Nodes.push_back(std::move(masks));
	g.Doc.Links = {
		{"small", "surface_out", "masks", "input_0"},
		{"large", "surface_out", "masks", "input_1"},
		{"masks", "array", "noise", "mask"}
	};
	g.CompileNow();
	ImageArray out;
	out.Images.push_back(NoiseSolid(1, 1, {9, 8, 7, 6}));
	const auto before = out.Images;
	CHECK(EvaluateArray(g.Doc, g.Compiled, "image", g.Request, out, g.Error) == Status::LimitExceeded);
	INFO(g.Error.Message);
	CHECK(g.Error.Message.find("entire processor batch") != std::string::npos);
	CHECK(out.Images == before);
}

TEST_CASE(
	"Noise synthetic dimension units reject an unselected linked array atomically",
	"[imagegraph][source_fractal_noise]"
) {
	NoiseGraph g("pc.noise_simplex");
	g.Set("dimension", Vector2{513, 513});
	g.Set("iteration", int64_t{4});
	g.Set("color_mode", EnumValue{1});
	g.Input("mask", NoiseSolid(1, 1, {255, 255, 255, 255}));
	Node units{"units", "pc.array", "", {}, {{"type", EnumValue{0}}}};
	units.DynamicInputs = {{"input_0", ValueType::Scalar, 2.}, {"input_1", ValueType::Scalar, 0.}};
	g.Doc.Nodes.push_back(std::move(units));
	g.Doc.Links.push_back({"units", "array", "noise", "dimension_unit"});
	g.CompileNow();
	for (uint64_t budget : {uint64_t{512 * 1024}, Limits::MaximumOutputBytes}) {
		Image out = NoiseSolid(1, 1, {9, 8, 7, 6});
		const auto before = out;
		CHECK(
			Evaluate(g.Doc, g.Compiled, "image", g.Request, out, g.Error, budget) ==
			Status::UnsupportedExecution
		);
		INFO(g.Error.Message);
		CHECK(g.Error.Port == "dimension_unit");
		CHECK(g.Error.Message == "integer reader cannot consume an array input");
		CHECK(out == before);
	}
}

TEST_CASE(
	"Noise private preflight conservatively covers mixed borrowed unit observations before admission",
	"[imagegraph][source_fractal_noise]"
) {
	NoiseGraph g("pc.noise_simplex");
	g.Set("dimension", Vector2{513, 513});
	g.Set("iteration", int64_t{4});
	g.Set("color_mode", EnumValue{1});
	const auto *entry = FindCatalogueEntry("pc.noise_simplex");
	const auto executor = detail::FindExecutor("pc.noise_simplex");
	REQUIRE(entry);
	REQUIRE(executor);
	const Value selectedUnit = EnumValue{2};
	const Value originalUnits = ArrayValue{ValueType::Enum, {EnumValue{2}, EnumValue{0}}};
	const std::array originals{std::pair<std::string_view, const Value *>{"dimension_unit", &originalUnits}};
	const Image mask = NoiseSolid(1, 1, {255, 255, 255, 255});
	detail::EvaluationBudget budget(512 * 1024);
	detail::NodeContext context(g.Doc.Nodes[0], *entry, g.Request, budget);
	context.ByteBudget = 512 * 1024;
	for (const auto &value : g.Doc.Nodes[0].Values)
		context.Values.emplace_back(value.Port, value.Data);
	context.ValueViews = {{"dimension_unit", &selectedUnit}};
	context.ProcessorOriginalValues = originals;
	context.ProcessorCount = 2;
	context.Images = {{"mask", &mask}};
	// Synthetic unit attributes are not graph-batched. These private selected/original views exercise
	// conservative admission defensively without claiming a native graph can create that schedule.
	CHECK_FALSE(executor(context));
	CHECK(context.FailureCode == Status::LimitExceeded);
	CHECK(context.FailureMessage.find("entire processor batch") != std::string::npos);
	CHECK(context.OutputImages.empty());
	CHECK(context.OutputImageArrays.empty());
	CHECK(budget.Peak() == 0);
}

TEST_CASE(
	"Noise Mask units multiply authored components and survive persistence", "[source_noise_mask_dimension]"
) {
	for (const auto type : {"pc.noise_simplex", "pc.ridge_noise"}) {
		NoiseGraph g(type);
		g.Set("dimension", Vector2{2, .5});
		g.Set("dimension_unit", EnumValue{2});
		g.Input("mask", NoiseSolid(2, 2, {255, 255, 255, 255}));
		const auto masked = g.Run();
		REQUIRE(masked.Width == 4);
		REQUIRE(masked.Height == 1);
		g.Roundtrip();
		CHECK(g.Run() == masked);
		g.Set("dimension", Vector2{4, 1});
		g.Set("dimension_unit", EnumValue{0});
		CHECK(g.Run() == masked);
	}
}
TEST_CASE(
	"Noise Mask projected fractions round half to even and clamp small sides", "[source_noise_mask_dimension]"
) {
	for (const auto type : {"pc.noise_simplex", "pc.ridge_noise"}) {
		for (const auto raw : {Vector2{.25, .75}, Vector2{.75, 1.25}, Vector2{.1, .1}}) {
			NoiseGraph g(type);
			g.Set("dimension", raw);
			g.Set("dimension_unit", EnumValue{2});
			g.Input("mask", NoiseSolid(2, 2, {255, 255, 255, 255}));
			const auto out = g.Run();
			const auto w = raw.X == .25 ? 1u : (raw.X == .75 ? 2u : 1u);
			const auto h = raw.Y == .1 ? 1u : 2u;
			CHECK(out.Width == w);
			CHECK(out.Height == h);
		}
	}
}
TEST_CASE(
	"Noise linked physical Dimension bypasses consumer Mask units without requiring a mask",
	"[source_noise_mask_dimension]"
) {
	for (const auto type : {"pc.noise_simplex", "pc.ridge_noise"}) {
		NoiseGraph g(type);
		g.Set("dimension_unit", EnumValue{2});
		Node dimensions{"dimensions", "pc.array", "", {}, {{"type", EnumValue{0}}}};
		dimensions.DynamicInputs = {{"input_0", ValueType::Any, Vector2{3, 2}}};
		g.Doc.Nodes.push_back(std::move(dimensions));
		g.Doc.Links.push_back({"dimensions", "array", "noise", "dimension"});
		const auto out = g.Run();
		CHECK(out.Width == 3);
		CHECK(out.Height == 2);
	}
}
TEST_CASE(
	"Noise heterogeneous Mask rows multiply each captured frame extent", "[source_noise_mask_dimension]"
) {
	NoiseGraph g("pc.noise_simplex");
	g.Set("dimension", Vector2{2, .5});
	g.Set("dimension_unit", EnumValue{2});
	g.Doc.Nodes.push_back({"small", "pc.image", "", {}, {}});
	g.Doc.Nodes.push_back({"large", "pc.image", "", {}, {}});
	g.Sources.emplace_back("small", NoiseSolid(2, 2, {255, 255, 255, 255}));
	g.Sources.emplace_back("large", NoiseSolid(3, 4, {255, 255, 255, 255}));
	Node masks{"masks", "pc.array", "", {}, {{"type", EnumValue{1}}}};
	masks.DynamicInputs = {{"input_0", ValueType::Image, {}}, {"input_1", ValueType::Image, {}}};
	g.Doc.Nodes.push_back(std::move(masks));
	g.Doc.Links = {
		{"small", "surface_out", "masks", "input_0"},
		{"large", "surface_out", "masks", "input_1"},
		{"masks", "array", "noise", "mask"}
	};
	g.CompileNow();
	ImageArray out;
	const auto status = EvaluateArray(g.Doc, g.Compiled, "image", g.Request, out, g.Error);
	INFO(g.Error.Message);
	REQUIRE(status == Status::Ok);
	REQUIRE(out.Images.size() == 2);
	CHECK(out.Images[0].Width == 4);
	CHECK(out.Images[0].Height == 1);
	CHECK(out.Images[1].Width == 6);
	CHECK(out.Images[1].Height == 2);
}
TEST_CASE(
	"Noise Mask multiplier later row is work-admitted before tight-budget output allocation",
	"[source_noise_mask_dimension]"
) {
	NoiseGraph g("pc.noise_simplex");
	g.Set("dimension", Vector2{2, 2});
	g.Set("dimension_unit", EnumValue{2});
	g.Set("iteration", int64_t{4});
	g.Set("color_mode", EnumValue{1});
	g.Doc.Nodes.push_back({"small", "pc.image", "", {}, {}});
	g.Doc.Nodes.push_back({"large", "pc.image", "", {}, {}});
	g.Sources.emplace_back("small", NoiseSolid(1, 1, {255, 255, 255, 255}));
	g.Sources.emplace_back("large", NoiseSolid(257, 257, {255, 255, 255, 255}));
	Node masks{"masks", "pc.array", "", {}, {{"type", EnumValue{1}}}};
	masks.DynamicInputs = {{"input_0", ValueType::Image, {}}, {"input_1", ValueType::Image, {}}};
	g.Doc.Nodes.push_back(std::move(masks));
	g.Doc.Links = {
		{"small", "surface_out", "masks", "input_0"},
		{"large", "surface_out", "masks", "input_1"},
		{"masks", "array", "noise", "mask"}
	};
	g.CompileNow();
	for (const auto bytes : {uint64_t{2 * 1024 * 1024}, Limits::MaximumOutputBytes}) {
		Image out = NoiseSolid(1, 1, {9, 8, 7, 6});
		const auto prior = out;
		const auto status = Evaluate(g.Doc, g.Compiled, "image", g.Request, out, g.Error, bytes);
		INFO(g.Error.Message);
		CHECK(status == Status::LimitExceeded);
		CHECK(g.Error.Port == "iteration");
		CHECK(g.Error.Message.find("entire processor batch") != std::string::npos);
		CHECK(out == prior);
	}
}
TEST_CASE(
	"Noise Mask projected later dimensions exceed native limits before allocation",
	"[source_noise_mask_dimension]"
) {
	NoiseGraph g("pc.noise_simplex");
	g.Set("dimension", ArrayValue{ValueType::Vector2, {Vector2{1, 1}, Vector2{3, 1}}});
	g.Set("dimension_unit", EnumValue{2});
	g.Input("mask", NoiseSolid(2048, 1, {255, 255, 255, 255}));
	g.CompileNow();
	Image out = NoiseSolid(1, 1, {9, 8, 7, 6});
	const auto prior = out;
	const auto status = Evaluate(g.Doc, g.Compiled, "image", g.Request, out, g.Error, 64 * 1024);
	INFO(g.Error.Message);
	CHECK(status == Status::LimitExceeded);
	CHECK(g.Error.Port == "dimension");
	CHECK(out == prior);
}
TEST_CASE(
	"Noise Mask multiplication finite and float-byte refusals preserve the previous image",
	"[source_noise_mask_dimension]"
) {
	NoiseGraph g("pc.noise_simplex");
	g.Set("dimension", Vector2{1.e308, 1});
	g.Set("dimension_unit", EnumValue{2});
	g.Input("mask", NoiseSolid(2, 2, {255, 255, 255, 255}));
	g.CompileNow();
	Image out = NoiseSolid(1, 1, {9, 8, 7, 6});
	const auto prior = out;
	CHECK(Evaluate(g.Doc, g.Compiled, "image", g.Request, out, g.Error) == Status::InvalidValue);
	CHECK(g.Error.Port == "dimension");
	CHECK(out == prior);
	g.Set("dimension", Vector2{128, 128});
	g.Set("attribute_color_depth", EnumValue{5});
	g.CompileNow();
	CHECK(Evaluate(g.Doc, g.Compiled, "image", g.Request, out, g.Error, 64 * 1024) == Status::LimitExceeded);
	CHECK(g.Error.Port == "surface_out");
	CHECK(out == prior);
}
TEST_CASE(
	"Noise later negative Mask projection overflow is refused before first output growth",
	"[source_noise_mask_dimension]"
) {
	NoiseGraph g("pc.noise_simplex");
	g.Set("dimension", ArrayValue{ValueType::Vector2, {Vector2{1, 1}, Vector2{-1.e308, 1}}});
	g.Set("dimension_unit", EnumValue{2});
	g.Input("mask", NoiseSolid(2, 2, {255, 255, 255, 255}));
	g.CompileNow();
	Image out = NoiseSolid(1, 1, {9, 8, 7, 6});
	const auto prior = out;
	const auto status = Evaluate(g.Doc, g.Compiled, "image", g.Request, out, g.Error, 64 * 1024);
	INFO(g.Error.Message);
	CHECK(status == Status::InvalidValue);
	CHECK(g.Error.Port == "dimension");
	CHECK(out == prior);
}
