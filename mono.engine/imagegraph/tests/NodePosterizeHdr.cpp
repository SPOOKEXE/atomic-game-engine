#include "../src/nodes/PosterizeRange.hpp"
#include "NodeHarness.hpp"

#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

TEST_SUITE_ID("engine.imagegraph.node_posterize_hdr")

namespace {
	using namespace engine::imagegraph;
	Image
	PosterizeFixture(SurfaceFormat format, SurfacePixel pixel, uint32_t width = 1, uint32_t height = 1) {
		const auto layout = CheckedSurfaceLayout(width, height, format, Limits::MaximumOutputBytes);
		REQUIRE(layout);
		Image image{width, height, std::vector<uint8_t>(layout->Bytes), 0, format};
		for (uint32_t y = 0; y < height; ++y)
			for (uint32_t x = 0; x < width; ++x)
				REQUIRE(StoreSurfacePixel(image, x, y, pixel));
		return image;
	}
	Status ReplayPosterize(const Image &source, Image &output, Diagnostic &diagnostic) {
		Document graph;
		graph.FormatVersion = 9;
		graph.Nodes = {
			{"capture", "image.captured", "", {}, {{"source_id", std::string{"source"}}}},
			{"posterize",
			 "pc.posterize",
			 "",
			 {},
			 {{"use_palette", false},
			  {"use_global_range", false},
			  {"steps", int64_t{4}},
			  {"gamma", 1.0},
			  {"attribute_color_depth", EnumValue{0}}}}
		};
		graph.Links = {{"capture", "image", "posterize", "surface_in"}};
		graph.Outputs = {{"pixels", "posterize", "surface_out"}};
		Document restored;
		const auto read = Read(Write(graph), restored, diagnostic);
		if (read != Status::Ok) return read;
		CHECK(restored == graph);
		Plan plan;
		const auto compile = Compile(restored, plan, diagnostic);
		if (compile != Status::Ok) return compile;
		RequestImageSource capture{"source", source};
		EvaluationRequest request;
		request.ImageSources = std::span<const RequestImageSource>(&capture, 1);
		return Evaluate(restored, plan, "pixels", request, output, diagnostic);
	}
}

TEST_CASE(
	"Posterize local range retains raw byte extrema after RGBA8 conversion", "[imagegraph][posterize_hdr]"
) {
	using namespace engine::imagegraph;
	for (auto format : {SurfaceFormat::RGBA8Unorm, SurfaceFormat::RGBA16Float, SurfaceFormat::RGBA32Float}) {
		const Image source = PosterizeFixture(format, {0.5, 0.25, 0.125, 1});
		Image output;
		Diagnostic diagnostic;
		const auto status = ReplayPosterize(source, output, diagnostic);
		INFO(diagnostic.Message);
		REQUIRE(status == Status::Ok);
		CHECK(output.Format == format);
		SurfacePixel pixel;
		REQUIRE(LoadSurfacePixel(output, 0, 0, pixel));
		// Byte maxima128/64/32 and seed minimum1 make each normalized input clamp to zero.
		CHECK(pixel == SurfacePixel{1, 1, 1, 1});
	}
	const Image hdr = PosterizeFixture(SurfaceFormat::RGBA32Float, {2, 0.25, 0.5, 1});
	Image output;
	Diagnostic diagnostic;
	REQUIRE(ReplayPosterize(hdr, output, diagnostic) == Status::Ok);
	SurfacePixel pixel;
	REQUIRE(LoadSurfacePixel(output, 0, 0, pixel));
	CHECK(pixel == SurfacePixel{1, 1, 1, 1});
	const Image signedHdr = PosterizeFixture(SurfaceFormat::RGBA32Float, {-0.5, 0.25, 0.5, 1});
	REQUIRE(ReplayPosterize(signedHdr, output, diagnostic) == Status::Ok);
	REQUIRE(LoadSurfacePixel(output, 0, 0, pixel));
	// The range copy clips red to zero, but the original negative numerator clamps -infinity to zero.
	CHECK(pixel == SurfacePixel{0, 1, 1, 1});
}

TEST_CASE(
	"Posterize second reduction minimum reads the previous maximum surface", "[imagegraph][posterize_hdr]"
) {
	using namespace engine::imagegraph;
	Image source =
		PosterizeFixture(SurfaceFormat::RGBA8Unorm, {16.0 / 255, 16.0 / 255, 16.0 / 255, 1}, 128, 128);
	REQUIRE(StoreSurfacePixel(source, 1, 1, {0, 0, 0, 1}));
	const auto *entry = FindCatalogueEntry("pc.posterize");
	REQUIRE(entry);
	Node node{"posterize", std::string(entry->Type), "", {}, {}};
	EvaluationRequest request;
	detail::EvaluationBudget budget(Limits::MaximumEvaluationBytes);
	detail::NodeContext context(node, *entry, request, budget);
	context.ByteBudget = Limits::MaximumEvaluationBytes;
	std::array<double, 3> minimum{1, 1, 1}, maximum{};
	const bool ok = detail::PosterizeLocalRange(context, source, minimum, maximum);
	INFO(context.FailureMessage);
	REQUIRE(ok);
	// First-pass maxima all16 discard the one zero. Pinned second-pass minimum samples those maxima.
	CHECK(minimum == std::array<double, 3>{1, 1, 1});
	CHECK(maximum == std::array<double, 3>{16, 16, 16});
	CHECK(budget.Used() == 0);
	CHECK(budget.Peak() == 4 * 128 * 128 * 4 + 64 * 64 * 4);
	Image output;
	Diagnostic diagnostic;
	const auto status = ReplayPosterize(source, output, diagnostic);
	INFO(diagnostic.Message);
	REQUIRE(status == Status::Ok);
	SurfacePixel pixel;
	REQUIRE(LoadSurfacePixel(output, 1, 1, pixel));
	CHECK(pixel == SurfacePixel{1, 1, 1, 1});
}

TEST_CASE(
	"Posterize four temporary surfaces overlap prior live payload exactly", "[imagegraph][posterize_hdr]"
) {
	using namespace engine::imagegraph;
	const Image source = PosterizeFixture(SurfaceFormat::RGBA8Unorm, {0.5, 0.25, 0.125, 1});
	const auto *entry = FindCatalogueEntry("pc.posterize");
	REQUIRE(entry);
	Node node{"posterize", std::string(entry->Type), "", {}, {}};
	EvaluationRequest request;
	for (uint64_t limit : {uint64_t{23}, uint64_t{22}}) {
		detail::EvaluationBudget budget(limit);
		auto prior = budget.Reserve(7);
		REQUIRE(prior);
		detail::NodeContext context(node, *entry, request, budget);
		context.ByteBudget = limit;
		std::array<double, 3> minimum{1, 1, 1}, maximum{};
		const bool ok = detail::PosterizeLocalRange(context, source, minimum, maximum);
		INFO(context.FailureMessage);
		CHECK(ok == (limit == 23));
		CHECK(budget.Used() == 7);
		if (ok)
			CHECK(budget.Peak() == 23);
		else
			CHECK(context.FailureCode == Status::LimitExceeded);
	}
}

TEST_CASE("Posterize singular local ranges refuse atomically", "[imagegraph][posterize_hdr]") {
	using namespace engine::imagegraph;
	const Image source = PosterizeFixture(SurfaceFormat::RGBA32Float, {0, 0.25, 0.5, 1});
	Image output = source;
	const Image sentinel = output;
	Diagnostic diagnostic;
	const auto status = ReplayPosterize(source, output, diagnostic);
	INFO(diagnostic.Message);
	REQUIRE(status == Status::InvalidValue);
	CHECK(output == sentinel);
}

TEST_CASE(
	"Posterize partial-alpha reduction uses explicitly cleared native targets", "[imagegraph][posterize_hdr]"
) {
	using namespace engine::imagegraph;
	const Image source = PosterizeFixture(
		SurfaceFormat::RGBA8Unorm, {16.0 / 255, 16.0 / 255, 16.0 / 255, 128.0 / 255}, 64, 64
	);
	const auto *entry = FindCatalogueEntry("pc.posterize");
	REQUIRE(entry);
	Node node{"posterize", std::string(entry->Type), "", {}, {}};
	EvaluationRequest request;
	detail::EvaluationBudget budget(Limits::MaximumEvaluationBytes);
	detail::NodeContext context(node, *entry, request, budget);
	context.ByteBudget = Limits::MaximumEvaluationBytes;
	std::array<double, 3> minimum{1, 1, 1}, maximum{};
	const bool ok = detail::PosterizeLocalRange(context, source, minimum, maximum);
	INFO(context.FailureMessage);
	REQUIRE(ok);
	CHECK(maximum == std::array<double, 3>{8, 8, 8});
	CHECK(minimum == std::array<double, 3>{1, 1, 1});
}
