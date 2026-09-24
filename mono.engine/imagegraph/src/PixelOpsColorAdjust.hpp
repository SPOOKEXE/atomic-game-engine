#pragma once

// Scalar Surface mode of the pinned Pixel Composer Color Adjust shader.
// Mask inversion and feathering are performed by the caller before this step.

#include "PixelOpsBlend.hpp"
#include "PixelOpsGenerate.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace engine::imagegraph::detail {
	enum class ColorAdjustStatus : uint8_t { Ok, InvalidImage, InvalidControl, UnsupportedControl };

	struct ColorAdjustControls {
		double Brightness = 0.0;
		double Contrast = 0.5;
		double Exposure = 1.0;
		double Hue = 0.0;
		double Saturation = 0.0;
		double Value = 0.0;
		double Alpha = 1.0;
		Colour Blend{255, 255, 255, 255};
		double BlendAmount = 0.0;
		int64_t BlendMode = 0;
		bool MappedParameter = false;
		bool PaletteInput = false;
	};

	inline ColorAdjustStatus ColorAdjustSurface(
		const Image &source, Image &output, const ColorAdjustControls &control, const Image *mask = nullptr
	) {
		if (source.Width == 0 || source.Height == 0 || output.Width != source.Width ||
			output.Height != source.Height || source.Pixels.size() != output.Pixels.size() ||
			source.Pixels.size() != static_cast<size_t>(source.Width) * source.Height * 4 ||
			(mask != nullptr && (mask->Width == 0 || mask->Height == 0 ||
								 mask->Pixels.size() != static_cast<size_t>(mask->Width) * mask->Height * 4)))
			return ColorAdjustStatus::InvalidImage;
		if (control.MappedParameter || control.PaletteInput) return ColorAdjustStatus::UnsupportedControl;
		if (control.BlendMode < 0 || control.BlendMode > 12 || !std::isfinite(control.Brightness) ||
			!std::isfinite(control.Contrast) || !std::isfinite(control.Exposure) ||
			!std::isfinite(control.Hue) || !std::isfinite(control.Saturation) ||
			!std::isfinite(control.Value) || !std::isfinite(control.Alpha) ||
			!std::isfinite(control.BlendAmount))
			return ColorAdjustStatus::InvalidControl;

		const BlendColor3 blend{
			control.Blend.Red / 255.0, control.Blend.Green / 255.0, control.Blend.Blue / 255.0
		};
		const double amount = control.BlendAmount * (control.Blend.Alpha / 255.0);
		for (uint32_t y = 0; y < source.Height; y++) {
			for (uint32_t x = 0; x < source.Width; x++) {
				const size_t offset = (static_cast<size_t>(y) * source.Width + x) * 4;
				BlendChannels original{};
				BlendChannels adjusted{};
				for (size_t channel = 0; channel < 4; channel++) {
					original[channel] = source.Pixels[offset + channel] / 255.0;
					adjusted[channel] =
						std::clamp(0.5 + control.Contrast * 2.0 * (original[channel] - 0.5), 0.0, 1.0);
					if (channel < 3)
						adjusted[channel] = std::clamp(adjusted[channel] + control.Brightness, 0.0, 1.0);
					adjusted[channel] = std::clamp(adjusted[channel] * control.Exposure, 0.0, 1.0);
				}
				BlendColor3 hsv = RgbToHsv(adjusted);
				hsv[0] = hsv[0] + control.Hue - std::floor(hsv[0] + control.Hue);
				hsv[2] = std::clamp(
					(hsv[2] + control.Value) * (1.0 + control.Saturation * hsv[1] * 0.5), 0.0, 1.0
				);
				hsv[1] = std::clamp(hsv[1] * (control.Saturation + 1.0), 0.0, 1.0);
				BlendColor3 rgb = HsvToRgb(hsv);
				const BlendChannels rgbChannels{rgb[0], rgb[1], rgb[2], original[3]};
				const BlendColor3 sourceHsv = RgbToHsv(rgbChannels);
				const BlendChannels blendChannels{blend[0], blend[1], blend[2], 1.0};
				const BlendColor3 blendHsv = RgbToHsv(blendChannels);
				BlendColor3 mixed = blend;
				const double luminance = rgb[0] * 0.2126 + rgb[1] * 0.7152 + rgb[2] * 0.0722;
				for (size_t channel = 0; channel < 3; channel++) {
					switch (control.BlendMode) {
					case 1:
						mixed[channel] = rgb[channel] + blend[channel];
						break;
					case 2:
						mixed[channel] = rgb[channel] - blend[channel];
						break;
					case 3:
						mixed[channel] = rgb[channel] * blend[channel];
						break;
					case 4:
						mixed[channel] = 1.0 - (1.0 - rgb[channel]) * (1.0 - blend[channel]);
						break;
					case 5:
						mixed[channel] = luminance > 0.5 ? 1.0 - (1.0 - 2.0 * (rgb[channel] - 0.5)) *
																	 (1.0 - blend[channel])
														 : 2.0 * rgb[channel] * blend[channel];
						break;
					case 9:
						mixed[channel] = std::max(rgb[channel], blend[channel]);
						break;
					case 10:
						mixed[channel] = std::min(rgb[channel], blend[channel]);
						break;
					case 12:
						mixed[channel] = std::abs(rgb[channel] - blend[channel]);
						break;
					default:
						break;
					}
				}
				if (control.BlendMode == 6) mixed = HsvToRgb({blendHsv[0], sourceHsv[1], sourceHsv[2]});
				if (control.BlendMode == 7)
					mixed = HsvToRgb(
						{sourceHsv[0], sourceHsv[1] * (1.0 - amount) + blendHsv[1] * amount, sourceHsv[2]}
					);
				if (control.BlendMode == 8) {
					BlendColor3 sourceHsl = RgbToHsl(rgbChannels);
					const BlendColor3 blendHsl = RgbToHsl(blendChannels);
					sourceHsl[2] = sourceHsl[2] * (1.0 - amount) + blendHsl[2] * amount;
					mixed = HslToRgb(sourceHsl);
				}
				if (control.BlendMode != 7 && control.BlendMode != 8) {
					for (size_t channel = 0; channel < 3; channel++)
						rgb[channel] = rgb[channel] * (1.0 - amount) + mixed[channel] * amount;
				} else {
					rgb = mixed;
				}
				const size_t maskOffset =
					mask == nullptr ? 0 : SampleOffset(*mask, x, y, source.Width, source.Height);
				for (size_t channel = 0; channel < 4; channel++) {
					double value = channel == 3 ? original[3] : rgb[channel];
					if (mask != nullptr) {
						const double maskValue = (mask->Pixels[maskOffset + channel] / 255.0) *
												 (mask->Pixels[maskOffset + 3] / 255.0);
						value = value * maskValue + original[channel] * (1.0 - maskValue);
					}
					if (channel == 3) {
						const double maskRed = mask == nullptr ? 1.0
															   : (mask->Pixels[maskOffset] / 255.0) *
																	 (mask->Pixels[maskOffset + 3] / 255.0);
						value = original[3] * (1.0 + (control.Alpha - 1.0) * maskRed);
					}
					output.Pixels[offset + channel] =
						static_cast<uint8_t>(std::lround(std::clamp(value, 0.0, 1.0) * 255.0));
				}
			}
		}
		return ColorAdjustStatus::Ok;
	}
}
