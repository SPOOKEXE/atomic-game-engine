#pragma once

#include "PixelOpsConversion.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace engine::imagegraph::detail {
	enum class ChannelAssemblyStatus : uint8_t { Ok, InvalidImage, InvalidControl, MissingSource };

	inline double ChannelUnit(uint8_t value) {
		return value / 255.0;
	}

	inline bool ValidChannelSources(const std::array<const Image *, 4> &inputs, const Image &output) {
		if (!ValidColorTransferImage(output)) return false;
		for (const Image *input : inputs)
			if (input && (!ValidConversionPair(*input, output))) return false;
		return true;
	}

	inline double SampleChannel(const Image &input, size_t offset, size_t channel, bool grayscale) {
		if (!grayscale) return ChannelUnit(input.Pixels[offset + channel]);
		return (ChannelUnit(input.Pixels[offset]) + ChannelUnit(input.Pixels[offset + 1]) +
				ChannelUnit(input.Pixels[offset + 2])) /
			   3.0 * ChannelUnit(input.Pixels[offset + 3]);
	}

	inline ChannelAssemblyStatus RenderRgbCombine(
		const std::array<const Image *, 4> &inputs, Image &output, int64_t samplingType, double base
	) {
		if (!inputs[0] && !inputs[1] && !inputs[2]) return ChannelAssemblyStatus::MissingSource;
		if (!ValidChannelSources(inputs, output)) return ChannelAssemblyStatus::InvalidImage;
		if (samplingType < 0 || samplingType > 1 || !std::isfinite(base) || base < 0.0 || base > 1.0)
			return ChannelAssemblyStatus::InvalidControl;
		for (size_t offset = 0; offset < output.Pixels.size(); offset += 4) {
			for (size_t channel = 0; channel < 3; ++channel)
				output.Pixels[offset + channel] = ConversionByte(
					inputs[channel] ? SampleChannel(*inputs[channel], offset, channel, samplingType == 1)
									: base
				);
			output.Pixels[offset + 3] =
				inputs[3] ? ConversionByte(SampleChannel(*inputs[3], offset, 3, samplingType == 1)) : 255;
		}
		return ChannelAssemblyStatus::Ok;
	}

	inline double HueToRgb(double m1, double m2, double hue) {
		if (hue < 0.0)
			hue += 1.0;
		else if (hue > 1.0)
			hue -= 1.0;
		if (6.0 * hue < 1.0) return m1 + (m2 - m1) * hue * 6.0;
		if (2.0 * hue < 1.0) return m2;
		if (3.0 * hue < 2.0) return m1 + (m2 - m1) * (2.0 / 3.0 - hue) * 6.0;
		return m1;
	}

	inline std::array<double, 3> HslToRgb(double hue, double saturation, double lightness) {
		if (saturation == 0.0) return {lightness, lightness, lightness};
		const double m2 = lightness <= 0.5 ? lightness * (1.0 + saturation)
										   : lightness + saturation - lightness * saturation;
		const double m1 = 2.0 * lightness - m2;
		return {HueToRgb(m1, m2, hue + 1.0 / 3.0), HueToRgb(m1, m2, hue), HueToRgb(m1, m2, hue - 1.0 / 3.0)};
	}

	inline std::array<double, 3> HsvToRgb(double hue, double saturation, double value) {
		std::array<double, 3> result{};
		constexpr std::array<double, 3> shift{1.0, 2.0 / 3.0, 1.0 / 3.0};
		for (size_t channel = 0; channel < 3; ++channel) {
			const double position = hue + shift[channel];
			const double fractional = position - std::floor(position);
			const double ramp = std::clamp(std::abs(fractional * 6.0 - 3.0) - 1.0, 0.0, 1.0);
			result[channel] = value * (1.0 + (ramp - 1.0) * saturation);
		}
		return result;
	}

	inline ChannelAssemblyStatus
	RenderHsvCombine(const std::array<const Image *, 4> &inputs, Image &output, int64_t colorSpace) {
		if (!inputs[0] && !inputs[1] && !inputs[2]) return ChannelAssemblyStatus::MissingSource;
		if (!ValidChannelSources(inputs, output)) return ChannelAssemblyStatus::InvalidImage;
		if (colorSpace < 0 || colorSpace > 1) return ChannelAssemblyStatus::InvalidControl;
		for (size_t offset = 0; offset < output.Pixels.size(); offset += 4) {
			std::array<double, 4> value{0.0, 0.0, 0.0, 1.0};
			for (size_t channel = 0; channel < 4; ++channel)
				if (inputs[channel]) value[channel] = SampleChannel(*inputs[channel], offset, channel, true);
			const auto rgb = colorSpace == 1 ? HslToRgb(value[0], value[1], value[2])
											 : HsvToRgb(value[0], value[1], value[2]);
			for (size_t channel = 0; channel < 3; ++channel)
				output.Pixels[offset + channel] = ConversionByte(rgb[channel]);
			output.Pixels[offset + 3] = ConversionByte(value[3]);
		}
		return ChannelAssemblyStatus::Ok;
	}

	inline ChannelAssemblyStatus RenderOverrideChannel(
		const Image &base, const std::array<const Image *, 4> &inputs, Image &output, int64_t samplingType
	) {
		if (!ValidConversionPair(base, output) || !ValidChannelSources(inputs, output))
			return ChannelAssemblyStatus::InvalidImage;
		if (samplingType < 0 || samplingType > 1) return ChannelAssemblyStatus::InvalidControl;
		for (size_t offset = 0; offset < output.Pixels.size(); offset += 4)
			for (size_t channel = 0; channel < 4; ++channel)
				output.Pixels[offset + channel] =
					inputs[channel]
						? ConversionByte(SampleChannel(*inputs[channel], offset, channel, samplingType == 0))
						: base.Pixels[offset + channel];
		return ChannelAssemblyStatus::Ok;
	}

	inline ChannelAssemblyStatus
	RenderMultiplyAlpha(const Image &source, Image &output, double threshold, Colour background) {
		if (!ValidConversionPair(source, output)) return ChannelAssemblyStatus::InvalidImage;
		if (!std::isfinite(threshold) || threshold < 0.0 || threshold > 1.0)
			return ChannelAssemblyStatus::InvalidControl;
		for (size_t offset = 0; offset < output.Pixels.size(); offset += 4) {
			const double alpha = ChannelUnit(source.Pixels[offset + 3]);
			if (alpha <= threshold) {
				for (size_t channel = 0; channel < 4; ++channel)
					output.Pixels[offset + channel] = 0;
				continue;
			}
			for (size_t channel = 0; channel < 3; ++channel) {
				const uint8_t bg = channel == 0	  ? background.Red
								   : channel == 1 ? background.Green
												  : background.Blue;
				const double value = ChannelUnit(source.Pixels[offset + channel]);
				output.Pixels[offset + channel] =
					ConversionByte(value * (alpha + (1.0 - alpha) * ChannelUnit(bg)));
			}
			output.Pixels[offset + 3] = 255;
		}
		return ChannelAssemblyStatus::Ok;
	}

	inline ChannelAssemblyStatus RenderGammaMap(const Image &source, Image &output, bool invert) {
		if (!ValidConversionPair(source, output)) return ChannelAssemblyStatus::InvalidImage;
		const double exponent = invert ? 2.2 : 1.0 / 2.2;
		for (size_t offset = 0; offset < output.Pixels.size(); offset += 4) {
			for (size_t channel = 0; channel < 3; ++channel)
				output.Pixels[offset + channel] =
					ConversionByte(std::pow(ChannelUnit(source.Pixels[offset + channel]), exponent));
			output.Pixels[offset + 3] = source.Pixels[offset + 3];
		}
		return ChannelAssemblyStatus::Ok;
	}
}
