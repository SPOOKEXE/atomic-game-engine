#pragma once

// CPU image oracle for render fixtures. No device state or renderer math enters this comparison.

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <optional>
#include <span>

namespace engine::render::test {

	enum class ImageFormat { Rgba8Unorm, Bgra8Unorm, R32Float, Rgba32Float, R32Uint };

	// Rows start at the top. Bytes are native-endian CPU readback, with no implicit gamma conversion.
	struct ImageView {
		uint32_t Width = 0;
		uint32_t Height = 0;
		ImageFormat Format = ImageFormat::Rgba8Unorm;
		std::span<const std::byte> Bytes;
		size_t RowStrideBytes = 0;
	};

	struct PixelRegion {
		uint32_t X = 0;
		uint32_t Y = 0;
		uint32_t Width = 0;
		uint32_t Height = 0;
	};

	struct ImageTolerance {
		double Absolute = 0.0;
		double Relative = 0.0;
		size_t AllowedMismatchedPixels = 0;
		double MaximumRootMeanSquareError = std::numeric_limits<double>::max();
		std::optional<PixelRegion> Region;
	};

	enum class ImageComparisonStatus {
		Match,
		Mismatch,
		InvalidImage,
		DimensionMismatch,
		IncompatibleFormats,
		InvalidTolerance,
		NonFinite,
	};

	struct [[nodiscard]] ImageComparison {
		ImageComparisonStatus Status = ImageComparisonStatus::InvalidImage;
		size_t ComparedPixels = 0;
		size_t MismatchedPixels = 0;
		size_t NonFiniteSamples = 0;
		double MaximumAbsoluteError = 0.0;
		double MeanAbsoluteError = 0.0;
		double RootMeanSquareError = 0.0;
		std::optional<PixelRegion> MismatchRegion;

		bool Passed() const {
			return Status == ImageComparisonStatus::Match;
		}
	};

	namespace image_comparison_detail {
		inline size_t Channels(ImageFormat format) {
			switch (format) {
			case ImageFormat::Rgba8Unorm:
			case ImageFormat::Bgra8Unorm:
			case ImageFormat::Rgba32Float:
				return 4;
			case ImageFormat::R32Float:
			case ImageFormat::R32Uint:
				return 1;
			}
			return 0;
		}

		inline size_t PixelBytes(ImageFormat format) {
			return format == ImageFormat::Rgba32Float ? 16 : 4;
		}

		inline std::optional<size_t> ValidStride(const ImageView &image) {
			if (image.Width == 0 || image.Height == 0 || Channels(image.Format) == 0 ||
				image.Width > std::numeric_limits<size_t>::max() / PixelBytes(image.Format)) {
				return {};
			}
			const size_t packedStride = static_cast<size_t>(image.Width) * PixelBytes(image.Format);
			const size_t stride = image.RowStrideBytes == 0 ? packedStride : image.RowStrideBytes;
			if (stride < packedStride || image.Bytes.size() < packedStride ||
				image.Height - 1 > (image.Bytes.size() - packedStride) / stride) {
				return {};
			}
			return stride;
		}

		inline double Sample(const std::byte *pixel, ImageFormat format, size_t channel) {
			if (format == ImageFormat::R32Uint) {
				uint32_t identifier;
				std::memcpy(&identifier, pixel, sizeof(identifier));
				return identifier;
			}
			if (format == ImageFormat::R32Float || format == ImageFormat::Rgba32Float) {
				float sample;
				std::memcpy(&sample, pixel + channel * sizeof(float), sizeof(sample));
				return sample;
			}
			if (format == ImageFormat::Bgra8Unorm && channel != 3) {
				channel = 2 - channel;
			}
			return std::to_integer<uint8_t>(pixel[channel]) / 255.0;
		}
	}

	// Outliers are counted once per pixel. R32Uint IDs remain exact regardless of float tolerances.
	// Non-finite samples always fail, even when the outlier budget would otherwise admit them.
	inline ImageComparison
	CompareImages(const ImageView &expected, const ImageView &actual, const ImageTolerance &tolerance = {}) {
		using namespace image_comparison_detail;
		ImageComparison comparison;
		const auto expectedStride = ValidStride(expected);
		const auto actualStride = ValidStride(actual);
		if (!expectedStride || !actualStride) {
			return comparison;
		}
		if (expected.Width != actual.Width || expected.Height != actual.Height) {
			comparison.Status = ImageComparisonStatus::DimensionMismatch;
			return comparison;
		}
		const bool exactIds = expected.Format == ImageFormat::R32Uint;
		if (Channels(expected.Format) != Channels(actual.Format) ||
			exactIds != (actual.Format == ImageFormat::R32Uint)) {
			comparison.Status = ImageComparisonStatus::IncompatibleFormats;
			return comparison;
		}
		const PixelRegion region =
			tolerance.Region.value_or(PixelRegion{0, 0, expected.Width, expected.Height});
		if (!std::isfinite(tolerance.Absolute) || tolerance.Absolute < 0 ||
			!std::isfinite(tolerance.Relative) || tolerance.Relative < 0 ||
			!std::isfinite(tolerance.MaximumRootMeanSquareError) ||
			tolerance.MaximumRootMeanSquareError < 0 || region.Width == 0 || region.Height == 0 ||
			region.X >= expected.Width || region.Y >= expected.Height ||
			region.Width > expected.Width - region.X || region.Height > expected.Height - region.Y) {
			comparison.Status = ImageComparisonStatus::InvalidTolerance;
			return comparison;
		}

		const size_t channels = Channels(expected.Format);
		double absoluteSum = 0.0;
		double squaredSum = 0.0;
		uint32_t minimumX = expected.Width;
		uint32_t minimumY = expected.Height;
		uint32_t maximumX = 0;
		uint32_t maximumY = 0;
		for (uint32_t y = region.Y; y < region.Y + region.Height; y++) {
			for (uint32_t x = region.X; x < region.X + region.Width; x++) {
				const auto *expectedPixel =
					expected.Bytes.data() + y * *expectedStride + x * PixelBytes(expected.Format);
				const auto *actualPixel =
					actual.Bytes.data() + y * *actualStride + x * PixelBytes(actual.Format);
				bool mismatch = false;
				for (size_t channel = 0; channel < channels; channel++) {
					const double reference = Sample(expectedPixel, expected.Format, channel);
					const double observed = Sample(actualPixel, actual.Format, channel);
					if (!std::isfinite(reference) || !std::isfinite(observed)) {
						comparison.NonFiniteSamples++;
						mismatch = true;
						continue;
					}
					const double error = std::abs(reference - observed);
					absoluteSum += error;
					squaredSum += error * error;
					comparison.MaximumAbsoluteError = std::max(comparison.MaximumAbsoluteError, error);
					const double allowed =
						exactIds ? 0.0 : tolerance.Absolute + tolerance.Relative * std::abs(reference);
					mismatch = mismatch || error > allowed;
				}
				comparison.ComparedPixels++;
				if (!mismatch) {
					continue;
				}
				comparison.MismatchedPixels++;
				minimumX = std::min(minimumX, x);
				minimumY = std::min(minimumY, y);
				maximumX = std::max(maximumX, x);
				maximumY = std::max(maximumY, y);
			}
		}
		if (comparison.MismatchedPixels != 0) {
			comparison.MismatchRegion =
				PixelRegion{minimumX, minimumY, maximumX - minimumX + 1, maximumY - minimumY + 1};
		}
		if (comparison.NonFiniteSamples != 0) {
			comparison.MaximumAbsoluteError = std::numeric_limits<double>::infinity();
			comparison.MeanAbsoluteError = std::numeric_limits<double>::infinity();
			comparison.RootMeanSquareError = std::numeric_limits<double>::infinity();
			comparison.Status = ImageComparisonStatus::NonFinite;
			return comparison;
		}
		const double samples = static_cast<double>(comparison.ComparedPixels) * static_cast<double>(channels);
		comparison.MeanAbsoluteError = absoluteSum / samples;
		comparison.RootMeanSquareError = std::sqrt(squaredSum / samples);
		comparison.Status = comparison.MismatchedPixels <= tolerance.AllowedMismatchedPixels &&
									comparison.RootMeanSquareError <= tolerance.MaximumRootMeanSquareError
								? ImageComparisonStatus::Match
								: ImageComparisonStatus::Mismatch;
		return comparison;
	}
}
