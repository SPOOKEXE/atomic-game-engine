#pragma once

// Native RGBA8 versions of source-visible Pixel Composer generation operations.
// The evaluator owns dimensions, links, and allocation limits before calling them.

#include <engine/imagegraph/Document.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace engine::imagegraph::detail {
	inline size_t SampleOffset(const Image &image, uint32_t x, uint32_t y, uint32_t width, uint32_t height) {
		const uint64_t sampleX = ((2ull * x + 1) * image.Width) / (2ull * width);
		const uint64_t sampleY = ((2ull * y + 1) * image.Height) / (2ull * height);
		return (sampleY * image.Width + sampleX) * 4;
	}

	// Foreground is composited by the shader before the mask adjusts alpha.
	inline void GenerateSolid(
		Image &output,
		Colour colour,
		const Image *foreground,
		const Image *mask,
		bool empty,
		bool maskAlphaOnly
	) {
		for (uint32_t y = 0; y < output.Height; y++) {
			for (uint32_t x = 0; x < output.Width; x++) {
				const size_t index = (static_cast<size_t>(y) * output.Width + x) * 4;
				if (empty) {
					if (foreground == nullptr) {
						std::fill_n(output.Pixels.begin() + index, 4, uint8_t{0});
						continue;
					}
					const size_t fgIndex = SampleOffset(*foreground, x, y, output.Width, output.Height);
					std::copy_n(foreground->Pixels.begin() + fgIndex, 4, output.Pixels.begin() + index);
					continue;
				}

				std::array<double, 4> channels{
					double(colour.Red), double(colour.Green), double(colour.Blue), double(colour.Alpha)
				};
				if (foreground != nullptr) {
					const size_t fgIndex = SampleOffset(*foreground, x, y, output.Width, output.Height);
					const double alpha = foreground->Pixels[fgIndex + 3] / 255.0;
					for (size_t channel = 0; channel < 3; channel++) {
						channels[channel] =
							channels[channel] * (1.0 - alpha) + foreground->Pixels[fgIndex + channel] * alpha;
					}
					channels[3] = 255.0;
				}
				if (mask != nullptr) {
					const size_t maskIndex = SampleOffset(*mask, x, y, output.Width, output.Height);
					double amount = mask->Pixels[maskIndex + 3] / 255.0;
					if (!maskAlphaOnly) {
						amount *= (double(mask->Pixels[maskIndex]) + mask->Pixels[maskIndex + 1] +
								   mask->Pixels[maskIndex + 2]) /
								  (3.0 * 255.0);
					}
					channels[3] *= amount;
				}
				for (size_t channel = 0; channel < 4; channel++) {
					output.Pixels[index + channel] = static_cast<uint8_t>(std::lround(channels[channel]));
				}
			}
		}
	}

	// Returns false where the source shader's division produces no finite height.
	inline bool BlendHeight(
		const Image &background,
		const Image &foreground,
		Image &output,
		int64_t mode,
		int64_t type,
		double factor
	) {
		for (uint32_t y = 0; y < output.Height; y++) {
			for (uint32_t x = 0; x < output.Width; x++) {
				const size_t index = (static_cast<size_t>(y) * output.Width + x) * 4;
				const size_t bgIndex = SampleOffset(background, x, y, output.Width, output.Height);
				const size_t fgIndex = SampleOffset(foreground, x, y, output.Width, output.Height);
				const auto height = [](const Image &image, size_t offset) {
					return (double(image.Pixels[offset]) + image.Pixels[offset + 1] +
							image.Pixels[offset + 2]) *
						   image.Pixels[offset + 3] / (3.0 * 255.0 * 255.0);
				};
				const double h0 = height(background, bgIndex);
				const double h1 = height(foreground, fgIndex);
				const double nearHeight = mode == 0 ? std::min(h0, h1) : std::max(h0, h1);
				const double a = mode == 0 ? 1.0 - h0 : h0;
				const double b = mode == 0 ? 1.0 - h1 : h1;
				const double difference = b - a;
				double blended = 0.0;
				if (type == 0) {
					const double k = factor;
					if (k == 0.0) return false;
					blended = -k * std::log2(std::exp2(-a / k) + std::exp2(-b / k));
				} else if (type == 1) {
					const double k = 2.0 * nearHeight * factor;
					blended = 0.5 * (a + b - std::sqrt(difference * difference + k * k));
				} else if (type == 2) {
					const double k = nearHeight * factor * std::log(2.0);
					if (k == 0.0) return false;
					blended = a + difference / (1.0 - std::exp2(difference / k));
				} else if (type == 3 || type == 4 || type == 5) {
					const double multiplier =
						type == 3 ? 4.0 : (type == 4 ? 6.0 : 1.0 / (1.0 - std::sqrt(0.5)));
					const double k = factor * multiplier;
					if (k == 0.0) return false;
					const double h = std::max(k - std::abs(a - b), 0.0) / k;
					if (type == 3) blended = std::min(a, b) - h * h * k * 0.25;
					if (type == 4) blended = std::min(a, b) - h * h * h * k / 6.0;
					if (type == 5)
						blended = std::min(a, b) - k * 0.5 * (1.0 + h - std::sqrt(1.0 - h * (h - 2.0)));
				} else {
					return false;
				}
				if (!std::isfinite(blended)) return false;
				const double result = std::clamp(mode == 0 ? 1.0 - blended : blended, 0.0, 1.0);
				const uint8_t grey = static_cast<uint8_t>(std::lround(result * 255.0));
				std::fill_n(output.Pixels.begin() + index, 3, grey);
				output.Pixels[index + 3] =
					std::max(background.Pixels[bgIndex + 3], foreground.Pixels[fgIndex + 3]);
			}
		}
		return true;
	}
}
