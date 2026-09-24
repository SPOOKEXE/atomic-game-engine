#pragma once

// Native RGBA8 pixel equations for the Blend modes present in supplied projects.
// Spatial placement, mask processing, and output dimension selection happen first.

#include "PixelOps.hpp"
#include "PixelOpsGenerate.hpp"

#include <engine/imagegraph/Document.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace engine::imagegraph::detail {
	enum class BlendPixelStatus : uint8_t { Ok, UnsupportedMode, UndefinedDivision };
	using BlendChannels = std::array<double, 4>;
	using BlendColor3 = std::array<double, 3>;
	inline bool SupportedBlendMode(int64_t mode) {
		switch (mode) {
		case 0:
		case 1:
		case 3:
		case 4:
		case 5:
		case 6:
		case 8:
		case 9:
		case 10:
		case 11:
		case 13:
		case 14:
		case 15:
		case 16:
		case 17:
		case 18:
		case 20:
		case 21:
		case 22:
		case 23:
		case 25:
		case 26:
		case 27:
		case 29:
		case 30:
			return true;
		default:
			return false;
		}
	}

	inline BlendColor3 RgbToHsv(const BlendChannels &rgb) {
		const double maximum = std::max({rgb[0], rgb[1], rgb[2]});
		const double minimum = std::min({rgb[0], rgb[1], rgb[2]});
		const double delta = maximum - minimum;
		double hue = 0.0;
		if (delta > 0.0) {
			if (maximum == rgb[0])
				hue = (rgb[1] - rgb[2]) / delta;
			else if (maximum == rgb[1])
				hue = 2.0 + (rgb[2] - rgb[0]) / delta;
			else
				hue = 4.0 + (rgb[0] - rgb[1]) / delta;
			hue /= 6.0;
			if (hue < 0.0) hue += 1.0;
		}
		return {hue, maximum == 0.0 ? 0.0 : delta / maximum, maximum};
	}

	inline BlendColor3 HsvToRgb(BlendColor3 hsv) {
		BlendColor3 rgb{};
		for (size_t channel = 0; channel < 3; channel++) {
			const double offset = channel == 0 ? 1.0 : (channel == 1 ? 2.0 / 3.0 : 1.0 / 3.0);
			const double phase = hsv[0] + offset;
			const double segment = std::abs((phase - std::floor(phase)) * 6.0 - 3.0);
			rgb[channel] = hsv[2] * (1.0 - hsv[1] + hsv[1] * std::clamp(segment - 1.0, 0.0, 1.0));
		}
		return rgb;
	}

	inline BlendColor3 RgbToHsl(const BlendChannels &rgb) {
		const double maximum = std::max({rgb[0], rgb[1], rgb[2]});
		const double minimum = std::min({rgb[0], rgb[1], rgb[2]});
		const double lightness = (maximum + minimum) * 0.5;
		if (maximum == minimum) return {0.0, 0.0, lightness};
		const double delta = maximum - minimum;
		const double saturation =
			lightness < 0.5 ? delta / (maximum + minimum) : delta / (2.0 - maximum - minimum);
		double hue = maximum == rgb[0] ? (rgb[1] - rgb[2]) / delta
									   : (maximum == rgb[1] ? 2.0 + (rgb[2] - rgb[0]) / delta
															: 4.0 + (rgb[0] - rgb[1]) / delta);
		if (hue < 0.0) hue += 6.0;
		return {hue / 6.0, saturation, lightness};
	}

	inline double HslHueToRgb(double low, double high, double hue) {
		if (hue < 0.0)
			hue += 1.0;
		else if (hue > 1.0)
			hue -= 1.0;
		if (6.0 * hue < 1.0) return low + (high - low) * hue * 6.0;
		if (2.0 * hue < 1.0) return high;
		if (3.0 * hue < 2.0) return low + (high - low) * (2.0 / 3.0 - hue) * 6.0;
		return low;
	}

	inline BlendColor3 HslToRgb(BlendColor3 hsl) {
		if (hsl[1] == 0.0) return {hsl[2], hsl[2], hsl[2]};
		const double high = hsl[2] <= 0.5 ? hsl[2] * (1.0 + hsl[1]) : hsl[2] + hsl[1] - hsl[2] * hsl[1];
		const double low = 2.0 * hsl[2] - high;
		return {
			HslHueToRgb(low, high, hsl[0] + 1.0 / 3.0),
			HslHueToRgb(low, high, hsl[0]),
			HslHueToRgb(low, high, hsl[0] - 1.0 / 3.0)
		};
	}

	inline double BlendMaskAmount(Colour mask, bool alphaOnly) {
		const double alpha = mask.Alpha / 255.0;
		if (alphaOnly) return alpha;
		return (double(mask.Red) + mask.Green + mask.Blue) * alpha / (3.0 * 255.0);
	}

	// Mode numbers are sparse identifiers from the pinned Pixel Composer source.
	inline BlendPixelStatus BlendPixel(
		Colour background,
		Colour foreground,
		int64_t mode,
		double opacity,
		double maskAmount,
		bool preserveAlpha,
		Colour &output
	) {
		const std::array<double, 4> bg = {
			background.Red / 255.0,
			background.Green / 255.0,
			background.Blue / 255.0,
			background.Alpha / 255.0
		};
		const std::array<double, 4> fg = {
			foreground.Red / 255.0,
			foreground.Green / 255.0,
			foreground.Blue / 255.0,
			foreground.Alpha / 255.0
		};
		std::array<double, 4> result{};
		const double weight = opacity * maskAmount;
		const double alpha = fg[3] + bg[3] * (1.0 - fg[3]);
		switch (mode) {
		case 0: { // Normal
			const double foregroundAlpha = fg[3] * weight;
			const double outAlpha = foregroundAlpha + bg[3] * (1.0 - foregroundAlpha);
			if (outAlpha == 0.0) {
				output = {0, 0, 0, 0};
				return BlendPixelStatus::Ok;
			}
			for (size_t channel = 0; channel < 3; channel++) {
				result[channel] =
					(fg[channel] * foregroundAlpha + bg[channel] * bg[3] * (1.0 - foregroundAlpha)) /
					outAlpha;
			}
			result[3] = preserveAlpha ? bg[3] : outAlpha;
			break;
		}
		case 1: // Replace
			for (size_t channel = 0; channel < 4; channel++)
				result[channel] = bg[channel] * (1.0 - weight) + fg[channel] * weight;
			if (preserveAlpha) result[3] = bg[3];
			break;
		case 3:	  // Multiply
		case 9: { // Screen
			if (alpha == 0.0) return BlendPixelStatus::UndefinedDivision;
			for (size_t channel = 0; channel < 4; channel++) {
				const double blend =
					mode == 3 ? bg[channel] * fg[channel]
							  : 1.0 - (1.0 - bg[channel]) *
										  (1.0 - (channel < 3 ? fg[channel] * fg[3] : fg[channel]));
				result[channel] = bg[channel] * (1.0 - weight) + blend * weight;
			}
			for (size_t channel = 0; channel < 3; channel++)
				result[channel] /= alpha;
			if (preserveAlpha) result[3] = bg[3];
			break;
		}
		case 8: { // Add
			const double foregroundAlpha = fg[3] * weight;
			const double outAlpha = foregroundAlpha + bg[3] * (1.0 - foregroundAlpha);
			if (outAlpha == 0.0) return BlendPixelStatus::UndefinedDivision;
			for (size_t channel = 0; channel < 3; channel++) {
				result[channel] = (bg[channel] * bg[3] + fg[channel] * foregroundAlpha) / outAlpha;
			}
			result[3] = preserveAlpha ? bg[3] : bg[3] + foregroundAlpha;
			break;
		}
		case 11: // Maximum
			for (size_t channel = 0; channel < 4; channel++)
				result[channel] = std::max(bg[channel], fg[channel] * weight);
			if (preserveAlpha) result[3] = bg[3];
			break;
		case 6: // Minimum
			for (size_t channel = 0; channel < 4; channel++)
				result[channel] = std::min(bg[channel], fg[channel] * weight);
			if (preserveAlpha) result[3] = bg[3];
			break;
		case 22: { // Subtract
			const double foregroundAlpha = fg[3] * weight;
			const double outAlpha = foregroundAlpha + bg[3] * (1.0 - foregroundAlpha);
			if (outAlpha == 0.0) return BlendPixelStatus::UndefinedDivision;
			const double mixAmount = preserveAlpha ? foregroundAlpha : opacity;
			for (size_t channel = 0; channel < 3; channel++) {
				const double premultiplied = bg[channel] * bg[3];
				const double blend = premultiplied - fg[channel] * foregroundAlpha;
				result[channel] = (premultiplied * (1.0 - mixAmount) + blend * mixAmount) / outAlpha;
			}
			const double blendAlpha = bg[3] - foregroundAlpha;
			result[3] = preserveAlpha ? bg[3] : bg[3] * (1.0 - mixAmount) + blendAlpha * mixAmount;
			break;
		}
		case 4:	   // Color Burn
		case 5:	   // Linear Burn
		case 10:   // Color Dodge
		case 13:   // Overlay
		case 14:   // Soft Light
		case 15:   // Hard Light
		case 16:   // Vivid Light
		case 17:   // Linear Light
		case 18:   // Pin Light
		case 21:   // Exclusion
		case 23: { // Divide
			BlendChannels adjusted = fg;
			adjusted[3] *= weight;
			const double mixedAlpha = adjusted[3] + bg[3] * (1.0 - adjusted[3]);
			if (mixedAlpha == 0.0) return BlendPixelStatus::UndefinedDivision;
			const double luminance = fg[0] * 0.2126 + fg[1] * 0.7152 + fg[2] * 0.0722;
			const double mixAmount = preserveAlpha ? adjusted[3] : opacity;
			for (size_t channel = 0; channel < 4; channel++) {
				const double b = bg[channel];
				const double f = adjusted[channel];
				double blend = 0.0;
				if (mode == 4 || mode == 23) {
					if (f == 0.0) return BlendPixelStatus::UndefinedDivision;
					blend = mode == 4 ? 1.0 - (1.0 - b) / f : b / f;
				} else if (mode == 5)
					blend = b + f - 1.0;
				else if (mode == 10) {
					if (b == 1.0) return BlendPixelStatus::UndefinedDivision;
					blend = f / (1.0 - b);
				} else if (mode == 13) {
					blend = luminance > 0.5 ? 1.0 - (1.0 - 2.0 * (f - 0.5)) * (1.0 - b) : 2.0 * f * b;
				} else if (mode == 14) {
					blend = luminance > 0.5 ? 1.0 - (1.0 - b) * (1.0 - (f - 0.5)) : b * (f + 0.5);
				} else if (mode == 15) {
					blend = luminance > 0.5 ? 1.0 - (1.0 - b) * (1.0 - 2.0 * (f - 0.5)) : 2.0 * b * f;
				} else if (mode == 16) {
					if (luminance <= 0.5 && f == 0.5) return BlendPixelStatus::UndefinedDivision;
					blend = luminance > 0.5 ? 1.0 - (1.0 - b) * (2.0 * (f - 0.5)) : b / (1.0 - 2.0 * f);
				} else if (mode == 17)
					blend = b + 2.0 * f - 1.0;
				else if (mode == 18) {
					blend = luminance > 0.5 ? std::max(b, 2.0 * (f - 0.5)) : std::min(b, 2.0 * f);
				} else
					blend = 0.5 - 2.0 * (b - 0.5) * (f - 0.5);
				const double mixed = mode == 13 ? blend : b * (1.0 - opacity) + blend * opacity;
				result[channel] = b * (1.0 - mixAmount) + mixed * mixAmount;
			}
			for (size_t channel = 0; channel < 3; channel++)
				result[channel] /= mixedAlpha;
			if (preserveAlpha) result[3] = bg[3];
			break;
		}
		case 20: // Difference
			for (size_t channel = 0; channel < 3; channel++)
				result[channel] = bg[channel] * (1.0 - weight) + std::abs(bg[channel] - fg[channel]) * weight;
			result[3] = bg[3] * (1.0 - weight) + weight;
			break;
		case 25:   // Hue
		case 26:   // Saturation
		case 27: { // Luminosity
			if (alpha == 0.0) return BlendPixelStatus::UndefinedDivision;
			const double amount = fg[3] * weight;
			BlendColor3 converted{};
			if (mode == 27) {
				BlendColor3 source = RgbToHsl(bg);
				const BlendColor3 target = RgbToHsl(fg);
				source[2] = source[2] * (1.0 - amount) + target[2] * amount;
				converted = HslToRgb(source);
			} else {
				BlendColor3 source = RgbToHsv(bg);
				const BlendColor3 target = RgbToHsv(fg);
				if (mode == 25) {
					BlendColor3 transferred{target[0], source[1], source[2]};
					const BlendColor3 rgb = HsvToRgb(transferred);
					for (size_t channel = 0; channel < 3; channel++)
						converted[channel] = bg[channel] * (1.0 - amount) + rgb[channel] * amount;
				} else {
					source[1] = source[1] * (1.0 - amount) + target[1] * amount;
					converted = HsvToRgb(source);
				}
			}
			for (size_t channel = 0; channel < 3; channel++)
				result[channel] = converted[channel] / alpha;
			result[3] = bg[3];
			break;
		}
		case 29:   // Equal
		case 30: { // Unequal
			const bool same = background == foreground;
			const double value = ((mode == 29) == same) ? weight : 0.0;
			result.fill(value);
			if (preserveAlpha) result[3] = bg[3];
			break;
		}
		default:
			return BlendPixelStatus::UnsupportedMode;
		}
		for (double channel : result) {
			if (!std::isfinite(channel)) return BlendPixelStatus::UndefinedDivision;
		}
		const auto quantize = [](double channel) {
			return static_cast<uint8_t>(std::lround(std::clamp(channel, 0.0, 1.0) * 255.0));
		};
		output = {quantize(result[0]), quantize(result[1]), quantize(result[2]), quantize(result[3])};
		return BlendPixelStatus::Ok;
	}

	// The source converts a modified mask to grayscale before optional feathering.
	inline Image ModifyBlendMask(const Image &mask, bool invert, bool alphaOnly, double feather) {
		if (!invert && feather == 0.0) return mask;
		Image modified = mask;
		for (size_t offset = 0; offset < modified.Pixels.size(); offset += 4) {
			const double source =
				alphaOnly
					? mask.Pixels[offset + 3] / 255.0
					: (double(mask.Pixels[offset]) + mask.Pixels[offset + 1] + mask.Pixels[offset + 2]) /
						  (3.0 * 255.0);
			const uint8_t amount =
				static_cast<uint8_t>(std::lround((invert ? 1.0 - source : source) * 255.0));
			if (alphaOnly) {
				std::fill_n(modified.Pixels.begin() + offset, 3, uint8_t{255});
				modified.Pixels[offset + 3] = amount;
			} else {
				std::fill_n(modified.Pixels.begin() + offset, 3, amount);
			}
		}
		return feather > 1.0 ? FeatherMask(modified, feather) : modified;
	}

	inline Colour ReadBlendColour(const Image &image, size_t offset) {
		return {
			image.Pixels[offset], image.Pixels[offset + 1], image.Pixels[offset + 2], image.Pixels[offset + 3]
		};
	}

	// The caller allocates output and validates controls and temporary byte budgets.
	inline BlendPixelStatus BlendCanvas(
		const Image &background,
		const Image *foreground,
		const Image *mask,
		Image &output,
		int64_t mode,
		double opacity,
		bool preserveAlpha,
		int64_t fillMode,
		Vector2 position,
		bool maskAlphaOnly
	) {
		for (uint32_t y = 0; y < output.Height; y++) {
			for (uint32_t x = 0; x < output.Width; x++) {
				const size_t offset = (static_cast<size_t>(y) * output.Width + x) * 4;
				const Colour bg =
					ReadBlendColour(background, SampleOffset(background, x, y, output.Width, output.Height));
				Colour result = bg;
				if (foreground != nullptr) {
					Colour fg{0, 0, 0, 0};
					if (fillMode == 1) {
						fg = ReadBlendColour(
							*foreground, SampleOffset(*foreground, x, y, output.Width, output.Height)
						);
					} else if (fillMode == 2) {
						const size_t fgOffset =
							((static_cast<size_t>(y) % foreground->Height) * foreground->Width +
							 x % foreground->Width) *
							4;
						fg = ReadBlendColour(*foreground, fgOffset);
					} else {
						const double left = position.X * output.Width - foreground->Width * 0.5;
						const double top = position.Y * output.Height - foreground->Height * 0.5;
						const int64_t fx = static_cast<int64_t>(std::floor(x + 0.5 - left));
						const int64_t fy = static_cast<int64_t>(std::floor(y + 0.5 - top));
						if (fx >= 0 && fy >= 0 && fx < foreground->Width && fy < foreground->Height) {
							fg = ReadBlendColour(
								*foreground, (static_cast<size_t>(fy) * foreground->Width + fx) * 4
							);
						}
					}
					const double amount =
						mask == nullptr
							? 1.0
							: BlendMaskAmount(
								  ReadBlendColour(
									  *mask, SampleOffset(*mask, x, y, output.Width, output.Height)
								  ),
								  maskAlphaOnly
							  );
					const BlendPixelStatus status =
						BlendPixel(bg, fg, mode, opacity, amount, preserveAlpha, result);
					if (status != BlendPixelStatus::Ok) return status;
				}
				output.Pixels[offset] = result.Red;
				output.Pixels[offset + 1] = result.Green;
				output.Pixels[offset + 2] = result.Blue;
				output.Pixels[offset + 3] = result.Alpha;
			}
		}
		return BlendPixelStatus::Ok;
	}
}
