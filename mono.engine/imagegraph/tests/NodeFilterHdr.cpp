// Typed filter oracles derived from the pinned shaders; device blend parity remains unverified.
#include "../src/SurfaceScratch.hpp"
#include "NodeHarness.hpp"

#include <engine/imagegraph/Document.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cmath>
#include <limits>

TEST_SUITE_ID("engine.imagegraph.node_filter_hdr")

namespace {
	using namespace engine::imagegraph;
	Image FloatFixture(
		SurfaceFormat format, uint32_t width, uint32_t height, std::span<const SurfacePixel> pixels
	) {
		const auto layout = CheckedSurfaceLayout(width, height, format, Limits::MaximumOutputBytes);
		REQUIRE(layout);
		Image image{width, height, std::vector<uint8_t>(layout->Bytes), 0, format};
		REQUIRE(pixels.size() == uint64_t(width) * height);
		for (uint32_t y = 0; y < height; ++y)
			for (uint32_t x = 0; x < width; ++x)
				REQUIRE(StoreSurfacePixel(image, x, y, pixels[y * width + x]));
		return image;
	}
	Document FilterGraph(std::string_view type, std::vector<AuthoredValue> properties = {}) {
		Document document;
		document.FormatVersion = 9;
		document.Nodes = {
			{"capture", "image.captured", "", {}, {{"source_id", std::string{"hdr"}}}},
			{"filter", std::string(type), "", {}, std::move(properties)}
		};
		document.Links = {{"capture", "image", "filter", "surface_in"}};
		document.Outputs = {{"pixels", "filter", "surface_out"}};
		return document;
	}
	Status
	PersistEvaluate(const Document &document, const Image &source, Image &output, Diagnostic &diagnostic) {
		Document restored;
		const auto read = Read(Write(document), restored, diagnostic);
		if (read != Status::Ok) return read;
		CHECK(restored == document);
		Plan plan;
		const auto compiled = Compile(restored, plan, diagnostic);
		if (compiled != Status::Ok) return compiled;
		RequestImageSource capture{"hdr", source};
		EvaluationRequest request;
		request.ImageSources = std::span<const RequestImageSource>(&capture, 1);
		return Evaluate(restored, plan, "pixels", request, output, diagnostic);
	}
	void
	PixelEquals(const Image &image, uint32_t x, uint32_t y, SurfacePixel expected, double tolerance = 0) {
		SurfacePixel actual;
		REQUIRE(LoadSurfacePixel(image, x, y, actual));
		for (size_t channel = 0; channel < 4; ++channel)
			CHECK(actual[channel] == Catch::Approx(expected[channel]).margin(tolerance));
	}
}

TEST_CASE("persisted HDR Invert retains signed channels and values above one", "[imagegraph][filter_hdr]") {
	using namespace engine::imagegraph;
	for (auto format : {SurfaceFormat::RGBA16Float, SurfaceFormat::RGBA32Float}) {
		const std::array<SurfacePixel, 1> samples{{{-0.25, 1.5, 2, 0.5}}};
		const Image source = FloatFixture(format, 1, 1, samples);
		auto document = FilterGraph("pc.invert", {{"attribute_color_depth", EnumValue{0}}});
		Image output;
		Diagnostic diagnostic;
		const auto status = PersistEvaluate(document, source, output, diagnostic);
		INFO(diagnostic.Message);
		REQUIRE(status == Status::Ok);
		CHECK(output.Format == format);
		PixelEquals(output, 0, 0, {1.25, -0.5, -1, 0.5});
		CHECK(output.Hash != 0);
		CHECK(source.Pixels == FloatFixture(format, 1, 1, samples).Pixels);
		// A concrete normalized output performs the source-selected conversion.
		document.Nodes[1].Values[0].Data = EnumValue{3};
		REQUIRE(PersistEvaluate(document, source, output, diagnostic) == Status::Ok);
		CHECK(output.Format == SurfaceFormat::RGBA8Unorm);
		CHECK(output.Pixels == std::vector<uint8_t>{255, 0, 0, 128});
	}
}

TEST_CASE("persisted Level then Invert keeps HDR through actual linked outputs", "[imagegraph][filter_hdr]") {
	using namespace engine::imagegraph;
	const std::array<SurfacePixel, 1> samples{{{0.25, 0.5, 0.75, 1}}};
	const Image source = FloatFixture(SurfaceFormat::RGBA32Float, 1, 1, samples);
	auto document =
		FilterGraph("pc.level", {{"attribute_color_depth", EnumValue{0}}, {"white_out", Vector2{-2, 4}}});
	document.Nodes.push_back({"invert", "pc.invert", "", {}, {{"attribute_color_depth", EnumValue{0}}}});
	document.Links.push_back({"filter", "surface_out", "invert", "surface_in"});
	document.Outputs[0] = {"pixels", "invert", "surface_out"};
	Image output;
	Diagnostic diagnostic;
	const auto status = PersistEvaluate(document, source, output, diagnostic);
	INFO(diagnostic.Message);
	REQUIRE(status == Status::Ok);
	PixelEquals(output, 0, 0, {1.5, 0, -1.5, 1});
	CHECK(output.Format == SurfaceFormat::RGBA32Float);
}

TEST_CASE("HDR inactive copy and mask channel restoration use typed samples", "[imagegraph][filter_hdr]") {
	using namespace engine::imagegraph;
	const std::array<SurfacePixel, 1> samples{{{-0.25, 1.5, 2, 0.5}}};
	const Image source = FloatFixture(SurfaceFormat::RGBA32Float, 1, 1, samples);
	const auto inactive = imagegraph_test::RunNode(
		"pc.invert", {{"surface_in", &source}}, {{"attribute_color_depth", EnumValue{3}}, {"active", false}}
	);
	REQUIRE(inactive.Ok);
	CHECK(inactive.Output().Format == source.Format);
	CHECK(inactive.Output().Pixels == source.Pixels);
	const std::array<SurfacePixel, 1> maskSample{{{0.5, 0.5, 0.5, 1}}};
	const Image mask = FloatFixture(SurfaceFormat::RGBA32Float, 1, 1, maskSample);
	const auto masked = imagegraph_test::RunNode(
		"pc.invert",
		{{"surface_in", &source}, {"mask", &mask}},
		{{"attribute_color_depth", EnumValue{0}}, {"mix", 0.5}, {"channel", int64_t{1}}}
	);
	INFO(masked.Message);
	REQUIRE(masked.Ok);
	PixelEquals(masked.Output(), 0, 0, {0.125, 1.5, 2, 0.5});
}

TEST_CASE(
	"Convolution identity works in each native format without byte reinterpretation",
	"[imagegraph][filter_hdr]"
) {
	using namespace engine::imagegraph;
	const MatrixValue identity{3, 3, {0, 0, 0, 0, 1, 0, 0, 0, 0}};
	for (int64_t depth = 2; depth <= 8; ++depth) {
		const auto format = SourceSurfaceFormat(depth);
		REQUIRE(format);
		const std::array<SurfacePixel, 1> samples{{{0.25, 0.5, 0.75, 1}}};
		const Image source = FloatFixture(*format, 1, 1, samples);
		const auto run = imagegraph_test::RunNode(
			"pc.convolution",
			{{"surface_in", &source}},
			{{"attribute_color_depth", EnumValue{0}}, {"kernel", identity}}
		);
		INFO(run.Message);
		REQUIRE(run.Ok);
		CHECK(run.Output().Format == *format);
		CHECK(run.Output().Pixels == source.Pixels);
	}
	const std::array<SurfacePixel, 1> samples{{{2, -0.5, 0.25, 1}}};
	const Image source = FloatFixture(SurfaceFormat::RGBA32Float, 1, 1, samples);
	const MatrixValue doubleWeight{3, 3, {0, 0, 0, 0, 2, 0, 0, 0, 0}};
	const auto run = imagegraph_test::RunNode(
		"pc.convolution",
		{{"surface_in", &source}},
		{{"attribute_color_depth", EnumValue{0}}, {"kernel", doubleWeight}, {"normalize", false}}
	);
	REQUIRE(run.Ok);
	PixelEquals(run.Output(), 0, 0, {4, -1, 0.5, 2});
	const MatrixValue negativeWeight{3, 3, {0, 0, 0, 0, -2, 0, 0, 0, 0}};
	const auto negative = imagegraph_test::RunNode(
		"pc.convolution",
		{{"surface_in", &source}},
		{{"attribute_color_depth", EnumValue{0}}, {"kernel", negativeWeight}, {"normalize", false}}
	);
	REQUIRE(negative.Ok);
	PixelEquals(negative.Output(), 0, 0, {-4, 1, -0.5, -2});
	const auto normalized = imagegraph_test::RunNode(
		"pc.convolution",
		{{"surface_in", &source}},
		{{"attribute_color_depth", EnumValue{0}}, {"kernel", negativeWeight}, {"normalize", true}}
	);
	REQUIRE(normalized.Ok);
	CHECK(normalized.Output().Pixels == source.Pixels);
}

TEST_CASE("Offset wraps signed HDR texels and retains exact RGBA8 routing", "[imagegraph][filter_hdr]") {
	using namespace engine::imagegraph;
	const std::array<SurfacePixel, 4> samples{
		{{-1, 2, 0, 1}, {3, -2, 0.5, 1}, {4, 1, 2, 1}, {-3, 0.5, 3, 1}}
	};
	const Image source = FloatFixture(SurfaceFormat::RGBA32Float, 2, 2, samples);
	const auto run = imagegraph_test::RunNode(
		"pc.offset",
		{{"surface_in", &source}},
		{{"attribute_color_depth", EnumValue{0}}, {"x_offset", 0.5}, {"y_offset", -0.5}}
	);
	REQUIRE(run.Ok);
	PixelEquals(run.Output(), 0, 0, samples[3]);
	PixelEquals(run.Output(), 1, 0, samples[2]);
	PixelEquals(run.Output(), 0, 1, samples[1]);
	PixelEquals(run.Output(), 1, 1, samples[0]);
}

TEST_CASE(
	"nonfinite shader results and half float overflow refuse atomic caller replacement",
	"[imagegraph][filter_hdr]"
) {
	using namespace engine::imagegraph;
	const std::array<SurfacePixel, 1> samples{{{-1, 1, 1, 1}}};
	const Image source = FloatFixture(SurfaceFormat::RGBA32Float, 1, 1, samples);
	Diagnostic diagnostic;
	Image sentinel = imagegraph_test::MakeImage(1, 1, {7, 8, 9, 10});
	const Image previous = sentinel;
	auto document = FilterGraph("pc.gamma_map", {{"attribute_color_depth", EnumValue{0}}});
	CHECK(PersistEvaluate(document, source, sentinel, diagnostic) == Status::InvalidValue);
	CHECK(sentinel == previous);
	document =
		FilterGraph("pc.level", {{"attribute_color_depth", EnumValue{4}}, {"white_out", Vector2{0, 1e10}}});
	CHECK(PersistEvaluate(document, source, sentinel, diagnostic) == Status::InvalidValue);
	CHECK(sentinel == previous);
}

TEST_CASE(
	"Convolution kernel and scratch retain prior live admission at exact and one byte short",
	"[imagegraph][filter_hdr]"
) {
	using namespace engine::imagegraph;
	const auto *entry = FindCatalogueEntry("pc.convolution");
	REQUIRE(entry);
	const auto executor = detail::FindExecutor(entry->Type);
	REQUIRE(executor);
	const uint64_t priorBytes = 88;
	const uint64_t storageBytes =
		entry->Outputs.size() * (sizeof(std::pair<std::string, Image>) + sizeof(AuthoredValue) +
								 (sizeof(std::pair<std::string, ImageArray>) +
								  sizeof(std::pair<std::string_view, SourceSocketDomain>)));
	const uint64_t required = priorBytes + storageBytes + 16 +
							  std::max(std::string_view{"surface_out"}.size(), std::string{}.capacity()) +
							  9 * sizeof(double);
	const std::array<SurfacePixel, 1> samples{{{2, -0.5, 0.25, 1}}};
	const Image source = FloatFixture(SurfaceFormat::RGBA32Float, 1, 1, samples);
	Value kernel = MatrixValue{3, 3, {0, 0, 0, 0, 1, 0, 0, 0, 0}};
	Value depth = EnumValue{0};
	for (uint64_t limit : {required, required - 1}) {
		detail::EvaluationBudget ledger(limit);
		auto prior = ledger.Reserve(priorBytes);
		REQUIRE(prior);
		{
			Node node{"convolution", std::string(entry->Type), "", {}, {}};
			EvaluationRequest request;
			detail::NodeContext context(node, *entry, request, ledger);
			context.ByteBudget = limit;
			context.Images = {{"surface_in", &source}};
			context.ValueViews = {{"kernel", &kernel}, {"attribute_color_depth", &depth}};
			const bool ok = executor(context) && context.FailureCode == Status::Ok;
			CHECK(ok == (limit == required));
			if (ok) {
				REQUIRE(context.OutputImages.size() == 1);
				CHECK(context.OutputImages[0].second.Pixels == source.Pixels);
				CHECK(ledger.Peak() == required);
			} else
				CHECK(context.FailureCode == Status::LimitExceeded);
		}
		CHECK(ledger.Used() == priorBytes);
	}
}

TEST_CASE(
	"typed scratch admissions reject one byte short and preview dimensions before allocation",
	"[imagegraph][filter_hdr]"
) {
	using namespace engine::imagegraph;
	const auto *entry = FindCatalogueEntry("pc.invert");
	REQUIRE(entry);
	Node node{"scratch", std::string(entry->Type), "", {}, {}};
	EvaluationRequest request;
	for (uint64_t limit : {uint64_t{80}, uint64_t{79}}) {
		detail::EvaluationBudget ledger(limit);
		auto prior = ledger.Reserve(64);
		REQUIRE(prior);
		{
			detail::NodeContext context(node, *entry, request, ledger);
			context.ByteBudget = limit;
			auto scratch = detail::MakeSurfaceScratch(context, 1, 1, SurfaceFormat::RGBA32Float, "scratch");
			CHECK(scratch.has_value() == (limit == 80));
			if (scratch) {
				CHECK(scratch->Data.Format == SurfaceFormat::RGBA32Float);
				CHECK(scratch->Data.Pixels.size() == 16);
				CHECK(ledger.Used() == 80);
			} else
				CHECK(context.FailureCode == Status::LimitExceeded);
		}
		CHECK(ledger.Used() == 64);
	}
	request.MaximumImageDimension = 128;
	detail::EvaluationBudget ledger(1024 * 1024);
	detail::NodeContext context(node, *entry, request, ledger);
	context.ByteBudget = 1024 * 1024;
	CHECK_FALSE(detail::MakeSurfaceScratch(context, 129, 1, SurfaceFormat::RGBA32Float, "scratch"));
	CHECK(context.FailureCode == Status::LimitExceeded);
	CHECK(ledger.Peak() == 0);
}

TEST_CASE(
	"Colorize mapped gradient reads HDR samples without an RGBA8 temporary", "[imagegraph][filter_hdr]"
) {
	using namespace engine::imagegraph;
	const std::array<SurfacePixel, 1> inputSample{{{0.5, 0.5, 0.5, 1}}};
	const std::array<SurfacePixel, 1> mapSample{{{2, -0.25, 1.5, 0.5}}};
	const Image source = FloatFixture(SurfaceFormat::RGBA32Float, 1, 1, inputSample);
	const Image map = FloatFixture(SurfaceFormat::RGBA32Float, 1, 1, mapSample);
	const auto run = imagegraph_test::RunNode(
		"pc.colorize",
		{{"surface_in", &source}, {"gradient_map", &map}},
		{{"attribute_color_depth", EnumValue{0}}, {"gradient_mapped", true}, {"keep_alpha", false}}
	);
	INFO(run.Message);
	REQUIRE(run.Ok);
	PixelEquals(run.Output(), 0, 0, mapSample[0]);
}

TEST_CASE(
	"PaletteShift wraps negative offsets and checks huge finite residual before narrowing",
	"[imagegraph][filter_hdr]"
) {
	using namespace engine::imagegraph;
	const std::array<SurfacePixel, 1> sample{{{1, 0, 0, 1}}};
	const Image source = FloatFixture(SurfaceFormat::RGBA32Float, 1, 1, sample);
	const ArrayValue palette{
		ValueType::Colour, {Colour{255, 0, 0, 255}, Colour{0, 255, 0, 255}, Colour{0, 0, 255, 255}}
	};
	const auto negative = imagegraph_test::RunNode(
		"pc.palette_shift",
		{{"surface_in", &source}},
		{{"attribute_color_depth", EnumValue{0}}, {"palette", palette}, {"shift", -1.0}}
	);
	REQUIRE(negative.Ok);
	PixelEquals(negative.Output(), 0, 0, {0, 0, 1, 1});
	const std::array<SurfacePixel, 3> expected{{{1, 0, 0, 1}, {0, 1, 0, 1}, {0, 0, 1, 1}}};
	for (const double shift :
		 {std::numeric_limits<double>::max(), -std::numeric_limits<double>::max(), 1e300}) {
		// Preserve the shader's wrapping arithmetic; a cancelled residual is not an ideal real modulo.
		const double residual = shift - 3.0 * std::floor(shift / 3.0);
		const auto run = imagegraph_test::RunNode(
			"pc.palette_shift",
			{{"surface_in", &source}},
			{{"attribute_color_depth", EnumValue{0}}, {"palette", palette}, {"shift", shift}}
		);
		INFO(residual);
		if (!std::isfinite(residual) || residual < 0 || residual >= 3) {
			CHECK_FALSE(run.Ok);
			CHECK(run.Code == Status::InvalidValue);
			CHECK(run.Port == "shift");
		} else {
			REQUIRE(run.Ok);
			PixelEquals(run.Output(), 0, 0, expected[static_cast<size_t>(residual)]);
		}
	}
}
