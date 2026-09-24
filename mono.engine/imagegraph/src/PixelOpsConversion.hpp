#pragma once

#include "PixelOpsCurveColor.hpp"

#include <engine/imagegraph/Document.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace engine::imagegraph::detail {
	enum class ConversionStatus : uint8_t { Ok, InvalidImage, InvalidControl, UndefinedCurve };

	inline uint8_t ConversionByte(double value) {
		return static_cast<uint8_t>(std::lround(std::clamp(value, 0.0, 1.0) * 255.0));
	}

	inline bool ValidConversionPair(const Image &source, const Image &output) {
		return ValidColorTransferImage(source) && ValidColorTransferImage(output) &&
			   source.Width == output.Width && source.Height == output.Height && &source != &output;
	}

	inline ConversionStatus RenderMonochrome(
		const Image &source, Image &output, bool blackWhite, double brightness, double contrast
	) {
		if (!ValidConversionPair(source, output)) return ConversionStatus::InvalidImage;
		if (!std::isfinite(brightness) || brightness < -1.0 || brightness > 1.0 || !std::isfinite(contrast) ||
			contrast < -1.0 || contrast > 4.0)
			return ConversionStatus::InvalidControl;
		for (size_t offset = 0; offset < source.Pixels.size(); offset += 4) {
			const double red = (source.Pixels[offset] / 255.0 + brightness) * contrast;
			const double green = (source.Pixels[offset + 1] / 255.0 + brightness) * contrast;
			const double blue = (source.Pixels[offset + 2] / 255.0 + brightness) * contrast;
			const double alpha = source.Pixels[offset + 3] / 255.0;
			const double luma = red * 0.2126 + green * 0.7152 + blue * 0.0722;
			const uint8_t gray = ConversionByte(blackWhite ? (luma > 0.5 ? 1.0 : 0.0) : luma * alpha);
			output.Pixels[offset] = gray;
			output.Pixels[offset + 1] = gray;
			output.Pixels[offset + 2] = gray;
			output.Pixels[offset + 3] = source.Pixels[offset + 3];
		}
		return ConversionStatus::Ok;
	}

	inline ConversionStatus RenderGreyAlpha(
		const Image &source, Image &output, const Curve &curve, bool invert, bool replace, Colour colour
	) {
		if (!ValidConversionPair(source, output)) return ConversionStatus::InvalidImage;
		if (!ValidColorCurve(curve)) return ConversionStatus::InvalidControl;
		for (size_t offset = 0; offset < source.Pixels.size(); offset += 4) {
			const double red = source.Pixels[offset] / 255.0;
			const double green = source.Pixels[offset + 1] / 255.0;
			const double blue = source.Pixels[offset + 2] / 255.0;
			const double originalAlpha = source.Pixels[offset + 3] / 255.0;
			double alpha = (red * 0.2126 + green * 0.7152 + blue * 0.0722) * originalAlpha;
			if (SampleColorCurveUnchecked(curve, alpha, alpha) != CurveColorStatus::Ok)
				return ConversionStatus::UndefinedCurve;
			if (invert) alpha = 1.0 - alpha;
			output.Pixels[offset] = replace ? colour.Red : source.Pixels[offset];
			output.Pixels[offset + 1] = replace ? colour.Green : source.Pixels[offset + 1];
			output.Pixels[offset + 2] = replace ? colour.Blue : source.Pixels[offset + 2];
			output.Pixels[offset + 3] = ConversionByte(alpha * (replace ? colour.Alpha / 255.0 : 1.0));
		}
		return ConversionStatus::Ok;
	}

	inline bool ValidConversionOutputs(const Image &source, const std::array<Image, 4> &outputs) {
		if (!ValidColorTransferImage(source)) return false;
		for (const Image &output : outputs)
			if (!ValidConversionPair(source, output)) return false;
		return true;
	}

	inline ConversionStatus
	RenderRgbExtract(const Image &source, std::array<Image, 4> &outputs, int64_t outputType, bool keepAlpha) {
		if (!ValidConversionOutputs(source, outputs)) return ConversionStatus::InvalidImage;
		if (outputType < 0 || outputType > 1) return ConversionStatus::InvalidControl;
		for (size_t offset = 0; offset < source.Pixels.size(); offset += 4) {
			for (size_t channel = 0; channel < 3; ++channel) {
				Image &output = outputs[channel];
				output.Pixels[offset] = outputType == 1 || channel == 0 ? source.Pixels[offset + channel] : 0;
				output.Pixels[offset + 1] =
					outputType == 1 || channel == 1 ? source.Pixels[offset + channel] : 0;
				output.Pixels[offset + 2] =
					outputType == 1 || channel == 2 ? source.Pixels[offset + channel] : 0;
				output.Pixels[offset + 3] = keepAlpha ? source.Pixels[offset + 3] : 255;
			}
			Image &alpha = outputs[3];
			const uint8_t alphaColor = outputType == 1 ? source.Pixels[offset + 3] : 255;
			alpha.Pixels[offset] = alphaColor;
			alpha.Pixels[offset + 1] = alphaColor;
			alpha.Pixels[offset + 2] = alphaColor;
			alpha.Pixels[offset + 3] = outputType == 1 ? 255 : source.Pixels[offset + 3];
		}
		return ConversionStatus::Ok;
	}

	inline ConversionStatus
	RenderHsvExtract(const Image &source, std::array<Image, 4> &outputs, int64_t colorSpace) {
		if (!ValidConversionOutputs(source, outputs)) return ConversionStatus::InvalidImage;
		if (colorSpace < 0 || colorSpace > 1) return ConversionStatus::InvalidControl;
		for (size_t offset = 0; offset < source.Pixels.size(); offset += 4) {
			const double r = source.Pixels[offset] / 255.0;
			const double g = source.Pixels[offset + 1] / 255.0;
			const double b = source.Pixels[offset + 2] / 255.0;
			const bool greenAboveBlue = g >= b;
			const double px = greenAboveBlue ? g : b;
			const double py = greenAboveBlue ? b : g;
			const double pz = greenAboveBlue ? 0.0 : -1.0;
			const double pw = greenAboveBlue ? -1.0 / 3.0 : 2.0 / 3.0;
			const bool redAboveP = r >= px;
			const double qx = redAboveP ? r : px;
			const double qy = py;
			const double qz = redAboveP ? pz : pw;
			const double qw = redAboveP ? px : r;
			const double delta = qx - std::min(qw, qy);
			const double hue = std::abs(qz + (qw - qy) / (6.0 * delta + 0.0000000001));
			const double saturation = delta / (qx + 0.0000000001);
			const double value = colorSpace == 0 ? qx : (std::max({r, g, b}) + std::min({r, g, b})) * 0.5;
			for (size_t channel = 0; channel < 3; ++channel) {
				Image &output = outputs[channel];
				const uint8_t component = ConversionByte(
					channel == 0   ? hue
					: channel == 1 ? saturation
								   : value
				);
				output.Pixels[offset] = component;
				output.Pixels[offset + 1] = component;
				output.Pixels[offset + 2] = component;
				output.Pixels[offset + 3] = source.Pixels[offset + 3];
			}
			Image &alpha = outputs[3];
			alpha.Pixels[offset] = 255;
			alpha.Pixels[offset + 1] = 255;
			alpha.Pixels[offset + 2] = 255;
			alpha.Pixels[offset + 3] = source.Pixels[offset + 3];
		}
		return ConversionStatus::Ok;
	}
}
