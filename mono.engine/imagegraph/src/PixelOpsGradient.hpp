#pragma once

// Source-visible gradient geometry, key interpolation and bounded pixel transfer.
// Graph diagnostics and output allocation stay with the evaluator.

#include "PixelOpsCurveColor.hpp"

#include <engine/imagegraph/Document.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <span>

namespace engine::imagegraph::detail {
	enum class GradientStatus : uint8_t { Ok, InvalidControl, UndefinedDivision, UndefinedCmykBlack };

	struct GradientKey {
		double Time = 0.0;
		Colour Color{};
	};

	struct GradientGeometry {
		int64_t Type = 0;
		int64_t Loop = 0;
		double AngleDegrees = 0.0;
		double Radius = 0.5;
		double CenterX = 0.5;
		double CenterY = 0.5;
		double ShapeX = 1.0;
		double ShapeY = 1.0;
		bool UniformRatio = true;
		double Shift = 0.0;
		double Scale = 1.0;
	};

	struct GradientRenderControls {
		const Image *UVMap = nullptr;
		const Image *AngleMap = nullptr;
		const Image *RadiusMap = nullptr;
		const Image *ShiftMap = nullptr;
		const Image *ScaleMap = nullptr;
		double UVMix = 1.0;
		double AngleMaximum = 0.0;
		double RadiusMaximum = 0.5;
		double ShiftMaximum = 0.0;
		double ScaleMaximum = 1.0;
		double InverseAxis = 0.0;
		const Curve *InverseCurve = nullptr;
		const Curve *ProgressRemap = nullptr;
		Vector2 LevelIn{0.0, 1.0};
		Vector2 LevelOut{0.0, 1.0};
		const Curve *BrightnessCurve = nullptr;
	};

	inline GradientStatus GradientProgress(
		const GradientGeometry &geometry,
		uint32_t width,
		uint32_t height,
		double u,
		double v,
		double &progress,
		double *inverseProgress = nullptr
	) {
		if (width == 0 || height == 0 || geometry.Type < 0 || geometry.Type > 3 || geometry.Loop < 0 ||
			geometry.Loop > 2 || !std::isfinite(u) || !std::isfinite(v) ||
			!std::isfinite(geometry.AngleDegrees) || !std::isfinite(geometry.Radius) ||
			!std::isfinite(geometry.CenterX) || !std::isfinite(geometry.CenterY) ||
			!std::isfinite(geometry.ShapeX) || !std::isfinite(geometry.ShapeY) ||
			!std::isfinite(geometry.Shift) || !std::isfinite(geometry.Scale)) {
			return GradientStatus::InvalidControl;
		}
		if (geometry.Scale == 0.0 ||
			((geometry.Type == 1 || geometry.Type == 3) &&
			 (geometry.Radius == 0.0 || geometry.ShapeX == 0.0 || geometry.ShapeY == 0.0)) ||
			(geometry.Type == 2 && inverseProgress &&
			 (geometry.Radius == 0.0 || geometry.ShapeX == 0.0 || geometry.ShapeY == 0.0))) {
			return GradientStatus::UndefinedDivision;
		}
		const double pi = std::acos(-1.0);
		const double tau = 2.0 * pi;
		const double angle = geometry.AngleDegrees * pi / 180.0;
		const double centerX = geometry.CenterX / width;
		const double centerY = geometry.CenterY / height;
		const double dx = u - centerX;
		const double dy = v - centerY;
		const double aspectX = geometry.UniformRatio ? double(width) / height : 1.0;
		double value = 0.0;
		if (geometry.Type == 0) {
			value = 0.5 + dx * std::cos(angle) - dy * std::sin(angle);
			if (inverseProgress)
				*inverseProgress = 0.5 + dx * std::cos(angle + tau / 4.0) - dy * std::sin(angle + tau / 4.0);
		} else if (geometry.Type == 1) {
			const double x = dx * aspectX / geometry.ShapeX;
			const double y = dy / geometry.ShapeY;
			value = std::hypot(x, y) / geometry.Radius;
			if (inverseProgress) {
				const double theta = std::atan2(dy, dx) + angle;
				*inverseProgress = (theta - std::floor(theta / tau) * tau) / tau;
			}
		} else if (geometry.Type == 2) {
			const double theta = std::atan2(dy, dx) + angle;
			value = (theta - std::floor(theta / tau) * tau) / tau;
			if (inverseProgress) {
				const double x = dx * aspectX / geometry.ShapeX;
				const double y = dy / geometry.ShapeY;
				*inverseProgress = std::hypot(x, y) / geometry.Radius;
			}
		} else {
			const double x = dx * std::cos(angle) - dy * std::sin(angle);
			const double y = dx * std::sin(angle) + dy * std::cos(angle);
			value =
				(std::abs(x * aspectX / geometry.ShapeX) + std::abs(y / geometry.ShapeY)) / geometry.Radius;
			if (inverseProgress) *inverseProgress = std::max(std::abs(dx), std::abs(dy));
		}
		value = (value + geometry.Shift - 0.5) / geometry.Scale + 0.5;
		if (geometry.Loop == 1)
			value -= std::floor(value);
		else if (geometry.Loop == 2) {
			const double wrapped = value - 2.0 * std::floor(value / 2.0);
			value = 1.0 - std::abs(wrapped - 1.0);
		}
		if (!std::isfinite(value)) return GradientStatus::UndefinedDivision;
		progress = value;
		return GradientStatus::Ok;
	}

	inline std::array<double, 3> GradientHSV(std::array<double, 3> color) {
		const double maximum = std::max({color[0], color[1], color[2]});
		const double minimum = std::min({color[0], color[1], color[2]});
		const double delta = maximum - minimum;
		double hue = 0.0;
		if (delta != 0.0) {
			if (maximum == color[0])
				hue = (color[1] - color[2]) / delta;
			else if (maximum == color[1])
				hue = (color[2] - color[0]) / delta + 2.0;
			else
				hue = (color[0] - color[1]) / delta + 4.0;
			hue = hue / 6.0 - std::floor(hue / 6.0);
		}
		return {hue, delta / (maximum + 1e-10), maximum};
	}

	inline std::array<double, 3> GradientRGB(std::array<double, 3> hsv) {
		std::array<double, 3> rgb{};
		for (size_t channel = 0; channel < 3; channel++) {
			const double phase = channel == 0 ? 1.0 : (channel == 1 ? 2.0 / 3.0 : 1.0 / 3.0);
			const double wrapped = hsv[0] + phase - std::floor(hsv[0] + phase);
			const double p = std::abs(wrapped * 6.0 - 3.0);
			rgb[channel] = hsv[2] * std::lerp(1.0, std::clamp(p - 1.0, 0.0, 1.0), hsv[1]);
		}
		return rgb;
	}

	inline std::array<double, 3> GradientOklab(std::array<double, 3> color) {
		for (double &channel : color)
			channel = std::pow(channel, 2.2);
		return {
			std::cbrt(
				std::max(0.0, 0.4121656120 * color[0] + 0.5362752080 * color[1] + 0.0514575653 * color[2])
			),
			std::cbrt(
				std::max(0.0, 0.2118591070 * color[0] + 0.6807189584 * color[1] + 0.1074065790 * color[2])
			),
			std::cbrt(
				std::max(0.0, 0.0883097947 * color[0] + 0.2818474174 * color[1] + 0.6302613616 * color[2])
			)
		};
	}

	inline std::array<double, 3> GradientFromOklab(std::array<double, 3> lab) {
		for (double &channel : lab)
			channel = channel * channel * channel;
		std::array<double, 3> rgb{
			4.0767245293 * lab[0] - 3.3072168827 * lab[1] + 0.2307590544 * lab[2],
			-1.2681437731 * lab[0] + 2.6093323231 * lab[1] - 0.3411344290 * lab[2],
			-0.0041119885 * lab[0] - 0.7034763098 * lab[1] + 1.7068625689 * lab[2]
		};
		for (double &channel : rgb)
			channel = std::pow(std::max(0.0, channel), 1.0 / 2.2);
		return rgb;
	}

	// Modes 0..6 are the equations in the pinned sh_gradient fragment source.
	inline GradientStatus GradientColor(
		std::span<const GradientKey> keys, int64_t blendMode, double progress, std::array<double, 4> &rgba
	) {
		if (keys.empty() || keys.size() > 128 || blendMode < 0 || blendMode > 6 || !std::isfinite(progress))
			return GradientStatus::InvalidControl;
		for (size_t i = 0; i < keys.size(); i++) {
			if (!std::isfinite(keys[i].Time) || (i > 0 && keys[i].Time <= keys[i - 1].Time))
				return GradientStatus::InvalidControl;
		}
		size_t upper = 0;
		while (upper < keys.size() && keys[upper].Time < progress)
			upper++;
		if (upper == keys.size()) upper = keys.size() - 1;
		const auto channels = [](Colour color) {
			return std::array<double, 4>{
				double(color.Red), double(color.Green), double(color.Blue), double(color.Alpha)
			};
		};
		if (upper == 0 || keys[upper].Time <= progress || blendMode == 1) {
			rgba = channels(keys[upper == 0 || keys[upper].Time <= progress ? upper : upper - 1].Color);
			return GradientStatus::Ok;
		}
		const auto before = channels(keys[upper - 1].Color);
		const auto after = channels(keys[upper].Color);
		const double mix = (progress - keys[upper - 1].Time) / (keys[upper].Time - keys[upper - 1].Time);
		rgba[3] = std::lerp(before[3], after[3], mix);
		if (blendMode == 0) {
			for (size_t channel = 0; channel < 3; channel++)
				rgba[channel] = std::lerp(before[channel], after[channel], mix);
			return GradientStatus::Ok;
		}
		std::array<double, 3> first{}, second{}, blended{};
		for (size_t channel = 0; channel < 3; channel++) {
			first[channel] = before[channel] / 255.0;
			second[channel] = after[channel] / 255.0;
		}
		if (blendMode == 2 || blendMode == 5) {
			const auto a = GradientHSV(first), b = GradientHSV(second);
			const double difference = b[0] - a[0] - std::floor(b[0] - a[0]);
			double distance = 2.0 * difference - std::floor(2.0 * difference) - difference;
			if (blendMode == 5) distance -= (distance > 0.0) - (distance < 0.0);
			blended =
				GradientRGB({a[0] + distance * mix, std::lerp(a[1], b[1], mix), std::lerp(a[2], b[2], mix)});
		} else if (blendMode == 3) {
			const auto a = GradientOklab(first), b = GradientOklab(second);
			for (size_t channel = 0; channel < 3; channel++)
				blended[channel] = std::lerp(a[channel], b[channel], mix);
			blended = GradientFromOklab(blended);
		} else if (blendMode == 4) {
			for (size_t channel = 0; channel < 3; channel++)
				blended[channel] = std::pow(
					std::lerp(std::pow(first[channel], 2.2), std::pow(second[channel], 2.2), mix), 1.0 / 2.2
				);
		} else {
			const double blackA = 1.0 - std::max({first[0], first[1], first[2]});
			const double blackB = 1.0 - std::max({second[0], second[1], second[2]});
			if (blackA == 1.0 || blackB == 1.0) return GradientStatus::UndefinedCmykBlack;
			const double black = std::lerp(blackA, blackB, mix);
			for (size_t channel = 0; channel < 3; channel++) {
				const double inkA = (1.0 - first[channel] - blackA) / (1.0 - blackA);
				const double inkB = (1.0 - second[channel] - blackB) / (1.0 - blackB);
				blended[channel] = (1.0 - std::lerp(inkA, inkB, mix)) * (1.0 - black);
			}
		}
		for (size_t channel = 0; channel < 3; channel++) {
			if (!std::isfinite(blended[channel])) return GradientStatus::UndefinedDivision;
			rgba[channel] = blended[channel] * 255.0;
		}
		return GradientStatus::Ok;
	}

	// The default level and curve controls leave sampled RGB unchanged.
	inline GradientStatus RenderGradientBase(
		Image &output,
		const GradientGeometry &geometry,
		std::span<const GradientKey> keys,
		int64_t blendMode,
		const Image *mask = nullptr,
		const GradientRenderControls &controls = {}
	) {
		const auto validMap = [&](const Image *image) {
			return image == nullptr || (image->Width == output.Width && image->Height == output.Height &&
										image->Pixels.size() == uint64_t(output.Width) * output.Height * 4);
		};
		if (output.Width == 0 || output.Height == 0 || output.Width > Limits::MaximumDimension ||
			output.Height > Limits::MaximumDimension ||
			uint64_t(output.Width) * output.Height * 4 > Limits::MaximumEvaluationBytes ||
			output.Pixels.size() != uint64_t(output.Width) * output.Height * 4 || !validMap(controls.UVMap) ||
			!validMap(controls.AngleMap) || !validMap(controls.RadiusMap) || !validMap(controls.ShiftMap) ||
			!validMap(controls.ScaleMap) || !std::isfinite(controls.UVMix) ||
			!std::isfinite(controls.AngleMaximum) || !std::isfinite(controls.RadiusMaximum) ||
			!std::isfinite(controls.ShiftMaximum) || !std::isfinite(controls.ScaleMaximum) ||
			!std::isfinite(controls.InverseAxis) || !std::isfinite(controls.LevelIn.X) ||
			!std::isfinite(controls.LevelIn.Y) || !std::isfinite(controls.LevelOut.X) ||
			!std::isfinite(controls.LevelOut.Y) ||
			(controls.InverseCurve && !ValidColorCurve(*controls.InverseCurve)) ||
			(controls.ProgressRemap && !ValidColorCurve(*controls.ProgressRemap)) ||
			(controls.BrightnessCurve && !ValidColorCurve(*controls.BrightnessCurve)) ||
			(mask != nullptr &&
			 (mask->Width == 0 || mask->Height == 0 || mask->Width > Limits::MaximumDimension ||
			  mask->Height > Limits::MaximumDimension ||
			  mask->Pixels.size() != uint64_t(mask->Width) * mask->Height * 4)))
			return GradientStatus::InvalidControl;
		if (controls.LevelIn.X == controls.LevelIn.Y) return GradientStatus::UndefinedDivision;
		for (uint32_t y = 0; y < output.Height; y++) {
			for (uint32_t x = 0; x < output.Width; x++) {
				const size_t offset = (static_cast<size_t>(y) * output.Width + x) * 4;
				const double u = (x + 0.5) / output.Width;
				const double v = (y + 0.5) / output.Height;
				double mappedU = u, mappedV = v, uvAlpha = 1.0;
				if (controls.UVMap) {
					const auto &pixels = controls.UVMap->Pixels;
					mappedU = std::lerp(u, pixels[offset] / 255.0, controls.UVMix);
					mappedV = std::lerp(v, 1.0 - pixels[offset + 1] / 255.0, controls.UVMix);
					uvAlpha = pixels[offset + 3] / 255.0;
				}
				const auto mappedScalar = [&](const Image *map, double minimum, double maximum) {
					if (!map) return minimum;
					const auto &pixels = map->Pixels;
					const double weight = (pixels[offset] + pixels[offset + 1] + pixels[offset + 2]) / 765.0;
					return std::lerp(minimum, maximum, weight);
				};
				GradientGeometry sampled = geometry;
				sampled.AngleDegrees =
					mappedScalar(controls.AngleMap, geometry.AngleDegrees, controls.AngleMaximum);
				sampled.Radius = mappedScalar(controls.RadiusMap, geometry.Radius, controls.RadiusMaximum);
				sampled.Shift = mappedScalar(controls.ShiftMap, geometry.Shift, controls.ShiftMaximum);
				sampled.Scale = mappedScalar(controls.ScaleMap, geometry.Scale, controls.ScaleMaximum);
				double progress = 0.0;
				double inverse = 0.0;
				// The source adds the inverse axis before shift, scale, loop and progress remap.
				if (controls.InverseAxis != 0.0) {
					GradientGeometry raw = sampled;
					raw.Shift = 0.0;
					raw.Scale = 1.0;
					raw.Loop = 0;
					const GradientStatus rawStatus = GradientProgress(
						raw, output.Width, output.Height, mappedU, mappedV, progress, &inverse
					);
					if (rawStatus != GradientStatus::Ok) return rawStatus;
					// The source's disconnected inverse curve defaults to flat zero.
					double inverseValue = 0.0;
					if (controls.InverseCurve) {
						if (SampleColorCurveUnchecked(*controls.InverseCurve, inverse, inverseValue) !=
							CurveColorStatus::Ok)
							return GradientStatus::UndefinedDivision;
					}
					sampled.Shift += inverseValue * controls.InverseAxis;
				}
				const GradientStatus geometryStatus =
					GradientProgress(sampled, output.Width, output.Height, mappedU, mappedV, progress);
				if (geometryStatus != GradientStatus::Ok) return geometryStatus;
				if (controls.ProgressRemap &&
					SampleColorCurveUnchecked(*controls.ProgressRemap, progress, progress) !=
						CurveColorStatus::Ok)
					return GradientStatus::UndefinedDivision;
				std::array<double, 4> channels{};
				const GradientStatus colorStatus = GradientColor(keys, blendMode, progress, channels);
				if (colorStatus != GradientStatus::Ok) return colorStatus;
				channels[3] *= uvAlpha;
				if (mask != nullptr) {
					const size_t maskX = ((2ull * x + 1) * mask->Width) / (2ull * output.Width);
					const size_t maskY = ((2ull * y + 1) * mask->Height) / (2ull * output.Height);
					channels[3] *= mask->Pixels[(maskY * mask->Width + maskX) * 4 + 3] / 255.0;
				}
				for (size_t channel = 0; channel < 3; channel++) {
					channels[channel] = std::lerp(
											controls.LevelOut.X,
											controls.LevelOut.Y,
											(channels[channel] / 255.0 - controls.LevelIn.X) /
												(controls.LevelIn.Y - controls.LevelIn.X)
										) *
										255.0;
				}
				if (controls.BrightnessCurve) {
					const double mean = (channels[0] + channels[1] + channels[2]) / (3.0 * 255.0);
					double target = 0.0;
					if (SampleColorCurveUnchecked(*controls.BrightnessCurve, mean, target) !=
						CurveColorStatus::Ok)
						return GradientStatus::UndefinedDivision;
					if (mean == 0.0)
						for (size_t channel = 0; channel < 3; channel++)
							channels[channel] = target * 255.0;
					else
						for (size_t channel = 0; channel < 3; channel++)
							channels[channel] *= target / mean;
				}
				for (size_t channel = 0; channel < 4; channel++)
					if (!std::isfinite(channels[channel])) return GradientStatus::UndefinedDivision;
				for (size_t channel = 0; channel < 4; channel++)
					output.Pixels[offset + channel] =
						static_cast<uint8_t>(std::lround(std::clamp(channels[channel], 0.0, 255.0)));
			}
		}
		return GradientStatus::Ok;
	}
}
