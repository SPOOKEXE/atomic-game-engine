// Pinned source format boundaries and bounded CPU scratch behavior.
#include "../src/nodes/Blur.hpp"
#include "NodeHarness.hpp"

#include <engine/testing/Suite.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cmath>

TEST_SUITE_ID("engine.imagegraph.node_blur_hdr")

namespace {
	using namespace engine::imagegraph;
	Image BlurHdrFixture(SurfaceFormat format, SurfacePixel pixel, uint32_t width = 1, uint32_t height = 1) {
		const auto layout = CheckedSurfaceLayout(width, height, format, Limits::MaximumOutputBytes);
		REQUIRE(layout);
		Image source{width, height, std::vector<uint8_t>(layout->Bytes), 0, format};
		for (uint32_t y = 0; y < height; ++y)
			for (uint32_t x = 0; x < width; ++x)
				REQUIRE(StoreSurfacePixel(source, x, y, pixel));
		return source;
	}
	void BlurHdrPixel(const Image &image, SurfacePixel expected, double margin = 0) {
		SurfacePixel actual;
		REQUIRE(LoadSurfacePixel(image, 0, 0, actual));
		for (size_t channel = 0; channel < 4; ++channel)
			CHECK(actual[channel] == Catch::Approx(expected[channel]).margin(margin));
	}
	Status ReplayBlur(
		std::string_view type,
		const Image &source,
		std::vector<AuthoredValue> values,
		Image &out,
		Diagnostic &diagnostic
	) {
		Document graph;
		graph.FormatVersion = 9;
		graph.Nodes = {
			{"capture", "image.captured", "", {}, {{"source_id", std::string{"hdr"}}}},
			{"blur", std::string(type), "", {}, std::move(values)}
		};
		graph.Links = {{"capture", "image", "blur", "surface_in"}};
		graph.Outputs = {{"pixels", "blur", "surface_out"}};
		Document restored;
		const auto read = Read(Write(graph), restored, diagnostic);
		if (read != Status::Ok) return read;
		CHECK(restored == graph);
		Plan plan;
		const auto compile = Compile(restored, plan, diagnostic);
		if (compile != Status::Ok) return compile;
		RequestImageSource capture{"hdr", source};
		EvaluationRequest request;
		request.ImageSources = std::span<const RequestImageSource>(&capture, 1);
		return Evaluate(restored, plan, "pixels", request, out, diagnostic);
	}
} // namespace

TEST_CASE(
	"persisted Gaussian uses input format passes before selected final "
	"conversion",
	"[imagegraph][blur_hdr]"
) {
	using namespace engine::imagegraph;
	for (auto format : {SurfaceFormat::RGBA32Float, SurfaceFormat::RGBA16Float}) {
		const Image source = BlurHdrFixture(format, {2, -0.5, 0.25, 1});
		Image output;
		Diagnostic diagnostic;
		const auto status = ReplayBlur(
			"pc.blur", source, {{"size", 1.0}, {"attribute_color_depth", EnumValue{0}}}, output, diagnostic
		);
		INFO(diagnostic.Message);
		REQUIRE(status == Status::Ok);
		CHECK(output.Format == format);
		// Each source pass divides RGB by alpha weight1 plus the shader's1e-5 seed.
		const double factor = 1.0 / ((1.0 + 1e-5) * (1.0 + 1e-5));
		BlurHdrPixel(
			output,
			{2 * factor, -0.5 * factor, 0.25 * factor, 1},
			format == SurfaceFormat::RGBA16Float ? 0.001 : 1e-6
		);
		REQUIRE(
			ReplayBlur(
				"pc.blur",
				source,
				{{"size", 1.0}, {"attribute_color_depth", EnumValue{3}}},
				output,
				diagnostic
			) == Status::Ok
		);
		CHECK(output.Format == SurfaceFormat::RGBA8Unorm);
		CHECK(output.Pixels == std::vector<uint8_t>{255, 0, 64, 255});
	}
}

TEST_CASE(
	"Directional and Zoom force input format while SmoothDirectional "
	"converts through RGBA8",
	"[imagegraph][blur_hdr]"
) {
	using namespace engine::imagegraph;
	const Image source = BlurHdrFixture(SurfaceFormat::RGBA32Float, {2, -0.5, 0.25, 1});
	for (auto type : {"pc.blur_directional", "pc.blur_zoom"}) {
		Image output;
		Diagnostic diagnostic;
		const auto status = ReplayBlur(
			type, source, {{"strength", 0.0}, {"attribute_color_depth", EnumValue{3}}}, output, diagnostic
		);
		INFO(diagnostic.Message);
		REQUIRE(status == Status::Ok);
		CHECK(output.Format == source.Format);
		BlurHdrPixel(output, {2, -0.5, 0.25, 1}, 1e-6);
	}
	const auto smooth = imagegraph_test::RunNode(
		"pc.blur_directional",
		{{"surface_in", &source}},
		{{"strength", 0.0}, {"smooth_blur", 1.0}, {"attribute_color_depth", EnumValue{5}}}
	);
	INFO(smooth.Message);
	REQUIRE(smooth.Ok);
	CHECK(smooth.Output().Format == SurfaceFormat::RGBA8Unorm);
	CHECK(smooth.Output().Pixels == std::vector<uint8_t>{255, 0, 64, 255});
	const auto step = imagegraph_test::RunNode(
		"pc.blur_zoom",
		{{"surface_in", &source}},
		{{"strength", 0.0}, {"mode", EnumValue{1}}, {"attribute_color_depth", EnumValue{3}}}
	);
	REQUIRE(step.Ok);
	CHECK(step.Output().Format == source.Format);
	CHECK(step.Output().Pixels == source.Pixels);
}

TEST_CASE(
	"Gaussian gamma abs and signed Directional gamma remain different "
	"shader domains",
	"[imagegraph][blur_hdr]"
) {
	using namespace engine::imagegraph;
	const Image source = BlurHdrFixture(SurfaceFormat::RGBA32Float, {-2, -0.5, 1, 1});
	const auto gaussian = imagegraph_test::RunNode(
		"pc.blur",
		{{"surface_in", &source}},
		{{"size", 1.0}, {"gamma_correction", true}, {"attribute_color_depth", EnumValue{0}}}
	);
	REQUIRE(gaussian.Ok);
	const double factor = std::pow(1.0 + 1e-5, -2.0 / 2.2);
	BlurHdrPixel(gaussian.Output(), {2 * factor, 0.5 * factor, factor, 1}, 1e-6);
	Image sentinel = imagegraph_test::MakeImage(1, 1, {5, 6, 7, 8});
	const Image previous = sentinel;
	Diagnostic diagnostic;
	CHECK(
		ReplayBlur(
			"pc.blur_directional",
			source,
			{{"strength", 0.0}, {"gamma_correction", true}},
			sentinel,
			diagnostic
		) == Status::InvalidValue
	);
	CHECK(sentinel == previous);
}

TEST_CASE(
	"Bloom branches and Average retain source normalized intermediate "
	"boundaries",
	"[imagegraph][blur_hdr]"
) {
	using namespace engine::imagegraph;
	const Image source = BlurHdrFixture(SurfaceFormat::RGBA32Float, {2, -0.5, 0.25, 1});
	for (int64_t type : {0, 1, 2}) {
		const auto bloom = imagegraph_test::RunNode(
			"pc.bloom",
			{{"surface_in", &source}},
			{{"type", EnumValue{type}},
			 {"size", 0.0},
			 {"strength", 0.0},
			 {"attribute_color_depth", EnumValue{5}}}
		);
		INFO(bloom.Message);
		REQUIRE(bloom.Ok);
		CHECK(bloom.Output().Format == SurfaceFormat::RGBA8Unorm);
		CHECK(bloom.Output("bloom_mask").Format == SurfaceFormat::RGBA8Unorm);
		CHECK(bloom.Output().Pixels == std::vector<uint8_t>{255, 0, 64, 255});
	}
	const Image square = BlurHdrFixture(SurfaceFormat::RGBA32Float, {2, -0.5, 0.25, 1}, 2, 2);
	const auto average = imagegraph_test::RunNode(
		"pc.average", {{"surface_in", &square}}, {{"attribute_color_depth", EnumValue{0}}}
	);
	REQUIRE(average.Ok);
	CHECK(average.Output().Format == SurfaceFormat::RGBA32Float);
	BlurHdrPixel(average.Output(), {1, 0, 64.0 / 255.0, 1}, 1e-7);
	REQUIRE(average.OutputValue("color"));
	CHECK(*average.OutputValue("color") == Value{Colour{255, 0, 64, 255}});
}

TEST_CASE(
	"FXAA and channel assembly override requested HDR output with source RGBA8", "[imagegraph][blur_hdr]"
) {
	using namespace engine::imagegraph;
	const Image source = BlurHdrFixture(SurfaceFormat::RGBA32Float, {2, 0.5, 0.25, 1});
	const auto fxaa = imagegraph_test::RunNode(
		"pc.fxaa", {{"surface_in", &source}}, {{"attribute_color_depth", EnumValue{5}}}
	);
	INFO(fxaa.Message);
	REQUIRE(fxaa.Ok);
	CHECK(fxaa.Output().Format == SurfaceFormat::RGBA8Unorm);
	CHECK(fxaa.Output("mask").Format == SurfaceFormat::RGBA8Unorm);
	CHECK(fxaa.Output().Pixels == std::vector<uint8_t>{255, 128, 64, 255});
	const auto combined = imagegraph_test::RunNode(
		"pc.combine_rgb",
		{{"red", &source}, {"green", &source}, {"blue", &source}},
		{{"attribute_color_depth", EnumValue{5}}}
	);
	REQUIRE(combined.Ok);
	CHECK(combined.Output().Format == SurfaceFormat::RGBA8Unorm);
	CHECK(combined.Output().Pixels == std::vector<uint8_t>{255, 128, 64, 255});
}

TEST_CASE(
	"Gaussian scratch reservations overlap exactly and refuse one byte short", "[imagegraph][blur_hdr]"
) {
	using namespace engine::imagegraph;
	const Image source = BlurHdrFixture(SurfaceFormat::RGBA32Float, {2, -0.5, 0.25, 1});
	const auto *entry = FindCatalogueEntry("pc.blur");
	REQUIRE(entry);
	Node node{"blur", std::string(entry->Type), "", {}, {}};
	EvaluationRequest request;
	for (uint64_t limit : {uint64_t{56}, uint64_t{55}}) {
		detail::EvaluationBudget budget(limit);
		auto prior = budget.Reserve(16);
		REQUIRE(prior);
		{
			detail::NodeContext context(node, *entry, request, budget);
			context.ByteBudget = limit;
			detail::GaussianArgs args;
			args.Size = args.SizeHigh = 1;
			auto blurred = detail::GaussianBlur(context, source, args);
			CHECK(blurred.has_value() == (limit == 56));
			if (blurred) {
				CHECK(budget.Peak() == 56);
				CHECK(budget.Used() == 32);
			} else
				CHECK(context.FailureCode == Status::LimitExceeded);
		}
		CHECK(budget.Used() == 16);
	}
}

TEST_CASE(
	"Shadow typed outputs preserve HDR and inner crop converts its alpha "
	"through RGBA8",
	"[imagegraph][blur_hdr]"
) {
	using namespace engine::imagegraph;
	const Image source = BlurHdrFixture(SurfaceFormat::RGBA32Float, {2, -0.5, 0.25, 2});
	for (int64_t grow : {0, 1}) {
		const auto outer = imagegraph_test::RunNode(
			"pc.shadow",
			{{"surface_in", &source}},
			{{"attribute_color_depth", EnumValue{0}},
			 {"grow", grow},
			 {"blur", int64_t{0}},
			 {"shift", Vector2{0, 0}},
			 {"color", Colour{255, 0, 0, 255}},
			 {"strength", 0.0},
			 {"remove_original", false}}
		);
		INFO(outer.Message);
		REQUIRE(outer.Ok);
		CHECK(outer.Output().Format == source.Format);
		CHECK(outer.Output("shadow_only").Format == source.Format);
		// Source compositor multiplies every base component by its alpha, including
		// alpha itself.
		BlurHdrPixel(outer.Output(), {4, -1, 0.5, 4}, 1e-6);
	}
	const auto inner = imagegraph_test::RunNode(
		"pc.shadow",
		{{"surface_in", &source}},
		{{"attribute_color_depth", EnumValue{0}},
		 {"grow", int64_t{0}},
		 {"blur", int64_t{0}},
		 {"shift", Vector2{0, 0}},
		 {"color", Colour{255, 0, 0, 255}},
		 {"side", EnumValue{1}}}
	);
	INFO(inner.Message);
	REQUIRE(inner.Ok);
	BlurHdrPixel(inner.Output("shadow_only"), {1, 0, 0, 0});
}

TEST_CASE(
	"Directional and Zoom spectral gradients borrow keys while retaining "
	"HDR output",
	"[imagegraph][blur_hdr]"
) {
	using namespace engine::imagegraph;
	const Image source = BlurHdrFixture(SurfaceFormat::RGBA32Float, {2, 0.5, 0.25, 1});
	const Gradient gradient{0, {{0, Colour{255, 0, 0, 255}}, {1, Colour{255, 0, 0, 255}}}};
	const double magnitude = std::sqrt(4 + 0.25 + 0.0625);
	for (auto type : {"pc.blur_directional", "pc.blur_zoom"}) {
		const auto run = imagegraph_test::RunNode(
			type,
			{{"surface_in", &source}},
			{{"strength", 0.0},
			 {"fade_distance", false},
			 {"fade", false},
			 {"samples", int64_t{1}},
			 {"colorize", EnumValue{2}},
			 {"gradient", gradient},
			 {"intensity", 1.0},
			 {"attribute_color_depth", EnumValue{0}}}
		);
		INFO(run.Message);
		REQUIRE(run.Ok);
		BlurHdrPixel(run.Output(), {2 + magnitude, 0.5, 0.25, 1}, 1e-6);
	}
	const auto step = imagegraph_test::RunNode(
		"pc.blur_zoom",
		{{"surface_in", &source}},
		{{"strength", 0.0},
		 {"fade_distance", false},
		 {"fade", false},
		 {"mode", EnumValue{1}},
		 {"colorize", EnumValue{2}},
		 {"gradient", gradient},
		 {"attribute_color_depth", EnumValue{0}}}
	);
	REQUIRE(step.Ok);
	BlurHdrPixel(step.Output(), {2, 0, 0, 1});
}

TEST_CASE("Gaussian typed scratch respects each native storage format", "[imagegraph][blur_hdr]") {
	using namespace engine::imagegraph;
	for (int64_t depth = 2; depth <= 8; ++depth) {
		const auto format = SourceSurfaceFormat(depth);
		REQUIRE(format);
		const Image source = BlurHdrFixture(*format, {0.25, 0.5, 0.75, 1});
		const auto run = imagegraph_test::RunNode(
			"pc.blur", {{"surface_in", &source}}, {{"size", 1.0}, {"attribute_color_depth", EnumValue{0}}}
		);
		INFO(run.Message);
		REQUIRE(run.Ok);
		CHECK(run.Output().Format == *format);
		SurfacePixel before, after;
		REQUIRE(LoadSurfacePixel(source, 0, 0, before));
		REQUIRE(LoadSurfacePixel(run.Output(), 0, 0, after));
		const auto descriptor = DescribeSurfaceFormat(*format);
		REQUIRE(descriptor);
		if (!descriptor->FloatingPoint)
			CHECK(run.Output().Pixels == source.Pixels);
		else {
			const double factor = 1.0 / ((1 + 1e-5) * (1 + 1e-5));
			for (size_t channel = 0; channel < 3; ++channel)
				CHECK(after[channel] == Catch::Approx(before[channel] * factor).margin(0.001));
			CHECK(after[3] == 1);
		}
	}
}

TEST_CASE(
	"persisted normalized Bloom and HDR Average replay their actual "
	"catalogue routes",
	"[imagegraph][blur_hdr]"
) {
	using namespace engine::imagegraph;
	const Image source = BlurHdrFixture(SurfaceFormat::RGBA32Float, {2, -0.5, 0.25, 1}, 2, 2);
	Diagnostic diagnostic;
	Image output;
	for (int64_t type : {0, 1, 2}) {
		const auto status = ReplayBlur(
			"pc.bloom",
			source,
			{{"type", EnumValue{type}},
			 {"size", 0.0},
			 {"strength", 0.0},
			 {"attribute_color_depth", EnumValue{5}}},
			output,
			diagnostic
		);
		INFO(diagnostic.Message);
		REQUIRE(status == Status::Ok);
		CHECK(output.Format == SurfaceFormat::RGBA8Unorm);
		BlurHdrPixel(output, {1, 0, 64.0 / 255.0, 1}, 1e-7);
	}
	const auto status =
		ReplayBlur("pc.average", source, {{"attribute_color_depth", EnumValue{0}}}, output, diagnostic);
	INFO(diagnostic.Message);
	REQUIRE(status == Status::Ok);
	CHECK(output.Format == source.Format);
	BlurHdrPixel(output, {1, 0, 64.0 / 255.0, 1}, 1e-7);
}

TEST_CASE(
	"Zoom refuses a nonadvancing float sample loop before scratch allocation", "[imagegraph][blur_hdr]"
) {
	using namespace engine::imagegraph;
	const Image source = BlurHdrFixture(SurfaceFormat::RGBA32Float, {2, -0.5, 0.25, 1});
	Image output = source;
	const Image sentinel = output;
	Diagnostic diagnostic;
	const auto status =
		ReplayBlur("pc.blur_zoom", source, {{"samples", int64_t{10'000'000}}}, output, diagnostic);
	INFO(diagnostic.Message);
	REQUIRE(status == Status::LimitExceeded);
	CHECK(diagnostic.Port == "samples");
	CHECK(output == sentinel);
}
