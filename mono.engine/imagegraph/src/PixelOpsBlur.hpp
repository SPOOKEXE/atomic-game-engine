#pragma once

// Native RGBA8 two-pass form of the pinned public Gaussian Blur default path.
// Mapped size, UV remapping, custom curves, and rotation require separate
// sampling paths and are reported as unsupported controls.

#include <engine/imagegraph/Document.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace engine::imagegraph::detail {
	enum class BlurStatus : uint8_t { Ok, InvalidImage, InvalidControl, UnsupportedControl };

	struct GaussianBlurControls {
		double Size = 8.0;
		int64_t Intensity = 0;
		double AspectRatio = 1.0;
		double DirectionRadians = 0.0;
		bool GammaCorrection = false;
		bool OverrideColor = false;
		Colour OverrideColour{0, 0, 0, 255};
	};

	inline std::vector<double> GaussianWeights(int64_t size) {
		const int64_t count = std::max<int64_t>(1, size);
		const double pi = std::acos(-1.0);
		const double spread = 0.3 * ((count - 1) * 0.5 - 1.0) + 0.8;
		const double factor = 1.0 / std::sqrt(2.0 * pi * spread);
		std::vector<double> weights(static_cast<size_t>(count));
		double total = 0.0;
		for (int64_t index = 0; index < count; index++) {
			const double distance = index * 0.5;
			weights[static_cast<size_t>(index)] =
				factor * std::exp(-(distance * distance) / (2.0 * spread * spread));
			total += weights[static_cast<size_t>(index)] * (index == 0 ? 1.0 : 2.0);
		}
		for (double &weight : weights)
			weight /= total;
		return weights;
	}

	inline void GaussianPass(
		const Image &source,
		Image &output,
		const std::vector<double> &weights,
		bool horizontal,
		bool gammaCorrection,
		bool overrideColor,
		Colour overrideColour
	) {
		for (uint32_t y = 0; y < source.Height; y++) {
			for (uint32_t x = 0; x < source.Width; x++) {
				double weightedAlpha = 0.00001;
				double totalWeight = 0.00001;
				double red = 0.0, green = 0.0, blue = 0.0;
				const auto sample = [&](int64_t offset, double weight) {
					const int64_t sx = horizontal ? int64_t(x) + offset : x;
					const int64_t sy = horizontal ? y : int64_t(y) + offset;
					totalWeight += weight;
					if (sx < 0 || sy < 0 || sx >= source.Width || sy >= source.Height) return;
					const size_t index = (size_t(sy) * source.Width + sx) * 4;
					const double alpha = source.Pixels[index + 3] / 255.0;
					weightedAlpha += weight * alpha;
					const auto linear = [&](size_t channel) {
						const double value = source.Pixels[index + channel] / 255.0;
						return gammaCorrection ? std::pow(std::abs(value), 2.2) : value;
					};
					red += weight * alpha * linear(0);
					green += weight * alpha * linear(1);
					blue += weight * alpha * linear(2);
				};
				sample(0, weights[0]);
				for (size_t index = 1; index < weights.size(); index++) {
					sample(int64_t(index), weights[index]);
					sample(-int64_t(index), weights[index]);
				}
				const size_t index = (size_t(y) * source.Width + x) * 4;
				const auto byte = [](double value) {
					return static_cast<uint8_t>(std::lround(std::clamp(value, 0.0, 1.0) * 255.0));
				};
				const auto encode = [&](double value) {
					return byte(
						gammaCorrection ? std::pow(value / weightedAlpha, 1.0 / 2.2) : value / weightedAlpha
					);
				};
				output.Pixels[index] = overrideColor ? overrideColour.Red : encode(red);
				output.Pixels[index + 1] = overrideColor ? overrideColour.Green : encode(green);
				output.Pixels[index + 2] = overrideColor ? overrideColour.Blue : encode(blue);
				const double alpha = weightedAlpha / totalWeight;
				output.Pixels[index + 3] = byte(alpha * (overrideColor ? overrideColour.Alpha / 255.0 : 1.0));
			}
		}
	}

	inline BlurStatus
	GaussianBlurDefault(const Image &source, Image &output, const GaussianBlurControls &control) {
		const uint64_t bytes = uint64_t(source.Width) * source.Height * 4;
		if (source.Width == 0 || source.Height == 0 || source.Width != output.Width ||
			source.Height != output.Height || source.Pixels.size() != bytes || output.Pixels.size() != bytes)
			return BlurStatus::InvalidImage;
		if (!std::isfinite(control.Size) || control.Size < 0 || control.Size > 32 ||
			!std::isfinite(control.AspectRatio) || !std::isfinite(control.DirectionRadians))
			return BlurStatus::InvalidControl;
		// The shader can read a weight beyond the rounded kernel for fractional sizes.
		if (std::trunc(control.Size) != control.Size) return BlurStatus::UnsupportedControl;
		if (control.Intensity != 0 || control.AspectRatio != 1.0 || control.DirectionRadians != 0.0)
			return BlurStatus::UnsupportedControl;
		const int64_t integerSize = static_cast<int64_t>(control.Size);
		const uint64_t samples = uint64_t(source.Width) * source.Height *
								 (2 * uint64_t(std::max<int64_t>(1, integerSize)) - 1) * 2;
		if (samples > 64'000'000) return BlurStatus::InvalidControl;
		const std::vector<double> weights = GaussianWeights(integerSize);
		Image horizontal{source.Width, source.Height, std::vector<uint8_t>(source.Pixels.size()), 0};
		GaussianPass(
			source,
			horizontal,
			weights,
			true,
			control.GammaCorrection,
			control.OverrideColor,
			control.OverrideColour
		);
		GaussianPass(
			horizontal,
			output,
			weights,
			false,
			control.GammaCorrection,
			control.OverrideColor,
			control.OverrideColour
		);
		return BlurStatus::Ok;
	}
}
