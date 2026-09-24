#pragma once

// Source-visible CPU paths for Offset, simple Threshold, and nonpalette Posterize.
// The graph evaluator owns links, masks, dimensions, and diagnostics.

#include <engine/imagegraph/Document.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace engine::imagegraph::detail {
	enum class BasicFilterStatus : uint8_t { Ok, InvalidControl, InvalidImage, UndefinedDivision };

	inline bool SameImageShape(const Image &source, const Image &output) {
		const uint64_t pixels = uint64_t(source.Width) * source.Height * 4;
		return source.Width > 0 && source.Height > 0 && source.Width == output.Width &&
			   source.Height == output.Height && source.Pixels.size() == pixels &&
			   output.Pixels.size() == pixels;
	}

	inline uint8_t FilterByte(double value) {
		return static_cast<uint8_t>(std::lround(std::clamp(value, 0.0, 1.0) * 255.0));
	}

	inline BasicFilterStatus
	OffsetImage(const Image &source, Image &output, double xOffset, double yOffset, double angleDegrees) {
		if (!SameImageShape(source, output)) return BasicFilterStatus::InvalidImage;
		if (!std::isfinite(xOffset) || !std::isfinite(yOffset) || !std::isfinite(angleDegrees))
			return BasicFilterStatus::InvalidControl;
		const double angle = angleDegrees * std::acos(-1.0) / 180.0;
		const double cosine = std::cos(angle);
		const double sine = std::sin(angle);
		const double scale = 1.0 / std::max(std::abs(cosine), std::abs(sine));
		for (uint32_t y = 0; y < output.Height; y++) {
			for (uint32_t x = 0; x < output.Width; x++) {
				const double u = (x + 0.5) / output.Width;
				const double v = (y + 0.5) / output.Height;
				const double sampledU = u - scale * (xOffset * cosine - yOffset * sine);
				const double sampledV = v - scale * (xOffset * sine + yOffset * cosine);
				const double wrapU = sampledU - std::floor(sampledU);
				const double wrapV = sampledV - std::floor(sampledV);
				const uint32_t sampleX = std::min(uint32_t(wrapU * source.Width), source.Width - 1);
				const uint32_t sampleY = std::min(uint32_t(wrapV * source.Height), source.Height - 1);
				const size_t srcIndex = (size_t(sampleY) * source.Width + sampleX) * 4;
				const size_t dstIndex = (size_t(y) * output.Width + x) * 4;
				std::copy_n(source.Pixels.begin() + srcIndex, 4, output.Pixels.begin() + dstIndex);
			}
		}
		return BasicFilterStatus::Ok;
	}

	struct SimpleThreshold {
		bool Brightness = false;
		double BrightnessThreshold = 0.5;
		double BrightnessSmoothness = 0.0;
		bool BrightnessInvert = false;
		bool BrightnessMultiply = false;
		int64_t ApplyToAlpha = 0;
		bool Alpha = false;
		double AlphaThreshold = 0.5;
		double AlphaSmoothness = 0.0;
		bool AlphaInvert = false;
	};

	inline double ThresholdWeight(double value, double threshold, double smoothness) {
		if (smoothness == 0.0) return value < threshold ? 0.0 : 1.0;
		const double t = std::clamp((value - (threshold - smoothness)) / (2.0 * smoothness), 0.0, 1.0);
		return t * t * (3.0 - 2.0 * t);
	}

	inline BasicFilterStatus
	ThresholdImage(const Image &source, Image &output, const SimpleThreshold &control) {
		if (!SameImageShape(source, output)) return BasicFilterStatus::InvalidImage;
		if (!std::isfinite(control.BrightnessThreshold) || !std::isfinite(control.BrightnessSmoothness) ||
			!std::isfinite(control.AlphaThreshold) || !std::isfinite(control.AlphaSmoothness) ||
			control.BrightnessSmoothness < 0.0 || control.AlphaSmoothness < 0.0 || control.ApplyToAlpha < 0 ||
			control.ApplyToAlpha > 2)
			return BasicFilterStatus::InvalidControl;
		for (size_t index = 0; index < source.Pixels.size(); index += 4) {
			std::array<double, 4> base{};
			std::array<double, 4> result{};
			for (size_t channel = 0; channel < 4; channel++)
				base[channel] = result[channel] = source.Pixels[index + channel] / 255.0;
			if (control.Brightness) {
				const double luminance = result[0] * 0.2126 + result[1] * 0.7152 + result[2] * 0.0722;
				double weight =
					ThresholdWeight(luminance, control.BrightnessThreshold, control.BrightnessSmoothness);
				if (control.BrightnessInvert) weight = 1.0 - weight;
				if (control.ApplyToAlpha == 0)
					result[0] = result[1] = result[2] = weight;
				else if (control.ApplyToAlpha == 1)
					result[3] = weight;
				else
					result = {weight, weight, weight, weight};
				if (control.BrightnessMultiply)
					for (size_t channel = 0; channel < 4; channel++)
						result[channel] *= base[channel];
			}
			if (control.Alpha) {
				result[3] = ThresholdWeight(result[3], control.AlphaThreshold, control.AlphaSmoothness);
				if (control.AlphaInvert) result[3] = 1.0 - result[3];
			}
			for (size_t channel = 0; channel < 4; channel++)
				output.Pixels[index + channel] = FilterByte(result[channel]);
		}
		return BasicFilterStatus::Ok;
	}

	struct NonPalettePosterize {
		int64_t Steps = 4;
		double Gamma = 1.0;
		bool GlobalRange = true;
		bool PosterizeAlpha = true;
	};

	inline BasicFilterStatus
	PosterizeWithoutPalette(const Image &source, Image &output, const NonPalettePosterize &control) {
		if (!SameImageShape(source, output)) return BasicFilterStatus::InvalidImage;
		if (control.Steps < 2 || control.Steps > 16 || !std::isfinite(control.Gamma) || control.Gamma < 0.0 ||
			control.Gamma > 2.0)
			return BasicFilterStatus::InvalidControl;
		std::array<double, 3> minimum{0.0, 0.0, 0.0};
		std::array<double, 3> maximum{1.0, 1.0, 1.0};
		if (!control.GlobalRange) {
			minimum.fill(1.0);
			maximum.fill(0.0);
			for (size_t index = 0; index < source.Pixels.size(); index += 4) {
				for (size_t channel = 0; channel < 3; channel++) {
					const double value = source.Pixels[index + channel] / 255.0;
					minimum[channel] = std::min(minimum[channel], value);
					maximum[channel] = std::max(maximum[channel], value);
				}
			}
		}
		for (size_t channel = 0; channel < 3; channel++)
			if (maximum[channel] == minimum[channel]) return BasicFilterStatus::UndefinedDivision;
		const double gamma = std::max(control.Gamma, 0.0001);
		for (size_t index = 0; index < source.Pixels.size(); index += 4) {
			for (size_t channel = 0; channel < 3; channel++) {
				const double range = maximum[channel] - minimum[channel];
				const double original = source.Pixels[index + channel] / 255.0;
				const double normalized = std::clamp((original - minimum[channel]) / range, 0.0, 1.0);
				const double stepped =
					std::floor(std::pow(normalized, gamma) * control.Steps) / (control.Steps - 1);
				output.Pixels[index + channel] =
					FilterByte(minimum[channel] + std::pow(stepped, 1.0 / gamma) * range);
			}
			output.Pixels[index + 3] = control.PosterizeAlpha ? 255 : source.Pixels[index + 3];
		}
		return BasicFilterStatus::Ok;
	}
}
