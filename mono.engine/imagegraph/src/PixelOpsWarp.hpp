#pragma once

// Bounded CPU forms of the pinned public Displace and Polar shaders.
// Coordinates are sampled at pixel centers; the caller handles graph masks and channels.

#include <engine/imagegraph/Document.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace engine::imagegraph::detail {
	enum class WarpStatus : uint8_t { Ok, InvalidImage, InvalidControl, UndefinedRange };
	enum class WarpSampling : uint8_t { Nearest, Linear };

	struct WarpPoint {
		double X = 0.0;
		double Y = 0.0;
	};

	struct DisplaceControls {
		int64_t Mode = 0;
		// The source node resolves its authored reference unit against output dimensions first.
		WarpPoint PositionPixels{1.0, 0.0};
		double Strength = 1.0;
		double MidValue = 0.5;
		double AngleOffsetDegrees = 0.0;
		WarpSampling Sampling = WarpSampling::Nearest;
	};

	struct PolarControls {
		WarpPoint Tile{1.0, 1.0};
		WarpPoint Center{0.5, 0.5};
		double AngleDegrees = 0.0;
		bool Invert = false;
		bool SwapAxis = false;
		double Blend = 1.0;
		int64_t RadiusMode = 0;
		WarpPoint RangeDegrees{0.0, 360.0};
		double Twist = 0.0;
		WarpSampling Sampling = WarpSampling::Linear;
	};

	inline bool ValidWarpImage(const Image &image) {
		return image.Width > 0 && image.Height > 0 && image.Width <= Limits::MaximumDimension &&
			   image.Height <= Limits::MaximumDimension &&
			   image.Pixels.size() == uint64_t(image.Width) * image.Height * 4;
	}

	inline bool FiniteWarpPoint(WarpPoint value) {
		return std::isfinite(value.X) && std::isfinite(value.Y);
	}

	inline bool BoundedWarpPoint(WarpPoint value) {
		return FiniteWarpPoint(value) && std::abs(value.X) <= 1'000'000.0 && std::abs(value.Y) <= 1'000'000.0;
	}

	inline std::array<double, 4>
	SampleWarp(const Image &image, WarpPoint uv, WarpSampling sampling, bool transparentOutside) {
		if (transparentOutside && (uv.X < 0.0 || uv.Y < 0.0 || uv.X > 1.0 || uv.Y > 1.0)) return {};
		const auto pixel = [&image](int64_t x, int64_t y) {
			const size_t offset = (size_t(std::clamp<int64_t>(y, 0, image.Height - 1)) * image.Width +
								   std::clamp<int64_t>(x, 0, image.Width - 1)) *
								  4;
			return std::array<double, 4>{
				double(image.Pixels[offset]) / 255.0,
				double(image.Pixels[offset + 1]) / 255.0,
				double(image.Pixels[offset + 2]) / 255.0,
				double(image.Pixels[offset + 3]) / 255.0
			};
		};
		if (sampling == WarpSampling::Nearest)
			return pixel(int64_t(std::floor(uv.X * image.Width)), int64_t(std::floor(uv.Y * image.Height)));
		const double px = uv.X * image.Width - 0.5;
		const double py = uv.Y * image.Height - 0.5;
		const auto x0 = int64_t(std::floor(px));
		const auto y0 = int64_t(std::floor(py));
		const double fx = px - x0;
		const double fy = py - y0;
		const auto a = pixel(x0, y0);
		const auto b = pixel(x0 + 1, y0);
		const auto c = pixel(x0, y0 + 1);
		const auto d = pixel(x0 + 1, y0 + 1);
		std::array<double, 4> result{};
		for (size_t channel = 0; channel < result.size(); channel++)
			result[channel] = (a[channel] * (1.0 - fx) + b[channel] * fx) * (1.0 - fy) +
							  (c[channel] * (1.0 - fx) + d[channel] * fx) * fy;
		return result;
	}

	inline void WriteWarpPixel(Image &output, uint32_t x, uint32_t y, const std::array<double, 4> &colour) {
		const size_t offset = (size_t(y) * output.Width + x) * 4;
		for (size_t channel = 0; channel < colour.size(); channel++)
			output.Pixels[offset + channel] =
				static_cast<uint8_t>(std::lround(std::clamp(colour[channel], 0.0, 1.0) * 255.0));
	}

	inline WarpStatus
	RenderDisplace(const Image &source, const Image &map, Image &output, const DisplaceControls &control) {
		if (!ValidWarpImage(source) || !ValidWarpImage(map) || !ValidWarpImage(output) ||
			&source == &output || &map == &output || output.Width != source.Width ||
			output.Height != source.Height)
			return WarpStatus::InvalidImage;
		if ((control.Mode != 0 && control.Mode != 3) || !BoundedWarpPoint(control.PositionPixels) ||
			!std::isfinite(control.Strength) || !std::isfinite(control.MidValue) ||
			!std::isfinite(control.AngleOffsetDegrees) || std::abs(control.Strength) > 1'000'000.0 ||
			std::abs(control.MidValue) > 1'000'000.0 || std::abs(control.AngleOffsetDegrees) > 1'000'000.0 ||
			(control.Sampling != WarpSampling::Nearest && control.Sampling != WarpSampling::Linear))
			return WarpStatus::InvalidControl;
		const double radians = control.AngleOffsetDegrees * std::acos(-1.0) / 180.0;
		const double cosine = std::cos(radians);
		const double sine = std::sin(radians);
		for (uint32_t y = 0; y < output.Height; y++) {
			for (uint32_t x = 0; x < output.Width; x++) {
				const WarpPoint uv{(double(x) + 0.5) / output.Width, (double(y) + 0.5) / output.Height};
				WarpPoint shift{};
				if (control.Mode == 0) {
					const auto colour = SampleWarp(map, uv, control.Sampling, false);
					const double brightness =
						(colour[0] * 0.2126 + colour[1] * 0.7152 + colour[2] * 0.0722) * colour[3];
					const double amount = (brightness - control.MidValue) * control.Strength;
					shift = {amount * control.PositionPixels.X, amount * control.PositionPixels.Y};
				} else {
					const auto grey = [&](WarpPoint point) {
						const auto colour = SampleWarp(map, point, control.Sampling, false);
						return (colour[0] + colour[1] + colour[2]) / 3.0;
					};
					const double dx = 1.0 / output.Width;
					const double dy = 1.0 / output.Height;
					const double gx = grey({uv.X + dx, uv.Y}) - grey({uv.X - dx, uv.Y});
					const double gy = grey({uv.X, uv.Y + dy}) - grey({uv.X, uv.Y - dy});
					shift = {
						(gx * cosine - gy * sine - control.MidValue) * control.Strength,
						(gx * sine + gy * cosine - control.MidValue) * control.Strength
					};
				}
				const WarpPoint sample{
					uv.X + shift.X / (control.Mode == 0 ? output.Width : 1.0),
					uv.Y + shift.Y / (control.Mode == 0 ? output.Height : 1.0)
				};
				WriteWarpPixel(output, x, y, SampleWarp(source, sample, control.Sampling, true));
			}
		}
		return WarpStatus::Ok;
	}

	inline WarpStatus RenderPolar(const Image &source, Image &output, const PolarControls &control) {
		if (!ValidWarpImage(source) || !ValidWarpImage(output) || &source == &output ||
			output.Width != source.Width || output.Height != source.Height)
			return WarpStatus::InvalidImage;
		if (!BoundedWarpPoint(control.Tile) || !BoundedWarpPoint(control.Center) ||
			!BoundedWarpPoint(control.RangeDegrees) || !std::isfinite(control.AngleDegrees) ||
			!std::isfinite(control.Blend) || !std::isfinite(control.Twist) ||
			std::abs(control.AngleDegrees) > 1'000'000.0 || std::abs(control.Twist) > 1'000'000.0 ||
			control.Blend < 0.0 || control.Blend > 1.0 || control.RadiusMode < 0 || control.RadiusMode > 2 ||
			(control.Sampling != WarpSampling::Nearest && control.Sampling != WarpSampling::Linear))
			return WarpStatus::InvalidControl;
		const double rangeScale = (control.RangeDegrees.Y - control.RangeDegrees.X) / 360.0;
		if (rangeScale == 0.0) return WarpStatus::UndefinedRange;
		if (control.RadiusMode == 2 && !control.Invert) {
			const double centerX = control.Center.X * output.Width - 0.5;
			const double centerY = control.Center.Y * output.Height - 0.5;
			if (centerX >= 0.0 && centerX < output.Width && centerY >= 0.0 && centerY < output.Height &&
				std::floor(centerX) == centerX && std::floor(centerY) == centerY)
				return WarpStatus::UndefinedRange;
		}
		const double pi = std::acos(-1.0);
		const double angleOffset = control.AngleDegrees * pi / 180.0;
		const double rangeStart = control.RangeDegrees.X * pi / 180.0;
		const double rangeEnd = control.RangeDegrees.Y * pi / 180.0;
		const WarpPoint tile = control.SwapAxis ? WarpPoint{control.Tile.Y, control.Tile.X} : control.Tile;
		const auto fract = [](double value) { return value - std::floor(value); };
		for (uint32_t y = 0; y < output.Height; y++) {
			for (uint32_t x = 0; x < output.Width; x++) {
				const WarpPoint uv{(double(x) + 0.5) / output.Width, (double(y) + 0.5) / output.Height};
				WarpPoint coordinate{};
				double distance = 0.0;
				double angle = 0.0;
				if (!control.Invert) {
					distance =
						std::hypot(uv.X - control.Center.X, uv.Y - control.Center.Y) / (std::sqrt(2.0) * 0.5);
					angle = std::atan2(uv.Y - control.Center.Y, control.Center.X - uv.X) + pi;
				} else {
					distance = uv.X * 0.5;
					angle = uv.Y * 2.0 * pi;
				}
				if (control.RadiusMode == 1)
					distance = std::sqrt(distance);
				else if (control.RadiusMode == 2)
					distance = std::log(distance);
				angle = (angle - rangeStart) / rangeScale;
				if (angle < rangeStart || angle > rangeEnd) {
					WriteWarpPixel(output, x, y, {});
					continue;
				}
				angle -= angleOffset;
				angle += control.Twist * distance;
				if (!control.Invert) {
					coordinate = {fract(distance * tile.X), fract(angle / (2.0 * pi) * tile.Y)};
				} else {
					coordinate = {
						fract(control.Center.X + std::cos(angle) * distance * tile.X),
						fract(control.Center.Y + std::sin(angle) * distance * tile.Y)
					};
				}
				if (control.SwapAxis) std::swap(coordinate.X, coordinate.Y);
				const WarpPoint sample{
					uv.X * (1.0 - control.Blend) + coordinate.X * control.Blend,
					uv.Y * (1.0 - control.Blend) + coordinate.Y * control.Blend
				};
				WriteWarpPixel(output, x, y, SampleWarp(source, sample, control.Sampling, false));
			}
		}
		return WarpStatus::Ok;
	}
}
