#pragma once

#include "PixelOpsConversion.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace engine::imagegraph::detail {
	enum class SpatialWarpStatus : uint8_t { Ok, InvalidImage, InvalidControl, UndefinedDivision };
	enum class SpatialBoundary : uint8_t { Transparent, Black, Clamp };

	struct SpatialPixel {
		std::array<double, 4> Channel{};
	};

	inline std::optional<SpatialPixel>
	SampleSpatial(const Image &source, double u, double v, SpatialBoundary boundary) {
		if (!std::isfinite(u) || !std::isfinite(v)) return std::nullopt;
		const bool outside = u < 0.0 || v < 0.0 || u > 1.0 || v > 1.0;
		if (outside && boundary == SpatialBoundary::Transparent) return SpatialPixel{};
		if (outside && boundary == SpatialBoundary::Black) return SpatialPixel{{0.0, 0.0, 0.0, 1.0}};
		const size_t x = std::min<size_t>(
			source.Width - 1, static_cast<size_t>(std::floor(std::clamp(u, 0.0, 1.0) * source.Width))
		);
		const size_t y = std::min<size_t>(
			source.Height - 1, static_cast<size_t>(std::floor(std::clamp(v, 0.0, 1.0) * source.Height))
		);
		const size_t offset = (y * source.Width + x) * 4;
		return SpatialPixel{
			{source.Pixels[offset] / 255.0,
			 source.Pixels[offset + 1] / 255.0,
			 source.Pixels[offset + 2] / 255.0,
			 source.Pixels[offset + 3] / 255.0}
		};
	}

	inline void StoreSpatial(Image &output, size_t offset, const SpatialPixel &pixel) {
		for (size_t channel = 0; channel < 4; ++channel)
			output.Pixels[offset + channel] = ConversionByte(pixel.Channel[channel]);
	}

	inline bool ValidSpatialPair(const Image &source, const Image &output) {
		return ValidConversionPair(source, output);
	}

	struct MirrorControl {
		double PositionX = 0.5;
		double PositionY = 0.5;
		double AngleDegrees = 0.0;
		bool Flip = false;
		bool BothSide = false;
	};

	inline SpatialWarpStatus
	RenderMirror(const Image &source, Image &colored, Image &mask, const MirrorControl &control) {
		if (!ValidSpatialPair(source, colored) || !ValidSpatialPair(source, mask) || &colored == &mask)
			return SpatialWarpStatus::InvalidImage;
		if (!std::isfinite(control.PositionX) || !std::isfinite(control.PositionY) ||
			!std::isfinite(control.AngleDegrees))
			return SpatialWarpStatus::InvalidControl;
		constexpr double pi = 3.14159265358979323846;
		constexpr double tau = 2.0 * pi;
		const double angle = control.AngleDegrees * pi / 180.0 + (control.Flip ? pi : 0.0);
		if (!std::isfinite(angle)) return SpatialWarpStatus::InvalidControl;
		for (uint32_t y = 0; y < source.Height; ++y)
			for (uint32_t x = 0; x < source.Width; ++x) {
				const size_t offset = (size_t(y) * source.Width + x) * 4;
				const double u = (x + 0.5) / source.Width;
				const double v = (y + 0.5) / source.Height;
				const double px = u * source.Width - control.PositionX;
				const double py = v * source.Height - control.PositionY;
				const double rotated = std::atan2(py, px) + angle;
				const double wrapped = rotated - std::floor(rotated / tau) * tau;
				const double polar = tau - wrapped;
				if (!std::isfinite(polar)) return SpatialWarpStatus::UndefinedDivision;
				const bool mirrorSide = polar < pi;
				StoreSpatial(
					mask,
					offset,
					{{{mirrorSide ? 1.0 : 0.0, mirrorSide ? 1.0 : 0.0, mirrorSide ? 1.0 : 0.0, 1.0}}}
				);
				double sampleU = u, sampleV = v;
				if (control.BothSide || mirrorSide) {
					const double inverseAngle = 2.0 * (angle + pi) - (polar + angle);
					const double distance = std::hypot(px, py);
					sampleU = (control.PositionX + std::cos(inverseAngle) * distance) / source.Width;
					sampleV = (control.PositionY - std::sin(inverseAngle) * distance) / source.Height;
				}
				if (!std::isfinite(sampleU) || !std::isfinite(sampleV))
					return SpatialWarpStatus::UndefinedDivision;
				SpatialPixel color{};
				if (control.BothSide) color = *SampleSpatial(source, u, v, SpatialBoundary::Transparent);
				if (sampleU > 0.0 && sampleU < 1.0 && sampleV > 0.0 && sampleV < 1.0) {
					const auto reflected =
						SampleSpatial(source, sampleU, sampleV, SpatialBoundary::Transparent);
					if (!reflected) return SpatialWarpStatus::UndefinedDivision;
					for (size_t channel = 0; channel < 4; ++channel)
						color.Channel[channel] += reflected->Channel[channel];
				}
				StoreSpatial(colored, offset, color);
			}
		return SpatialWarpStatus::Ok;
	}

	struct BarrelControl {
		double CenterX = 0.5;
		double CenterY = 0.5;
		double Intensity = 1.5;
		double ScaleX = 1.0;
		double ScaleY = 1.0;
		int64_t DistanceMethod = 0;
		SpatialBoundary Boundary = SpatialBoundary::Transparent;
	};

	inline SpatialWarpStatus RenderBarrel(const Image &source, Image &output, const BarrelControl &control) {
		if (!ValidSpatialPair(source, output)) return SpatialWarpStatus::InvalidImage;
		if (!std::isfinite(control.CenterX) || !std::isfinite(control.CenterY) ||
			!std::isfinite(control.Intensity) || !std::isfinite(control.ScaleX) ||
			!std::isfinite(control.ScaleY) || control.ScaleX == 0.0 || control.ScaleY == 0.0 ||
			control.DistanceMethod < 0 || control.DistanceMethod > 3)
			return SpatialWarpStatus::InvalidControl;
		for (uint32_t y = 0; y < source.Height; ++y)
			for (uint32_t x = 0; x < source.Width; ++x) {
				const size_t offset = (size_t(y) * source.Width + x) * 4;
				const double u = (x + 0.5) / source.Width;
				const double v = (y + 0.5) / source.Height;
				const double px = (u - control.CenterX / source.Width) / control.ScaleX;
				const double py = (v - control.CenterY / source.Height) / control.ScaleY;
				const double radius = control.DistanceMethod == 0	? std::hypot(px, py)
									  : control.DistanceMethod == 1 ? std::abs(px) + std::abs(py)
									  : control.DistanceMethod == 2 ? std::max(std::abs(px), std::abs(py))
																	: std::min(std::abs(px), std::abs(py));
				if (radius == 0.0 && control.Intensity < 0.0) return SpatialWarpStatus::UndefinedDivision;
				const double distorted = std::pow(radius, control.Intensity);
				const double theta = std::atan2(py, px);
				const double sampleU = control.CenterX / source.Width + distorted * std::cos(theta);
				const double sampleV = control.CenterY / source.Height + distorted * std::sin(theta);
				const auto sampled = SampleSpatial(source, sampleU, sampleV, control.Boundary);
				if (!sampled) return SpatialWarpStatus::UndefinedDivision;
				StoreSpatial(output, offset, *sampled);
			}
		return SpatialWarpStatus::Ok;
	}

	struct ChromaticControl {
		double CenterX = 0.5;
		double CenterY = 0.5;
		double Strength = 1.0;
		double Intensity = 1.0;
		uint32_t Iterations = 1;
		SpatialBoundary Boundary = SpatialBoundary::Transparent;
	};

	inline SpatialWarpStatus
	RenderChromaticScale(const Image &source, Image &output, const ChromaticControl &control) {
		if (!ValidSpatialPair(source, output)) return SpatialWarpStatus::InvalidImage;
		if (!std::isfinite(control.CenterX) || !std::isfinite(control.CenterY) ||
			!std::isfinite(control.Strength) || !std::isfinite(control.Intensity) ||
			control.Intensity < 0.0 || control.Intensity > 4.0 || control.Iterations < 1 ||
			control.Iterations > 64)
			return SpatialWarpStatus::InvalidControl;
		for (uint32_t y = 0; y < source.Height; ++y)
			for (uint32_t x = 0; x < source.Width; ++x) {
				const size_t offset = (size_t(y) * source.Width + x) * 4;
				const double u = (x + 0.5) / source.Width;
				const double v = (y + 0.5) / source.Height;
				const double cx = (u - control.CenterX / source.Width) * 2.0;
				const double cy = (v - control.CenterY / source.Height) * 2.0;
				SpatialPixel outputPixel{};
				for (uint32_t iteration = 0; iteration < control.Iterations; ++iteration) {
					const double strength = control.Strength * (iteration + 1.0) / control.Iterations;
					const double radius2 = cx * cx + cy * cy;
					const double shiftU = radius2 * cx * strength / source.Width;
					const double shiftV = radius2 * cy * strength / source.Height;
					const auto sampledRed = SampleSpatial(source, u - shiftU, v - shiftV, control.Boundary);
					const auto sampledBlue = SampleSpatial(source, u + shiftU, v + shiftV, control.Boundary);
					const auto sampledCenter = SampleSpatial(source, u, v, control.Boundary);
					if (!sampledRed || !sampledBlue || !sampledCenter)
						return SpatialWarpStatus::UndefinedDivision;
					SpatialPixel red = *sampledRed, blue = *sampledBlue, center = *sampledCenter;
					for (SpatialPixel *pixel : {&red, &blue, &center})
						for (size_t channel = 0; channel < 3; ++channel)
							pixel->Channel[channel] *= pixel->Channel[3];
					SpatialPixel combined = center;
					const std::array<double, 4> separated{
						red.Channel[0],
						center.Channel[1],
						blue.Channel[2],
						center.Channel[3] + red.Channel[3] + blue.Channel[3]
					};
					for (size_t channel = 0; channel < 4; ++channel)
						combined.Channel[channel] =
							center.Channel[channel] +
							(separated[channel] - center.Channel[channel]) * control.Intensity;
					for (size_t channel = 0; channel < 4; ++channel)
						outputPixel.Channel[channel] += combined.Channel[channel] / control.Iterations;
				}
				StoreSpatial(output, offset, outputPixel);
			}
		return SpatialWarpStatus::Ok;
	}

	struct SpherizeControl {
		double CenterX = 0.5;
		double CenterY = 0.5;
		double PositionX = 0.0;
		double PositionY = 0.0;
		double RotationDegrees = 0.0;
		double Strength = 1.0;
		double Radius = 0.2;
		bool Normalize = false;
		double Trim = 0.0;
		double TextureOffsetX = 0.0;
		double TextureOffsetY = 0.0;
		double TextureScaleX = 1.0;
		double TextureScaleY = 1.0;
		SpatialBoundary Boundary = SpatialBoundary::Clamp;
	};

	inline SpatialWarpStatus
	RenderSpherize(const Image &source, Image &output, const SpherizeControl &control) {
		if (!ValidSpatialPair(source, output)) return SpatialWarpStatus::InvalidImage;
		const std::array<double, 11> values{
			control.CenterX,
			control.CenterY,
			control.PositionX,
			control.PositionY,
			control.RotationDegrees,
			control.Strength,
			control.Radius,
			control.Trim,
			control.TextureOffsetX,
			control.TextureOffsetY,
			control.TextureScaleX
		};
		for (const double value : values)
			if (!std::isfinite(value)) return SpatialWarpStatus::InvalidControl;
		if (!std::isfinite(control.TextureScaleY) || control.Radius == 0.0 || control.TextureScaleX == 0.0 ||
			control.TextureScaleY == 0.0)
			return SpatialWarpStatus::InvalidControl;
		constexpr double pi = 3.14159265358979323846;
		const double angle = control.RotationDegrees * pi / 180.0;
		const double cosine = std::cos(angle), sine = std::sin(angle);
		for (uint32_t y = 0; y < source.Height; ++y)
			for (uint32_t x = 0; x < source.Width; ++x) {
				const size_t offset = (size_t(y) * source.Width + x) * 4;
				const double u = (x + 0.5) / source.Width;
				const double v = (y + 0.5) / source.Height;
				const double tx = u - control.PositionX / source.Width;
				const double ty = v - control.PositionY / source.Height;
				const double centerU = control.CenterX / source.Width;
				const double centerV = control.CenterY / source.Height;
				const double dx = tx - centerU, dy = ty - centerV;
				const double rotatedX = dx * cosine + dy * sine;
				const double rotatedY = -dx * sine + dy * cosine;
				const double d = 1.0 - (rotatedX * rotatedX + rotatedY * rotatedY) / control.Radius;
				if (!std::isfinite(d)) return SpatialWarpStatus::UndefinedDivision;
				if (d <= control.Trim) {
					StoreSpatial(output, offset, {});
					continue;
				}
				double distance = std::sqrt(std::abs(d));
				if (control.Normalize) distance /= control.Radius;
				if (distance == 0.0 && control.Strength != 0.0) return SpatialWarpStatus::UndefinedDivision;
				const double sourceU = control.Strength == 0.0
										   ? rotatedX
										   : rotatedX + (rotatedX / distance - rotatedX) * control.Strength;
				const double sourceV = control.Strength == 0.0
										   ? rotatedY
										   : rotatedY + (rotatedY / distance - rotatedY) * control.Strength;
				const double sampleU =
					0.5 + (centerU + sourceU - 0.5) / control.TextureScaleX - control.TextureOffsetX;
				const double sampleV =
					0.5 + (centerV + sourceV - 0.5) / control.TextureScaleY - control.TextureOffsetY;
				const auto sampled = SampleSpatial(source, sampleU, sampleV, control.Boundary);
				if (!sampled) return SpatialWarpStatus::UndefinedDivision;
				StoreSpatial(output, offset, *sampled);
			}
		return SpatialWarpStatus::Ok;
	}

	struct DilateControl {
		double CenterX = 0.5;
		double CenterY = 0.5;
		double Strength = 1.0;
		double Radius = 0.5;
		SpatialBoundary Boundary = SpatialBoundary::Transparent;
	};

	inline SpatialWarpStatus RenderDilate(const Image &source, Image &output, const DilateControl &control) {
		if (!ValidSpatialPair(source, output)) return SpatialWarpStatus::InvalidImage;
		if (!std::isfinite(control.CenterX) || !std::isfinite(control.CenterY) ||
			!std::isfinite(control.Strength) || !std::isfinite(control.Radius) || control.Radius == 0.0)
			return SpatialWarpStatus::InvalidControl;
		for (uint32_t y = 0; y < source.Height; ++y)
			for (uint32_t x = 0; x < source.Width; ++x) {
				const size_t offset = (size_t(y) * source.Width + x) * 4;
				const double u = (x + 0.5) / source.Width;
				const double v = (y + 0.5) / source.Height;
				const double px = u * source.Width, py = v * source.Height;
				const double towardX = control.CenterX - px, towardY = control.CenterY - py;
				const double distance = std::hypot(towardX, towardY) / control.Radius;
				if (!std::isfinite(distance)) return SpatialWarpStatus::UndefinedDivision;
				const double effect = 1.0 - std::clamp(distance, 0.0, 1.0);
				const double sampleU = (px + towardX * effect * control.Strength) / source.Width;
				const double sampleV = (py + towardY * effect * control.Strength) / source.Height;
				const auto sampled = SampleSpatial(source, sampleU, sampleV, control.Boundary);
				if (!sampled) return SpatialWarpStatus::UndefinedDivision;
				StoreSpatial(output, offset, *sampled);
			}
		return SpatialWarpStatus::Ok;
	}
}
