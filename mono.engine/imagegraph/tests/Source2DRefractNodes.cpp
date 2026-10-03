#include "../src/nodes/SourceRefractClean.hpp"
#include "ComplexGeneratorFixture.hpp"

#include <engine/imagegraph/StatefulReplay.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_approx.hpp>
TEST_SUITE_ID("engine.imagegraph.source_refract")
using namespace complex_generator_test;
namespace {
	constexpr Colour NORMAL{255, 128, 255, 255};
	Node Texture(std::string id, uint32_t width, uint32_t height) {
		MatrixValue matrix{width, height, {}};
		ArrayValue palette{ValueType::Colour, {}};
		for (uint32_t x = 0; x < width; ++x)
			palette.Elements.emplace_back(
				Colour{uint8_t(x * 24), uint8_t(255 - x * 24), uint8_t(x * 12), 255}
			);
		for (uint32_t y = 0; y < height; ++y)
			for (uint32_t x = 0; x < width; ++x)
				matrix.Values.push_back(x);
		return {
			std::move(id),
			"pc.interpret_matrix",
			"",
			{},
			{{"matrix", matrix},
			 {"palette", palette},
			 {"mode", EnumValue{1}},
			 {"dimension", Vector2{double(width), double(height)}},
			 {"dimension_unit", EnumValue{0}},
			 {"attribute_color_depth", EnumValue{3}}}
		};
	}
	Document Refract() {
		Document d;
		d.FormatVersion = 9;
		d.Nodes = {
			{"generator",
			 "pc.refract",
			 "",
			 {},
			 {{"height", 0.},
			  {"distance", .25},
			  {"ior", .5},
			  {"interpolate", EnumValue{1}},
			  {"mask_feather", 0.}}},
			Texture("source", 8, 4),
			Solid("normal", {2, 1}, NORMAL),
			Solid("depth", {1, 1}, {255, 255, 255, 255})
		};
		d.Links = {
			{"source", "surface_out", "generator", "surface_in"},
			{"normal", "surface_out", "generator", "normal_map"},
			{"depth", "surface_out", "generator", "depth_map"}
		};
		d.Outputs = {{"out", "generator", "surface_out"}};
		return d;
	}
	Image Source(Document d) {
		d.Outputs = {{"out", "source", "surface_out"}};
		return Draw(d);
	}
	void Same(const Image &a, const Image &b) {
		CHECK(a.Width == b.Width);
		CHECK(a.Height == b.Height);
		CHECK(a.Format == b.Format);
		CHECK(a.Pixels == b.Pixels);
		CHECK(a.Hash == SurfaceHash(a));
		CHECK(a == b);
	}
	// Independent scalar vector arithmetic for the GLSL nearest/Clamp route.
	Image Oracle(
		const Image &source,
		Colour normal,
		double depth,
		double height,
		double distance,
		double eta,
		double perspective = 0
	) {
		const std::array n{normal.Red / 255. * 2 - 1, normal.Green / 255. * 2 - 1, normal.Blue / 255.};
		const double nl = std::hypot(n[0], n[1], n[2]);
		Image expected = source;
		for (uint32_t y = 0; y < source.Height; ++y)
			for (uint32_t x = 0; x < source.Width; ++x) {
				const double u = (x + .5) / source.Width, v = (y + .5) / source.Height;
				const std::array eye{(u - .5) * perspective, (v - .5) * perspective, -1.};
				const double el = std::hypot(eye[0], eye[1], eye[2]);
				double dot = 0;
				for (size_t axis = 0; axis < 3; ++axis)
					dot += n[axis] / nl * eye[axis] / el;
				const double discriminant = 1 - eta * eta * (1 - dot * dot),
							 depthDistance = distance + depth * height;
				double sx = u, sy = v;
				if (discriminant >= 0) {
					const double correction = eta * dot + std::sqrt(discriminant);
					sx += (eta * eye[0] / el - correction * n[0] / nl) * depthDistance;
					sy += (eta * eye[1] / el - correction * n[1] / nl) * depthDistance;
				}
				const auto ix = uint32_t(
							   std::clamp(std::floor(sx * source.Width), 0., double(source.Width - 1))
						   ),
						   iy = uint32_t(
							   std::clamp(std::floor(sy * source.Height), 0., double(source.Height - 1))
						   );
				for (size_t c = 0; c < 4; ++c)
					expected.Pixels[(size_t(y) * source.Width + x) * 4 + c] =
						source.Pixels[(size_t(iy) * source.Width + ix) * 4 + c];
			}
		expected.Hash = SurfaceHash(expected);
		return expected;
	}
	// Source Gaussian radius 2, sampleMode 1 and two RGBA8 passes over an opaque 8x4 mask.
	// Independent shader-equation goldens include transparent padding and intermediate quantization.
	Image FeatheredOpaqueMaskOracle(const Image &original, const Image &refracted) {
		REQUIRE(original.Width == 8);
		REQUIRE(original.Height == 4);
		REQUIRE(refracted.Width == original.Width);
		REQUIRE(refracted.Height == original.Height);
		REQUIRE(original.Format == SurfaceFormat::RGBA8Unorm);
		REQUIRE(refracted.Format == SurfaceFormat::RGBA8Unorm);
		constexpr std::array<uint8_t, 32> alpha{125, 179, 179, 179, 179, 179, 179, 125, 179, 255, 255,
												255, 255, 255, 255, 179, 179, 255, 255, 255, 255, 255,
												255, 179, 125, 179, 179, 179, 179, 179, 179, 125};
		Image expected = original;
		for (size_t pixel = 0; pixel < alpha.size(); ++pixel) {
			const size_t offset = pixel * 4;
			REQUIRE(original.Pixels[offset + 3] != 0);
			REQUIRE(refracted.Pixels[offset + 3] != 0);
			const double ratio = alpha[pixel] / 255.;
			for (size_t channel = 0; channel < 4; ++channel)
				expected.Pixels[offset + channel] = uint8_t(
					std::lround(
						original.Pixels[offset + channel] * (1 - ratio) +
						refracted.Pixels[offset + channel] * ratio
					)
				);
		}
		expected.Hash = SurfaceHash(expected);
		return expected;
	}
	void Remove(Document &d, std::string_view port) {
		std::erase_if(d.Links, [&](const Link &link) {
			return link.ToNode == "generator" && link.ToPort == port;
		});
	}
}
TEST_CASE("Refract literal IOR directly controls refraction and total reflection", "[source_refract]") {
	for (double eta : {0., .5, 1., 2., -1.}) {
		auto d = Refract();
		Set(d, "ior", eta);
		const auto source = Source(d);
		Same(Draw(d), Oracle(source, NORMAL, 1, 0, .25, eta));
	}
}
TEST_CASE("Refract zero distance retains full source metadata", "[source_refract]") {
	auto d = Refract();
	Set(d, "distance", 0.);
	Same(Draw(d), Source(d));
}
TEST_CASE("Refract normal blue stays uncentered and perspective uses original UV", "[source_refract]") {
	auto d = Refract();
	Set(d, "perspective", 3.);
	Set(d, "height", .1);
	Same(Draw(d), Oracle(Source(d), NORMAL, 1, .1, .25, .5, 3.));
}
TEST_CASE("Refract mapped height uses mean RGB rather than alpha or luminance", "[source_refract]") {
	auto d = Refract();
	std::erase_if(d.Nodes[0].Values, [](const AuthoredValue &v) { return v.Port == "height"; });
	Set(d, "height_mapped", true);
	Set(d, "height_map_range", Vector2{.1, .7});
	d.Nodes.push_back(Solid("map", {1, 1}, {255, 0, 0, 255}));
	d.Links.push_back({"map", "surface_out", "generator", "height_map"});
	Same(Draw(d), Oracle(Source(d), NORMAL, 1, .3, .25, .5));
}
TEST_CASE("Refract mapped distance preserves source assignment to depth multiplier", "[source_refract]") {
	auto d = Refract();
	std::erase_if(d.Nodes[0].Values, [](const AuthoredValue &v) { return v.Port == "distance"; });
	Set(d, "distance_mapped", true);
	Set(d, "distance_map_range", Vector2{.1, .7});
	Set(d, "height", 9.);
	d.Nodes.push_back(Solid("map", {1, 1}, {255, 0, 0, 255}));
	d.Links.push_back({"map", "surface_out", "generator", "distance_map"});
	Same(Draw(d), Oracle(Source(d), NORMAL, 1, .3, .1, .5));
}
TEST_CASE("Refract mapped IOR takes physical numeric endpoints before synthetic range", "[source_refract]") {
	auto d = Refract();
	Set(d, "ior_mapped", true);
	Set(d, "ior", ArrayValue{ValueType::Scalar, {0., 1.}});
	Set(d, "ior_map_range", Vector2{9, 9});
	d.Nodes.push_back(Solid("map", {1, 1}, {255, 128, 0, 255}));
	d.Links.push_back({"map", "surface_out", "generator", "ior_map"});
	Same(Draw(d), Oracle(Source(d), NORMAL, 1, 0, .25, (1 + 128 / 255.) / 3));
}
TEST_CASE("Refract absent optional control maps use first mapped endpoints", "[source_refract]") {
	auto d = Refract();
	for (const char *port : {"height", "distance", "ior"}) {
		Set(d, std::string(port) + "_mapped", true);
	}
	Same(Draw(d), Oracle(Source(d), NORMAL, 1, 0, .25, .5));
}
TEST_CASE("Refract absent required sampler state rejects atomically", "[source_refract]") {
	for (const char *port : {"normal_map", "depth_map"}) {
		auto d = Refract();
		Remove(d, port);
		Refuse(
			d,
			Status::UnsupportedExecution,
			port,
			Limits::MaximumEvaluationBytes,
			"unobserved sampler binding"
		);
	}
}
TEST_CASE("Refract inactive scalar Active copies without normal or depth binding", "[source_refract]") {
	auto d = Refract();
	Set(d, "active", false);
	Remove(d, "normal_map");
	Remove(d, "depth_map");
	Same(Draw(d), Source(d));
}
TEST_CASE("Refract zero normal has a source undefined normalization diagnostic", "[source_refract]") {
	auto d = Refract();
	Remove(d, "normal_map");
	d.Nodes.push_back(
		{"half",
		 "pc.interpret_matrix",
		 "",
		 {},
		 {{"matrix", MatrixValue{1, 1, {.5}}},
		  {"dimension", Vector2{1, 1}},
		  {"dimension_unit", EnumValue{0}},
		  {"attribute_color_depth", EnumValue{5}}}}
	);
	d.Nodes.push_back(
		{"zero",
		 "pc.matrix_color_apply",
		 "",
		 {},
		 {{"matrix", MatrixValue{3, 3, {1, 0, 0, 0, 1, 0, 0, 0, 0}}},
		  {"attribute_color_depth", EnumValue{5}}}}
	);
	d.Links.push_back({"half", "surface_out", "zero", "surface_in"});
	d.Links.push_back({"zero", "surface_out", "generator", "normal_map"});
	Refuse(
		d,
		Status::UnsupportedExecution,
		"normal_map",
		Limits::MaximumEvaluationBytes,
		"normalization is undefined"
	);
}
TEST_CASE("Refract UV alpha is inert and final sample does not remap through UV", "[source_refract]") {
	auto d = Refract();
	d.Nodes.push_back(Solid("uv", {1, 1}, {255, 0, 0, 0}));
	d.Links.push_back({"uv", "surface_out", "generator", "uv_map"});
	for (double mix : {0., 1., 2.}) {
		Set(d, "uv_mix", mix);
		Same(Draw(d), Oracle(Source(d), NORMAL, 1, 0, .25, .5));
	}
}
TEST_CASE("Refract every interpolation route retains uniform RGBA pixels", "[source_refract]") {
	auto d = Refract();
	d.Nodes[1] = Solid("source", {3, 2}, {91, 37, 203, 255});
	for (int64_t mode : {1, 2, 3, 4, 5, 6}) {
		Set(d, "interpolate", EnumValue{mode});
		Same(Draw(d), Source(d));
	}
}
TEST_CASE("Refract oversample Empty Black and Clamp distinguish shifted pixels", "[source_refract]") {
	auto d = Refract();
	Set(d, "distance", 100.);
	for (int64_t mode : {1, 2, 3}) {
		Set(d, "oversample", EnumValue{mode});
		const auto image = Draw(d);
		for (size_t i = 0; i < image.Pixels.size(); i += 4) {
			CHECK(image.Pixels[i] == 0);
			CHECK(image.Pixels[i + 1] == (mode == 3 ? 255 : 0));
			CHECK(image.Pixels[i + 3] == (mode == 1 ? 0 : 255));
		}
	}
}
TEST_CASE("Refract repeat modes retain uniform surfaces including CleanEdge", "[source_refract]") {
	auto d = Refract();
	d.Nodes[1] = Solid("source", {2, 2}, {19, 41, 71, 255});
	Set(d, "distance", 10.);
	for (int64_t interpolation : {1, 6})
		for (int64_t repeat : {4, 8, 12}) {
			Set(d, "interpolate", EnumValue{interpolation});
			Set(d, "oversample", EnumValue{repeat});
			Same(Draw(d), Source(d));
		}
}
TEST_CASE("Refract Mix zero and channel zero preserve the original surface", "[source_refract]") {
	auto d = Refract();
	Set(d, "mix", 0.);
	Same(Draw(d), Source(d));
	Set(d, "mix", 1.);
	Set(d, "channel", int64_t{0});
	Same(Draw(d), Source(d));
}
TEST_CASE(
	"Refract mask inversion alpha-only and feather execute common processor controls", "[source_refract]"
) {
	auto d = Refract();
	d.Nodes.push_back(Solid("mask", {8, 4}, {0, 0, 0, 255}));
	d.Links.push_back({"mask", "surface_out", "generator", "mask"});
	Same(Draw(d), Source(d));
	Set(d, "invert_mask", true);
	Same(Draw(d), Oracle(Source(d), NORMAL, 1, 0, .25, .5));
	Set(d, "invert_mask", false);
	Set(d, "mask_alpha_only", true);
	const auto original = Source(d);
	const auto refracted = Oracle(original, NORMAL, 1, 0, .25, .5);
	Same(Draw(d), refracted);
	Set(d, "mask_feather", 2.);
	Same(Draw(d), FeatheredOpaqueMaskOracle(original, refracted));
}
TEST_CASE("Refract seven typed formats regenerate stable metadata", "[source_refract]") {
	for (int64_t depth = 2; depth <= 8; ++depth) {
		auto d = Refract();
		Set(d, "attribute_color_depth", EnumValue{depth});
		const auto image = Draw(d);
		REQUIRE(image.Format == *SourceSurfaceFormat(depth));
		CHECK(image.Hash == SurfaceHash(image));
		Same(image, Draw(d));
	}
}
TEST_CASE("Refract source red-channel safe draw bypasses absent samplers", "[source_refract]") {
	for (int64_t depth = 6; depth <= 8; ++depth) {
		auto d = Refract();
		Remove(d, "normal_map");
		Remove(d, "depth_map");
		d.Nodes[1] = Solid("source", {2, 2}, {64, 99, 128, 255});
		d.Nodes[1].Values.push_back({"attribute_color_depth", EnumValue{depth}});
		Set(d, "attribute_color_depth", EnumValue{3});
		const auto out = Draw(d);
		for (size_t i = 0; i < out.Pixels.size(); i += 4) {
			CHECK(out.Pixels[i] == 64);
			CHECK(out.Pixels[i + 1] == 64);
			CHECK(out.Pixels[i + 2] == 64);
			CHECK(out.Pixels[i + 3] == 255);
		}
	}
}
TEST_CASE("Refract physical source numeric links preserve processor rows", "[source_refract]") {
	auto d = Refract();
	d.Nodes.push_back(Array("ior_values", ValueType::Scalar, .5, 2.));
	d.Links.push_back({"ior_values", "array", "generator", "ior"});
	Plan plan;
	Diagnostic diag;
	REQUIRE(Compile(d, plan, diag) == Status::Ok);
	ImageArray rows;
	const auto status = EvaluateArray(d, plan, "out", {}, rows, diag);
	INFO(diag.Message);
	REQUIRE(status == Status::Ok);
	REQUIRE(rows.Images.size() == 2);
	Same(rows.Images[0], Oracle(Source(d), NORMAL, 1, 0, .25, .5));
	Same(rows.Images[1], Source(d));
}
TEST_CASE("Refract float source surface getter projects width and height rows", "[source_refract]") {
	auto d = Refract();
	d.Nodes.push_back(Solid("value_surface", {1, 2}, {0, 0, 0, 255}));
	d.Links.push_back({"value_surface", "surface_out", "generator", "ior"});
	Plan p;
	Diagnostic diag;
	REQUIRE(Compile(d, p, diag) == Status::Ok);
	ImageArray rows;
	const auto status = EvaluateArray(d, p, "out", {}, rows, diag);
	INFO(diag.Message);
	REQUIRE(status == Status::Ok);
	REQUIRE(rows.Images.size() == 2);
	Same(rows.Images[0], Source(d));
	Same(rows.Images[1], Source(d));
}
TEST_CASE(
	"Refract native persistence and inherited instances regenerate source controls", "[source_refract]"
) {
	auto d = Refract();
	Set(d, "perspective", 1.5);
	const auto expected = Draw(d);
	Document restored;
	Diagnostic diag;
	REQUIRE(Read(Write(d), restored, diag) == Status::Ok);
	Same(Draw(restored), expected);
	Node instance{"instance", "pc.refract", "", {}, {}};
	instance.InstanceBase = "generator";
	d.Nodes.push_back(instance);
	d.Outputs = {{"out", "instance", "surface_out"}};
	Same(Draw(d), expected);
}
TEST_CASE(
	"Refract selected public image variant and value-only rejection preserve identity", "[source_refract]"
) {
	auto d = Refract();
	Plan p;
	Diagnostic diag;
	REQUIRE(Compile(d, p, diag) == Status::Ok);
	StatefulEvaluationResult result;
	REQUIRE(EvaluateStateful(d, p, "out", {}, result, diag) == Status::Ok);
	const auto *image = std::get_if<Image>(&result.Output);
	REQUIRE(image);
	Same(*image, Draw(d));
	EvaluatedValue value{"prior", 42., std::nullopt};
	const auto prior = value;
	CHECK(EvaluateValue(d, p, "out", {}, value, diag) == Status::InvalidOutput);
	CHECK(value == prior);
}
TEST_CASE("Refract later large source rows preflight work before first output", "[source_refract]") {
	auto d = Refract();
	d.Nodes.push_back(Solid("large", {256, 64}, {1, 2, 3, 255}));
	Node rows{"rows", "value.array", "", {}, {}};
	rows.DynamicInputs = {
		{"first", ValueType::Image, std::nullopt}, {"later", ValueType::Image, std::nullopt}
	};
	d.Nodes.push_back(rows);
	Remove(d, "surface_in");
	d.Links.push_back({"source", "surface_out", "rows", "first"});
	d.Links.push_back({"large", "surface_out", "rows", "later"});
	d.Links.push_back({"rows", "array", "generator", "surface_in"});
	Refuse(d, Status::LimitExceeded, "surface_in", Limits::MaximumEvaluationBytes, "whole-array work");
}
TEST_CASE("Refract admission preserves a previous public result at a tight byte cap", "[source_refract]") {
	Refuse(Refract(), Status::LimitExceeded, "", 1);
}
TEST_CASE("Refract CleanEdge exact checkerboard does not invent a diagonal slice", "[source_refract]") {
	using namespace engine::imagegraph::detail;
	const Rgba black{0, 0, 0, 1}, white{1, 1, 1, 1};
	const auto pixel = source_refract_clean::Slice(
		{.8, .8},
		{1, 1},
		{1, 1},
		white,
		black,
		white,
		white,
		black,
		white,
		white,
		black,
		white,
		black,
		white,
		white,
		white,
		white
	);
	CHECK(pixel == Rgba{-1, -1, -1, -1});
}

TEST_CASE(
	"Refract all oversample policies retain exact Empty Black and repeated CleanEdge pixels",
	"[source_refract]"
) {
	auto d = Refract();
	d.Nodes[1] = Solid("source", {2, 2}, {19, 41, 71, 255});
	d.Nodes[2] = Solid("normal", {1, 1}, {255, 255, 255, 255});
	Set(d, "distance", 100.);
	for (int64_t interpolation : {1, 2, 3, 4, 5, 6})
		for (int64_t mode : {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12}) {
			Set(d, "interpolate", EnumValue{interpolation});
			Set(d, "oversample", EnumValue{mode});
			const auto image = Draw(d);
			const bool empty = mode == 1 || mode == 5 || mode == 6 || mode == 9 || mode == 10,
					   black = mode == 2 || mode == 7 || mode == 11;
			for (size_t at = 0; at < image.Pixels.size(); at += 4) {
				CHECK(image.Pixels[at] == (empty || black ? 0 : 19));
				CHECK(image.Pixels[at + 1] == (empty || black ? 0 : 41));
				CHECK(image.Pixels[at + 2] == (empty || black ? 0 : 71));
				CHECK(image.Pixels[at + 3] == (empty ? 0 : 255));
			}
		}
}
TEST_CASE(
	"Refract CleanEdge five literal slicing branches retain independent source-equation goldens",
	"[source_refract]"
) {
	using namespace engine::imagegraph::detail;
	constexpr std::array<std::array<int, 14>, 5> patterns{
		{{0, 0, 0, 0, 0, 0, 1, 0, 1, 1, 0, 0, 0, 0},
		 {0, 0, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 1, 0},
		 {0, 0, 0, 0, 0, 1, 0, 0, 1, 0, 0, 0, 0, 0},
		 {0, 0, 0, 0, 0, 1, 0, 1, 1, 0, 0, 0, 0, 0},
		 {0, 1, 0, 0, 0, 1, 0, 0, 1, 0, 0, 0, 0, 0}}
	};
	for (const auto &pattern : patterns) {
		std::array<Rgba, 14> n{};
		for (size_t i = 0; i < n.size(); ++i)
			n[i] = {double(pattern[i]), double(pattern[i]), double(pattern[i]), 1};
		CHECK(
			source_refract_clean::Slice(
				{.99, .99},
				{1, 1},
				{1, 1},
				n[0],
				n[1],
				n[2],
				n[3],
				n[4],
				n[5],
				n[6],
				n[7],
				n[8],
				n[9],
				n[10],
				n[11],
				n[12],
				n[13]
			) == Rgba{1, 1, 1, 1}
		);
	}
}
TEST_CASE("Refract compiled CleanEdge diagonal covers a corner that Pixel leaves black", "[source_refract]") {
	auto d = Refract();
	MatrixValue triangle{5, 5, {}};
	for (size_t y = 0; y < 5; ++y)
		for (size_t x = 0; x < 5; ++x)
			triangle.Values.push_back(x + y >= 5 ? 1. : 0.);
	d.Nodes[1] = {
		"source",
		"pc.interpret_matrix",
		"",
		{},
		{{"matrix", triangle},
		 {"dimension", Vector2{5, 5}},
		 {"dimension_unit", EnumValue{0}},
		 {"mode", EnumValue{1}},
		 {"palette", ArrayValue{ValueType::Colour, {Colour{0, 0, 0, 255}, Colour{255, 255, 255, 255}}}}}
	};
	d.Nodes[2] = Solid("normal", {1, 1}, {255, 255, 255, 255});
	Set(d, "distance", -.27193547379766747);
	const auto pixel = Draw(d);
	CHECK(pixel.Pixels[48] == 0);
	Set(d, "interpolate", EnumValue{6});
	const auto clean = Draw(d);
	CHECK(clean.Pixels[48] == 255);
	CHECK(clean.Pixels[49] == 255);
	CHECK(clean.Pixels[50] == 255);
	CHECK(clean.Pixels[51] == 255);
}
TEST_CASE("Refract catalogue recovers final Clamp default and CleanEdge choice", "[source_refract]") {
	const auto *entry = FindCatalogueEntry("pc.refract");
	REQUIRE(entry);
	const auto *oversample = FindCatalogueInput(*entry, "oversample"),
			   *interpolate = FindCatalogueInput(*entry, "interpolate");
	REQUIRE(oversample);
	REQUIRE(interpolate);
	CHECK(oversample->Default == "e 3");
	CHECK(CatalogueChoiceCount(*interpolate) == 7);
}
TEST_CASE(
	"Refract private disabled first row admits original later source work before copying", "[source_refract]"
) {
	const auto *entry = FindCatalogueEntry("pc.refract");
	const auto executor = engine::imagegraph::detail::FindExecutor("pc.refract");
	REQUIRE(entry);
	REQUIRE(executor);
	Node authored{"generator", "pc.refract", "", {}, {}};
	EvaluationRequest request;
	engine::imagegraph::detail::NodeContext context(authored, *entry, request);
	context.ByteBudget = Limits::MaximumEvaluationBytes;
	context.ProcessorCount = 2;
	const auto small = imagegraph_test::MakeImage(1, 1, {1, 2, 3, 255});
	ImageArray original;
	original.Images.push_back(small);
	original.Images.push_back(imagegraph_test::MakeImage(256, 64, std::vector<uint8_t>(256 * 64 * 4, 255)));
	context.Images = {{"surface_in", &small}};
	context.ImageArrays = {{"surface_in", &original}};
	context.Values = {{"active", false}, {"mask_feather", 0.}};
	CHECK_FALSE(executor(context));
	CHECK(context.FailureCode == Status::LimitExceeded);
	CHECK(context.FailureMessage.find("whole-array work") != std::string::npos);
	CHECK(context.OutputImages.empty());
}
TEST_CASE(
	"Refract all processor array schedules preserve independent scalar row oracles", "[source_refract]"
) {
	for (int64_t mode = 0; mode < 4; ++mode) {
		auto d = Refract();
		Set(d, "attribute_array_process", EnumValue{mode});
		d.Nodes.push_back(Array("iors", ValueType::Scalar, .5, 2.));
		Node distances{"distances", "pc.array", "", {}, {}, {}};
		distances.DynamicInputs = {
			{"input_0", ValueType::Scalar, .1},
			{"input_1", ValueType::Scalar, .2},
			{"input_2", ValueType::Scalar, .3}
		};
		d.Nodes.push_back(distances);
		d.Links.push_back({"iors", "array", "generator", "ior"});
		d.Links.push_back({"distances", "array", "generator", "distance"});
		Plan plan;
		Diagnostic diag;
		REQUIRE(Compile(d, plan, diag) == Status::Ok);
		ImageArray rows;
		const auto status = EvaluateArray(d, plan, "out", {}, rows, diag);
		INFO(diag.Message);
		REQUIRE(status == Status::Ok);
		const size_t count = mode < 2 ? 3 : 6;
		REQUIRE(rows.Images.size() == count);
		for (size_t row = 0; row < count; ++row) {
			// Expand follows physical Distance10 then IOR11. Source inverse reverses
			// the complete 18-slot suffix table, repeating first endpoints for these late slots.
			const size_t etaIndex = mode == 0	? row % 2
									: mode == 1 ? std::min(row, size_t{1})
									: mode == 2 ? row % 2
												: 0;
			const size_t distanceIndex = mode < 2 ? row : mode == 2 ? row / 2 : 0;
			Same(
				rows.Images[row],
				Oracle(Source(d), NORMAL, 1, 0, .1 * (distanceIndex + 1), etaIndex ? 2. : .5)
			);
		}
	}
}
TEST_CASE(
	"Refract UV actually selects normal texels while base lookup retains its own coordinates",
	"[source_refract]"
) {
	auto d = Refract();
	d.Nodes[2] = {
		"normal",
		"pc.interpret_matrix",
		"",
		{},
		{{"matrix", MatrixValue{2, 1, {0, 1}}},
		 {"palette", ArrayValue{ValueType::Colour, {NORMAL, Colour{128, 128, 255, 255}}}},
		 {"mode", EnumValue{1}},
		 {"dimension", Vector2{2, 1}},
		 {"dimension_unit", EnumValue{0}}}
	};
	const auto unmodified = Draw(d);
	CHECK(unmodified.Pixels[7 * 4] == 168);
	d.Nodes.push_back(Solid("uv", {1, 1}, {0, 0, 0, 0}));
	d.Links.push_back({"uv", "surface_out", "generator", "uv_map"});
	const auto mapped = Draw(d);
	Same(mapped, Oracle(Source(d), NORMAL, 1, 0, .25, .5));
	CHECK(mapped.Pixels[7 * 4] == 144);
}
TEST_CASE("Refract inherited animated IOR resolves source endpoint ticks", "[source_refract]") {
	auto d = Refract();
	d.Keyframes = {{"generator", "ior", 0, .5}, {"generator", "ior", 2, 2.}};
	d.Nodes[0].SourceAnimatedInputs = {"ior"};
	Node instance{"instance", "pc.refract", "", {}, {}};
	instance.InstanceBase = "generator";
	d.Nodes.push_back(instance);
	d.Outputs[0].NodeId = "instance";
	Plan plan;
	Diagnostic diag;
	REQUIRE(Compile(d, plan, diag) == Status::Ok);
	for (uint64_t tick : {0, 2}) {
		EvaluationRequest request;
		request.Tick = tick;
		Image output;
		const auto status = Evaluate(d, plan, "out", request, output, diag);
		INFO(diag.Message);
		REQUIRE(status == Status::Ok);
		Same(output, Oracle(Source(d), NORMAL, 1, 0, .25, tick == 0 ? .5 : 2.));
	}
}
TEST_CASE("Refract inherited project depth remains a typed processor output", "[source_refract]") {
	auto d = Refract();
	Set(d, "attribute_color_depth", EnumValue{1});
	d.Project = ProjectSettings{.ColorDepth = 3};
	const auto image = Draw(d);
	CHECK(image.Format == SurfaceFormat::RGBA32Float);
	CHECK(image.Hash == SurfaceHash(image));
}
TEST_CASE("Refract absent mask leaves source feather inert", "[source_refract]") {
	auto d = Refract();
	Set(d, "mask_feather", 1e12);
	Same(Draw(d), Oracle(Source(d), NORMAL, 1, 0, .25, .5));
}
TEST_CASE("Refract later feather rows admit Gaussian work before first output", "[source_refract]") {
	auto d = Refract();
	d.Nodes.push_back(Solid("mask", {1, 1}, {255, 255, 255, 255}));
	d.Links.push_back({"mask", "surface_out", "generator", "mask"});
	d.Nodes.push_back(Array("feathers", ValueType::Scalar, 0., 1e12));
	d.Links.push_back({"feathers", "array", "generator", "mask_feather"});
	Refuse(
		d, Status::LimitExceeded, "mask_feather", Limits::MaximumEvaluationBytes, "whole-array feather work"
	);
}
