#pragma once
#include "../PixelOpsBlend.hpp"
#include "Gradient.hpp"
namespace engine::imagegraph::detail {
	inline BlendPixelStatus SourceBlendFormula(
		const Rgba &bg,
		const Rgba &fg,
		int64_t mode,
		double opacity,
		double maskAmount,
		bool preserveAlpha,
		Rgba &output
	) {
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
				BlendColor3 source = ShaderRgbToHsv({bg[0], bg[1], bg[2]});
				const BlendColor3 target = ShaderRgbToHsv({fg[0], fg[1], fg[2]});
				if (mode == 25) {
					BlendColor3 transferred{target[0], source[1], source[2]};
					const BlendColor3 rgb = ShaderHsvToRgb(transferred);
					for (size_t channel = 0; channel < 3; channel++)
						converted[channel] = bg[channel] * (1.0 - amount) + rgb[channel] * amount;
				} else {
					source[1] = source[1] * (1.0 - amount) + target[1] * amount;
					converted = ShaderHsvToRgb(source);
				}
			}
			for (size_t channel = 0; channel < 3; channel++)
				result[channel] = converted[channel] / alpha;
			result[3] = bg[3];
			break;
		}
		case 29:   // Equal
		case 30: { // Unequal
			const bool same = bg == fg;
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
		output = result;
		return BlendPixelStatus::Ok;
	}
}
