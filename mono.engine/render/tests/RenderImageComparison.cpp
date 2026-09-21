#include "RenderImageComparison.hpp"

#include <engine/testing/Suite.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <limits>

TEST_SUITE_ID("engine.render.imagecomparison")

using namespace engine::render::test;
using Catch::Approx;

namespace {
	constexpr std::array<uint8_t, 24> ASYMMETRIC_RGBA{
		255, 0,	 0,	 255, 0,  255, 0,	255, 0,	  0,   255, 255,
		20,	 40, 60, 255, 80, 100, 120, 255, 140, 160, 180, 255,
	};

	template <typename T, size_t N>
	ImageView View(const std::array<T, N> &pixels, uint32_t width, uint32_t height, ImageFormat format) {
		return ImageView{width, height, format, std::as_bytes(std::span(pixels))};
	}
}

TEST_CASE("image oracle rejects empty malformed and mismatched captures", "[render][imagecomparison]") {
	const auto image = View(ASYMMETRIC_RGBA, 3, 2, ImageFormat::Rgba8Unorm);
	CHECK(CompareImages({}, {}).Status == ImageComparisonStatus::InvalidImage);
	CHECK(CompareImages(image, image).Passed());
	auto invalid = image;
	invalid.Bytes = invalid.Bytes.first(23);
	CHECK(CompareImages(image, invalid).Status == ImageComparisonStatus::InvalidImage);
	invalid = image;
	invalid.RowStrideBytes = 11;
	CHECK(CompareImages(invalid, image).Status == ImageComparisonStatus::InvalidImage);
	invalid.RowStrideBytes = std::numeric_limits<size_t>::max();
	CHECK(CompareImages(invalid, image).Status == ImageComparisonStatus::InvalidImage);
	invalid = image;
	invalid.Width = 2;
	CHECK(CompareImages(invalid, image).Status == ImageComparisonStatus::DimensionMismatch);
	invalid = image;
	invalid.Format = static_cast<ImageFormat>(255);
	CHECK(CompareImages(invalid, image).Status == ImageComparisonStatus::InvalidImage);
	invalid = image;
	invalid.Format = ImageFormat::R32Float;
	CHECK(CompareImages(invalid, image).Status == ImageComparisonStatus::IncompatibleFormats);
	invalid = image;
	invalid.Width = std::numeric_limits<uint32_t>::max();
	invalid.Height = std::numeric_limits<uint32_t>::max();
	CHECK(CompareImages(invalid, image).Status == ImageComparisonStatus::InvalidImage);
}

TEST_CASE(
	"image oracle reads padded unaligned rows and explicit channel order", "[render][imagecomparison]"
) {
	std::array<uint8_t, 33> padded{};
	for (size_t pixel = 0; pixel < 6; pixel++) {
		const size_t offset = 1 + (pixel / 3) * 16 + (pixel % 3) * 4;
		for (size_t channel = 0; channel < 4; channel++) {
			padded[offset + channel] = ASYMMETRIC_RGBA[pixel * 4 + (channel == 3 ? 3 : 2 - channel)];
		}
	}
	const auto expected = View(ASYMMETRIC_RGBA, 3, 2, ImageFormat::Rgba8Unorm);
	ImageView actual{3, 2, ImageFormat::Bgra8Unorm, std::as_bytes(std::span(padded)).subspan(1), 16};
	CHECK(CompareImages(expected, actual).Passed());
	actual.Format = ImageFormat::Rgba8Unorm;
	CHECK_FALSE(CompareImages(expected, actual).Passed());
	actual.Format = ImageFormat::Bgra8Unorm;
	actual.RowStrideBytes = 12;
	CHECK_FALSE(CompareImages(expected, actual).Passed());

	const std::array<float, 2> depth{0.25f, 0.75f};
	std::array<std::byte, 10> unaligned{};
	std::memcpy(unaligned.data() + 1, depth.data(), sizeof(depth));
	const ImageView unalignedView{2, 1, ImageFormat::R32Float, std::span(unaligned).subspan(1)};
	CHECK(CompareImages(View(depth, 2, 1, ImageFormat::R32Float), unalignedView).Passed());
}

TEST_CASE(
	"image oracle counts bad pixels and reports independent numeric errors", "[render][imagecomparison]"
) {
	const std::array<float, 4> expected{0, 2, 4, 6};
	const std::array<float, 4> actual{0, 3, 2, 6};
	const auto reference = View(expected, 2, 2, ImageFormat::R32Float);
	const auto observed = View(actual, 2, 2, ImageFormat::R32Float);
	const auto comparison = CompareImages(reference, observed);
	CHECK_FALSE(comparison.Passed());
	CHECK(comparison.ComparedPixels == 4);
	CHECK(comparison.MismatchedPixels == 2);
	CHECK(comparison.MaximumAbsoluteError == 2);
	CHECK(comparison.MeanAbsoluteError == Approx(0.75));
	CHECK(comparison.RootMeanSquareError == Approx(std::sqrt(1.25)));
	REQUIRE(comparison.MismatchRegion);
	CHECK(comparison.MismatchRegion->X == 0);
	CHECK(comparison.MismatchRegion->Y == 0);
	CHECK(comparison.MismatchRegion->Width == 2);
	CHECK(comparison.MismatchRegion->Height == 2);
	ImageTolerance tolerance;
	tolerance.AllowedMismatchedPixels = 2;
	CHECK(CompareImages(reference, observed, tolerance).Passed());
	tolerance.MaximumRootMeanSquareError = 1;
	CHECK_FALSE(CompareImages(reference, observed, tolerance).Passed());

	const std::array<float, 4> zero{};
	const std::array<float, 4> white{1, 1, 1, 1};
	CHECK(
		CompareImages(View(zero, 1, 1, ImageFormat::Rgba32Float), View(white, 1, 1, ImageFormat::Rgba32Float))
			.MismatchedPixels == 1
	);
}

TEST_CASE("image oracle combines absolute and reference relative precision", "[render][imagecomparison]") {
	const std::array<float, 3> expected{0, 100, -100};
	const std::array<float, 3> actual{0.125f, 101.125f, -101.125f};
	ImageTolerance tolerance;
	tolerance.Absolute = 0.125;
	tolerance.Relative = 0.01;
	const auto reference = View(expected, 3, 1, ImageFormat::R32Float);
	const auto observed = View(actual, 3, 1, ImageFormat::R32Float);
	CHECK(CompareImages(reference, observed, tolerance).Passed());
	tolerance.Absolute = 0.124;
	CHECK(CompareImages(reference, observed, tolerance).MismatchedPixels == 3);

	const std::array<uint8_t, 4> rgba{0, 255, 0, 255};
	const std::array<float, 4> linear{0, 1, 0, 1};
	CHECK(
		CompareImages(View(rgba, 1, 1, ImageFormat::Rgba8Unorm), View(linear, 1, 1, ImageFormat::Rgba32Float))
			.Passed()
	);
}

TEST_CASE(
	"image oracle refuses nonfinite inputs even with a full outlier budget", "[render][imagecomparison]"
) {
	ImageTolerance tolerance;
	tolerance.AllowedMismatchedPixels = 1;
	const std::array<float, 1> finite{0};
	for (float invalid :
		 {std::numeric_limits<float>::quiet_NaN(),
		  std::numeric_limits<float>::infinity(),
		  -std::numeric_limits<float>::infinity()}) {
		const std::array<float, 1> broken{invalid};
		const auto brokenView = View(broken, 1, 1, ImageFormat::R32Float);
		const auto finiteView = View(finite, 1, 1, ImageFormat::R32Float);
		for (const auto &comparison :
			 {CompareImages(brokenView, brokenView, tolerance),
			  CompareImages(finiteView, brokenView, tolerance),
			  CompareImages(brokenView, finiteView, tolerance)}) {
			CHECK(comparison.Status == ImageComparisonStatus::NonFinite);
			CHECK(comparison.NonFiniteSamples == 1);
			CHECK(comparison.MismatchedPixels == 1);
			CHECK(std::isinf(comparison.MaximumAbsoluteError));
			CHECK(std::isinf(comparison.MeanAbsoluteError));
			CHECK(std::isinf(comparison.RootMeanSquareError));
		}
	}
}

TEST_CASE("image oracle validates tolerance and bounded regions", "[render][imagecomparison]") {
	const auto image = View(ASYMMETRIC_RGBA, 3, 2, ImageFormat::Rgba8Unorm);
	ImageTolerance tolerance;
	for (double invalid :
		 {-1.0, std::numeric_limits<double>::infinity(), std::numeric_limits<double>::quiet_NaN()}) {
		tolerance.Absolute = invalid;
		CHECK(CompareImages(image, image, tolerance).Status == ImageComparisonStatus::InvalidTolerance);
		tolerance = {};
		tolerance.Relative = invalid;
		CHECK(CompareImages(image, image, tolerance).Status == ImageComparisonStatus::InvalidTolerance);
		tolerance = {};
		tolerance.MaximumRootMeanSquareError = invalid;
		CHECK(CompareImages(image, image, tolerance).Status == ImageComparisonStatus::InvalidTolerance);
		tolerance = {};
	}
	for (const PixelRegion region :
		 {PixelRegion{},
		  PixelRegion{3, 0, 1, 1},
		  PixelRegion{0, 2, 1, 1},
		  PixelRegion{2, 0, 2, 1},
		  PixelRegion{0, 1, 1, 2}}) {
		tolerance.Region = region;
		CHECK(CompareImages(image, image, tolerance).Status == ImageComparisonStatus::InvalidTolerance);
	}
}

TEST_CASE(
	"image oracle catches flips shifted projection and stale small features", "[render][imagecomparison]"
) {
	const auto reference = View(ASYMMETRIC_RGBA, 3, 2, ImageFormat::Rgba8Unorm);
	auto flipped = ASYMMETRIC_RGBA;
	std::swap_ranges(flipped.begin(), flipped.begin() + 12, flipped.begin() + 12);
	CHECK(CompareImages(reference, View(flipped, 3, 2, ImageFormat::Rgba8Unorm)).MismatchedPixels == 6);
	auto shifted = ASYMMETRIC_RGBA;
	std::rotate(shifted.begin(), shifted.begin() + 4, shifted.begin() + 12);
	CHECK(CompareImages(reference, View(shifted, 3, 2, ImageFormat::Rgba8Unorm)).MismatchedPixels == 3);

	auto stale = ASYMMETRIC_RGBA;
	stale[16] = 0;
	const auto observed = View(stale, 3, 2, ImageFormat::Rgba8Unorm);
	ImageTolerance tolerance;
	tolerance.Region = PixelRegion{1, 1, 1, 1};
	const auto comparison = CompareImages(reference, observed, tolerance);
	CHECK_FALSE(comparison.Passed());
	CHECK(comparison.ComparedPixels == 1);
	REQUIRE(comparison.MismatchRegion);
	CHECK(comparison.MismatchRegion->X == 1);
	CHECK(comparison.MismatchRegion->Y == 1);
	CHECK(comparison.MismatchRegion->Width == 1);
	CHECK(comparison.MismatchRegion->Height == 1);
	tolerance.Region = PixelRegion{0, 0, 3, 1};
	CHECK(CompareImages(reference, observed, tolerance).Passed());
}

TEST_CASE("image oracle compares full width integer identifiers exactly", "[render][imagecomparison]") {
	const std::array<uint32_t, 2> expected{0xFFFFFFFEu, 0x01000001u};
	const std::array<uint32_t, 2> actual{0xFFFFFFFFu, 0x01000000u};
	ImageTolerance tolerance;
	tolerance.Absolute = 100;
	tolerance.Relative = 1;
	const auto reference = View(expected, 2, 1, ImageFormat::R32Uint);
	const auto observed = View(actual, 2, 1, ImageFormat::R32Uint);
	CHECK(CompareImages(reference, reference).Passed());
	CHECK(CompareImages(reference, observed, tolerance).MismatchedPixels == 2);
	CHECK(CompareImages(reference, observed).MaximumAbsoluteError == 1);
	CHECK(
		CompareImages(reference, View(actual, 2, 1, ImageFormat::R32Float)).Status ==
		ImageComparisonStatus::IncompatibleFormats
	);
}
