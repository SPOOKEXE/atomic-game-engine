#pragma once

// Source-visible CPU color transfer for Curve and Colorize nodes.
// Graph masks and channel selection are applied by the evaluator after these operations.

#include <engine/imagegraph/Document.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace engine::imagegraph::detail {
	enum class CurveColorStatus : uint8_t { Ok, InvalidImage, InvalidControl, UndefinedDivision };

	struct CurveColorControls {
		Curve Brightness;
		Curve Red;
		Curve Green;
		Curve Blue;
		Curve Alpha;
	};

	struct ColorizeControls {
		double Shift = 0.0;
		Vector2 Range{0.0, 1.0};
		int64_t Overflow = 0;
		bool MultiplyAlpha = true;
		bool KeepAlpha = true;
	};

	inline Curve IdentityColorCurve() {
		return {
			{0.0, 1.0, 0.0, 0.0, 1.0, 0.0},
			{{{0.0, 0.0, 0.0, 0.0, 1.0 / 3.0, 1.0 / 3.0}, {-1.0 / 3.0, -1.0 / 3.0, 1.0, 1.0, 0.0, 0.0}}}
		};
	}

	inline bool ValidColorTransferImage(const Image &image) {
		return image.Width > 0 && image.Height > 0 && image.Width <= Limits::MaximumDimension &&
			   image.Height <= Limits::MaximumDimension &&
			   image.Pixels.size() == uint64_t(image.Width) * image.Height * 4;
	}

	inline bool ValidColorCurve(const Curve &curve) {
		// The pinned GLSL shader has 64 floats, including the six-number header.
		if (curve.Anchors.size() < 2 || curve.Anchors.size() > 9 ||
			(curve.Header[2] != 0.0 && curve.Header[2] != 1.0) || curve.Header[1] == 0.0)
			return false;
		for (double value : curve.Header)
			if (!std::isfinite(value)) return false;
		for (size_t i = 0; i < curve.Anchors.size(); i++) {
			for (double value : curve.Anchors[i])
				if (!std::isfinite(value)) return false;
			if (i > 0 && curve.Anchors[i][2] <= curve.Anchors[i - 1][2]) return false;
		}
		return true;
	}

	inline CurveColorStatus SampleColorCurveUnchecked(const Curve &curve, double input, double &output) {
		if (!std::isfinite(input)) return CurveColorStatus::InvalidControl;
		const double x = std::clamp(input / curve.Header[1] - curve.Header[0], 0.0, 1.0);
		double y = 0.0;
		if (x <= curve.Anchors.front()[2])
			y = curve.Anchors.front()[3];
		else if (x >= curve.Anchors.back()[2])
			y = curve.Anchors.back()[3];
		else if (curve.Header[2] == 1.0) {
			for (const auto &anchor : curve.Anchors) {
				if (x < anchor[2]) break;
				y = anchor[3];
			}
		} else {
			size_t segment = 0;
			while (segment + 1 < curve.Anchors.size() && x > curve.Anchors[segment + 1][2])
				segment++;
			const auto &a = curve.Anchors[segment];
			const auto &b = curve.Anchors[segment + 1];
			const double span = b[2] - a[2];
			double dx0 = a[4], dx1 = b[0];
			const double handleWidth = std::abs(dx0) + std::abs(dx1);
			if (handleWidth > span * 2.0) {
				const double ratio = span * 2.0 / handleWidth;
				dx0 *= ratio;
				dx1 *= ratio;
			}
			double t = (x - a[2]) / span;
			if (dx0 == 0.0 && a[5] == 0.0 && dx1 == 0.0 && b[1] == 0.0) {
				y = std::lerp(a[3], b[3], t);
			} else {
				const double ax = dx0 / span;
				const double bx = 1.0 + dx1 / span;
				for (int iteration = 0; iteration < 8; iteration++) {
					const double inv = 1.0 - t;
					const double currentX = 3.0 * inv * inv * t * ax + 3.0 * inv * t * t * bx + t * t * t;
					if (std::abs(currentX - (x - a[2]) / span) < 0.0001) break;
					const double derivative =
						3.0 * inv * inv * ax + 6.0 * inv * t * (bx - ax) + 3.0 * t * t * (1.0 - bx);
					if (derivative == 0.0) return CurveColorStatus::UndefinedDivision;
					t -= (currentX - (x - a[2]) / span) / derivative;
				}
				const double inv = 1.0 - t;
				y = a[3] * inv * inv * inv + (a[3] + a[5]) * 3.0 * inv * inv * t +
					(b[3] + b[1]) * 3.0 * inv * t * t + b[3] * t * t * t;
			}
		}
		double min = curve.Header[3], max = curve.Header[4];
		if (min == 0.0 && max == 0.0) max = 1.0;
		output = std::lerp(min, max, y);
		return std::isfinite(output) ? CurveColorStatus::Ok : CurveColorStatus::UndefinedDivision;
	}

	inline CurveColorStatus SampleColorCurve(const Curve &curve, double input, double &output) {
		if (!ValidColorCurve(curve)) return CurveColorStatus::InvalidControl;
		return SampleColorCurveUnchecked(curve, input, output);
	}

	inline uint8_t ColorTransferByte(double value) {
		return static_cast<uint8_t>(std::lround(std::clamp(value, 0.0, 1.0) * 255.0));
	}

	inline CurveColorStatus
	RenderCurveColor(const Image &source, Image &output, const CurveColorControls &control) {
		if (!ValidColorTransferImage(source) || !ValidColorTransferImage(output) || &source == &output ||
			source.Width != output.Width || source.Height != output.Height)
			return CurveColorStatus::InvalidImage;
		if (!ValidColorCurve(control.Brightness) || !ValidColorCurve(control.Red) ||
			!ValidColorCurve(control.Green) || !ValidColorCurve(control.Blue) ||
			!ValidColorCurve(control.Alpha))
			return CurveColorStatus::InvalidControl;
		std::vector<uint8_t> pixels(output.Pixels.size());
		for (size_t offset = 0; offset < pixels.size(); offset += 4) {
			double channels[4] = {
				double(source.Pixels[offset]) / 255.0,
				double(source.Pixels[offset + 1]) / 255.0,
				double(source.Pixels[offset + 2]) / 255.0,
				double(source.Pixels[offset + 3]) / 255.0
			};
			const Curve *curves[4] = {&control.Red, &control.Green, &control.Blue, &control.Alpha};
			for (size_t channel = 0; channel < 4; channel++) {
				const CurveColorStatus status =
					SampleColorCurveUnchecked(*curves[channel], channels[channel], channels[channel]);
				if (status != CurveColorStatus::Ok) return status;
			}
			const double brightness = (channels[0] + channels[1] + channels[2]) / 3.0;
			double target = 0.0;
			const CurveColorStatus status = SampleColorCurveUnchecked(control.Brightness, brightness, target);
			if (status != CurveColorStatus::Ok) return status;
			if (brightness == 0.0)
				channels[0] = channels[1] = channels[2] = target;
			else {
				const double ratio = target / brightness;
				for (size_t channel = 0; channel < 3; channel++)
					channels[channel] *= ratio;
			}
			for (size_t channel = 0; channel < 4; channel++)
				pixels[offset + channel] = ColorTransferByte(channels[channel]);
		}
		output.Pixels = std::move(pixels);
		return CurveColorStatus::Ok;
	}

	inline CurveColorStatus RenderColorize(
		const Image &source, Image &output, const Gradient &gradient, const ColorizeControls &control
	) {
		if (!ValidColorTransferImage(source) || !ValidColorTransferImage(output) || &source == &output ||
			source.Width != output.Width || source.Height != output.Height)
			return CurveColorStatus::InvalidImage;
		if (!std::isfinite(control.Shift) || !std::isfinite(control.Range.X) ||
			!std::isfinite(control.Range.Y) || (control.Overflow != 0 && control.Overflow != 1) ||
			gradient.Mode > 1 || gradient.Keys.empty() || gradient.Keys.size() > Limits::MaximumGradientKeys)
			return CurveColorStatus::InvalidControl;
		if (control.Range.X == control.Range.Y) return CurveColorStatus::UndefinedDivision;
		for (size_t i = 0; i < gradient.Keys.size(); i++)
			if (!std::isfinite(gradient.Keys[i].Time) ||
				(i > 0 && gradient.Keys[i].Time <= gradient.Keys[i - 1].Time))
				return CurveColorStatus::InvalidControl;
		std::vector<uint8_t> pixels(output.Pixels.size());
		for (size_t offset = 0; offset < pixels.size(); offset += 4) {
			const double alpha = double(source.Pixels[offset + 3]) / 255.0;
			double progress =
				(double(source.Pixels[offset]) / 255.0 * 0.2126 +
				 double(source.Pixels[offset + 1]) / 255.0 * 0.7152 +
				 double(source.Pixels[offset + 2]) / 255.0 * 0.0722 + control.Shift - control.Range.X) /
				(control.Range.Y - control.Range.X);
			if (!std::isfinite(progress)) return CurveColorStatus::UndefinedDivision;
			if (control.Overflow == 0)
				progress = std::clamp(progress, 0.0, 1.0);
			else
				progress -= std::floor(progress);
			if (control.MultiplyAlpha) progress *= alpha;
			size_t upper = size_t(
				std::lower_bound(
					gradient.Keys.begin(),
					gradient.Keys.end(),
					progress,
					[](const auto &key, double value) { return key.Time < value; }
				) -
				gradient.Keys.begin()
			);
			if (upper == gradient.Keys.size()) upper = gradient.Keys.size() - 1;
			const auto colour = [&](size_t key) {
				const Colour c = gradient.Keys[key].Color;
				return std::array<double, 4>{double(c.Red), double(c.Green), double(c.Blue), double(c.Alpha)};
			};
			std::array<double, 4> result{};
			if (upper == 0 || gradient.Keys[upper].Time <= progress || gradient.Mode == 1)
				result = colour(upper == 0 || gradient.Keys[upper].Time <= progress ? upper : upper - 1);
			else {
				const auto before = colour(upper - 1), after = colour(upper);
				const double t = (progress - gradient.Keys[upper - 1].Time) /
								 (gradient.Keys[upper].Time - gradient.Keys[upper - 1].Time);
				for (size_t channel = 0; channel < 4; channel++)
					result[channel] = std::lerp(before[channel], after[channel], t);
			}
			if (control.KeepAlpha) result[3] = source.Pixels[offset + 3];
			for (size_t channel = 0; channel < 4; channel++)
				pixels[offset + channel] =
					static_cast<uint8_t>(std::lround(std::clamp(result[channel], 0.0, 255.0)));
		}
		output.Pixels = std::move(pixels);
		return CurveColorStatus::Ok;
	}
}
