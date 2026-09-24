#pragma once

// Private RGBA8 operations shared by document evaluation and exact pixel tests.
// The evaluator checks dimensions, links and allocation budgets before calling them.

#include <engine/imagegraph/Document.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace engine::imagegraph::detail {
	// Pixel Composer's flip shader assigns 1 to X, 2 to Y and 3 to both.
	inline void Flip(const Image &source, Image &destination, int64_t axis) {
		const bool horizontal = axis == 1 || axis == 3;
		const bool vertical = axis == 2 || axis == 3;
		for (uint32_t y = 0; y < source.Height; y++) {
			for (uint32_t x = 0; x < source.Width; x++) {
				const uint32_t sourceX = horizontal ? source.Width - 1 - x : x;
				const uint32_t sourceY = vertical ? source.Height - 1 - y : y;
				const size_t targetOffset = (static_cast<size_t>(y) * source.Width + x) * 4;
				const size_t sourceOffset = (static_cast<size_t>(sourceY) * source.Width + sourceX) * 4;
				std::copy_n(
					source.Pixels.begin() + sourceOffset, 4, destination.Pixels.begin() + targetOffset
				);
			}
		}
	}

	inline void Invert(Image &image, bool includeAlpha) {
		for (size_t offset = 0; offset < image.Pixels.size(); offset += 4) {
			for (size_t channel = 0; channel < (includeAlpha ? 4u : 3u); channel++) {
				image.Pixels[offset + channel] = static_cast<uint8_t>(255 - image.Pixels[offset + channel]);
			}
		}
	}

	inline void AlphaCutoff(Image &image, double minimum) {
		for (size_t offset = 0; offset < image.Pixels.size(); offset += 4) {
			if (static_cast<double>(image.Pixels[offset + 3]) / 255.0 < minimum) {
				std::fill_n(image.Pixels.begin() + offset, 4, uint8_t{0});
			}
		}
	}

	// The documented Mask and Mix inputs use luminance-times-alpha weight. Mask
	// pixels use native nearest normalized-UV sampling when dimensions differ.
	inline void ApplyMaskMix(
		const Image &original, Image &edited, const Image *mask, double mix, bool invertMask = false
	) {
		if (mask == nullptr && mix == 1.0) return;
		for (size_t pixel = 0; pixel < edited.Pixels.size() / 4; pixel++) {
			const size_t offset = pixel * 4;
			double weight = mix;
			if (mask != nullptr) {
				const uint64_t x = pixel % edited.Width;
				const uint64_t y = pixel / edited.Width;
				const uint64_t maskX = ((2 * x + 1) * mask->Width) / (2 * edited.Width);
				const uint64_t maskY = ((2 * y + 1) * mask->Height) / (2 * edited.Height);
				const size_t maskOffset = (maskY * mask->Width + maskX) * 4;
				const double luminance = (static_cast<double>(mask->Pixels[maskOffset]) +
										  mask->Pixels[maskOffset + 1] + mask->Pixels[maskOffset + 2]) /
										 (3.0 * 255.0);
				const double maskAlpha = static_cast<double>(mask->Pixels[maskOffset + 3]) / 255.0;
				const double maskAmount = (invertMask ? 1.0 - luminance : luminance) * maskAlpha;
				weight *= maskAmount;
			}
			weight = std::clamp(weight, 0.0, 1.0);
			std::array<uint8_t, 4> blended{};
			for (size_t channel = 0; channel < 4; channel++) {
				blended[channel] = static_cast<uint8_t>(std::lround(
					static_cast<double>(original.Pixels[offset + channel]) * (1.0 - weight) +
					static_cast<double>(edited.Pixels[offset + channel]) * weight
				));
			}
			if (original.Pixels[offset + 3] == 0) {
				std::copy_n(edited.Pixels.begin() + offset, 3, blended.begin());
			}
			if (edited.Pixels[offset + 3] == 0) {
				std::copy_n(original.Pixels.begin() + offset, 3, blended.begin());
			}
			std::copy(blended.begin(), blended.end(), edited.Pixels.begin() + offset);
		}
	}

	// The source shader performs two alpha-aware Gaussian passes with transparent
	// samples outside the mask. Quantize each pass to the native RGBA8 surface.
	inline Image FeatherMask(const Image &mask, double feather) {
		if (feather <= 0.0) return mask;
		const int radius = std::max(1, static_cast<int>(std::lround(feather)));
		const double spread = 0.3 * ((radius - 1) * 0.5 - 1.0) + 0.8;
		std::vector<double> weights(static_cast<size_t>(radius));
		for (int index = 0; index < radius; index++) {
			const double x = index * 0.5;
			weights[static_cast<size_t>(index)] = std::exp(-(x * x) / (2.0 * spread * spread));
		}
		Image horizontal = mask;
		Image vertical = mask;
		for (int pass = 0; pass < 2; pass++) {
			const Image &source = pass == 0 ? mask : horizontal;
			Image &target = pass == 0 ? horizontal : vertical;
			for (uint32_t y = 0; y < mask.Height; y++) {
				for (uint32_t x = 0; x < mask.Width; x++) {
					double sumAlpha = 0.0;
					double sumWeight = 0.0;
					std::array<double, 3> sumColour{};
					for (int offset = 1 - radius; offset < radius; offset++) {
						const double weight = weights[static_cast<size_t>(std::abs(offset))];
						sumWeight += weight;
						const int64_t sx = pass == 0 ? static_cast<int64_t>(x) + offset : x;
						const int64_t sy = pass == 0 ? y : static_cast<int64_t>(y) + offset;
						if (sx < 0 || sy < 0 || sx >= mask.Width || sy >= mask.Height) continue;
						const size_t index = (static_cast<size_t>(sy) * mask.Width + sx) * 4;
						const double alpha = source.Pixels[index + 3] / 255.0;
						sumAlpha += weight * alpha;
						for (size_t channel = 0; channel < 3; channel++) {
							sumColour[channel] += weight * alpha * source.Pixels[index + channel];
						}
					}
					const size_t index = (static_cast<size_t>(y) * mask.Width + x) * 4;
					for (size_t channel = 0; channel < 3; channel++) {
						target.Pixels[index + channel] = static_cast<uint8_t>(
							std::lround(sumAlpha > 0.0 ? sumColour[channel] / sumAlpha : 0.0)
						);
					}
					target.Pixels[index + 3] =
						static_cast<uint8_t>(std::lround(255.0 * sumAlpha / sumWeight));
				}
			}
		}
		return vertical;
	}

	// Channel bits select which edited RGBA channels survive after mask/mix.
	inline void ApplyChannels(const Image &original, Image &edited, int64_t channels) {
		for (size_t offset = 0; offset < edited.Pixels.size(); offset += 4) {
			for (size_t channel = 0; channel < 4; channel++) {
				if ((channels & (int64_t{1} << channel)) == 0) {
					edited.Pixels[offset + channel] = original.Pixels[offset + channel];
				}
			}
		}
	}

	inline uint64_t PixelHash(const Image &image) {
		uint64_t hash = 14695981039346656037ull;
		for (const uint8_t byte : image.Pixels) {
			hash ^= byte;
			hash *= 1099511628211ull;
		}
		return hash;
	}
}
