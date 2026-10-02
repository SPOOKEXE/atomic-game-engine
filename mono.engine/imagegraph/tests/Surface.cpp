#include <engine/imagegraph/Surface.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <bit>
#include <cmath>
#include <limits>

TEST_SUITE_ID("engine.imagegraph.surface")
using namespace engine::imagegraph;

TEST_CASE(
	"surface layouts use exact format bytes and reject overflowing capacities", "[imagegraph][surface]"
) {
	constexpr uint8_t sizes[]{2, 4, 8, 16, 1, 2, 4};
	for (int64_t choice = 2; choice <= 8; ++choice) {
		const auto format = SourceSurfaceFormat(choice);
		REQUIRE(format);
		const uint64_t exact = uint64_t(sizes[choice - 2]) * 6;
		const auto layout = CheckedSurfaceLayout(2, 3, *format, exact);
		REQUIRE(layout);
		CHECK(layout->RowBytes == 2 * sizes[choice - 2]);
		CHECK(layout->Bytes == exact);
		CHECK_FALSE(CheckedSurfaceLayout(2, 3, *format, exact - 1));
	}
	CHECK_FALSE(SourceSurfaceFormat(1));
	CHECK_FALSE(SourceSurfaceFormat(9));
	CHECK_FALSE(CheckedSurfaceLayout(0, 3, SurfaceFormat::RGBA32Float, 100));
	CHECK_FALSE(CheckedSurfaceLayout(UINT32_MAX, UINT32_MAX, SurfaceFormat::RGBA32Float, UINT64_MAX));
	CHECK_FALSE(CheckedSurfaceLayout(1, 1, static_cast<SurfaceFormat>(255), 100));
}

TEST_CASE("binary16 conversion preserves every finite word and rounds ties even", "[imagegraph][surface]") {
	bool complete = true;
	for (uint32_t bits = 0; bits <= UINT16_MAX; ++bits) {
		if ((bits & 0x7c00) == 0x7c00) continue;
		complete &= EncodeHalf(DecodeHalf(uint16_t(bits))) == bits;
	}
	CHECK(complete);
	CHECK(EncodeHalf(1.0f + std::ldexp(1.0f, -11)) == 0x3c00);
	CHECK(EncodeHalf(1.0f + 3 * std::ldexp(1.0f, -11)) == 0x3c02);
	CHECK(EncodeHalf(std::ldexp(1.0f, -24)) == 1);
	CHECK(EncodeHalf(std::ldexp(1.0f, -25)) == 0);
	CHECK(EncodeHalf(65504.0f) == 0x7bff);
	CHECK(EncodeHalf(-0.0f) == 0x8000);
	CHECK(std::signbit(DecodeHalf(0x8000)));
	CHECK(std::isinf(DecodeHalf(0x7c00)));
	CHECK(std::isnan(DecodeHalf(0x7e00)));
}

TEST_CASE(
	"numeric surface storage retains signed HDR and refuses writes atomically", "[imagegraph][surface]"
) {
	for (const auto format : {SurfaceFormat::RGBA16Float, SurfaceFormat::RGBA32Float}) {
		const auto layout = CheckedSurfaceLayout(1, 1, format, 100);
		REQUIRE(layout);
		Image image{1, 1, std::vector<uint8_t>(layout->Bytes), 0, format};
		const SurfacePixel sample{-2, 4, .25, -0.0};
		REQUIRE(StoreSurfacePixel(image, 0, 0, sample));
		SurfacePixel actual{};
		REQUIRE(LoadSurfacePixel(image, 0, 0, actual));
		CHECK(actual == sample);
		CHECK(std::signbit(actual[3]));
		CHECK(ValidSurfaceLayout(image, 1, layout->Bytes));
		CHECK(FiniteSurfaceSamples(image));
		const auto retained = image.Pixels;
		CHECK_FALSE(StoreSurfacePixel(image, 0, 0, {std::numeric_limits<double>::infinity(), 0, 0, 1}));
		CHECK_FALSE(StoreSurfacePixel(image, 0, 0, {std::numeric_limits<double>::max(), 0, 0, 1}));
		CHECK(image.Pixels == retained);
		CHECK_FALSE(StoreSurfacePixel(image, 1, 0, sample));
		CHECK_FALSE(LoadSurfacePixel(image, 0, 1, actual));
	}
	Image red{1, 1, std::vector<uint8_t>(4), 0, SurfaceFormat::R32Float};
	REQUIRE(StoreSurfacePixel(red, 0, 0, {-3, 0, 0, 1}));
	SurfacePixel sampled{};
	REQUIRE(LoadSurfacePixel(red, 0, 0, sampled));
	CHECK(sampled == SurfacePixel{-3, 0, 0, 1});
	CHECK(red.Pixels == std::vector<uint8_t>{0, 0, 64, 192});
	red.Pixels = {0, 0, 128, 127};
	CHECK_FALSE(FiniteSurfaceSamples(red));
}

TEST_CASE("normalized native layouts quantize and hashes identify typed content", "[imagegraph][surface]") {
	Image packed{1, 1, std::vector<uint8_t>(2), 0, SurfaceFormat::RGBA4Unorm};
	REQUIRE(StoreSurfacePixel(packed, 0, 0, {1, 0, .5, 1}));
	CHECK(packed.Pixels == std::vector<uint8_t>{15, 248});
	Image bytes{1, 1, {0, 0, 0, 0}, 0};
	Image floating{1, 1, {0, 0, 0, 0}, 0, SurfaceFormat::R32Float};
	CHECK(SurfaceHash(bytes) != SurfaceHash(floating));
	floating.Width = 2;
	CHECK_FALSE(ValidSurfaceLayout(floating, 2, 100));
}
