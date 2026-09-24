#pragma once

// Surface output of the pinned Shape shader for the four supported distances.
// UV remapping, distance curves, twist, shear, and additional shapes are separate paths.

#include "PixelOpsGenerate.hpp"
#include "PixelOpsShape.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace engine::imagegraph::detail {
	enum class ShapeRenderStatus : uint8_t { Ok, InvalidImage, InvalidControl, UndefinedDistance };

	struct ShapeRenderControls {
		ShapeGeometry Geometry{};
		Colour Color{255, 255, 255, 255};
		int64_t Background = 0;
		Colour BackgroundColor{0, 0, 0, 255};
		int64_t BackgroundBlend = 0;
		bool Antialias = false;
		bool Height = false;
		bool Opacity = false;
		bool MultiplyAlpha = false;
		bool MaskAlphaOnly = false;
		double LevelIn = 0.0;
		double LevelOut = 1.0;
	};
	struct ShapeAuxiliaryOutputs {
		Image *Mask = nullptr;
		Image *Height = nullptr;
		Image *UV = nullptr;
	};

	inline ShapeRenderStatus RenderShapeBase(
		Image &output,
		const ShapeRenderControls &control,
		const Image *mask = nullptr,
		const Image *backgroundSurface = nullptr,
		ShapeAuxiliaryOutputs auxiliary = {}
	) {
		const auto validImage = [](const Image &image) {
			return image.Width > 0 && image.Height > 0 &&
				   image.Pixels.size() == static_cast<size_t>(image.Width) * image.Height * 4;
		};
		if (!validImage(output) || (mask != nullptr && !validImage(*mask)) ||
			(backgroundSurface != nullptr && !validImage(*backgroundSurface)))
			return ShapeRenderStatus::InvalidImage;
		for (Image *image : {auxiliary.Mask, auxiliary.Height, auxiliary.UV}) {
			if (image != nullptr && (!validImage(*image) || image->Width != output.Width ||
									 image->Height != output.Height || image == &output))
				return ShapeRenderStatus::InvalidImage;
		}
		if ((auxiliary.Mask && (auxiliary.Mask == auxiliary.Height || auxiliary.Mask == auxiliary.UV)) ||
			(auxiliary.Height && auxiliary.Height == auxiliary.UV))
			return ShapeRenderStatus::InvalidImage;
		if (control.Background < 0 || control.Background > 2 || control.BackgroundBlend < 0 ||
			control.BackgroundBlend > 4 || !std::isfinite(control.LevelIn) ||
			!std::isfinite(control.LevelOut) || control.LevelIn == control.LevelOut)
			return ShapeRenderStatus::InvalidControl;
		// The node treats an absent BG Surface as None, and a linked one as Surface.
		const int64_t backgroundMode = backgroundSurface != nullptr && control.Background == 0 ? 2
									   : control.Background == 2 && backgroundSurface == nullptr
										   ? 0
										   : control.Background;
		const auto channels = [](Colour color) {
			return std::array<double, 4>{
				color.Red / 255.0, color.Green / 255.0, color.Blue / 255.0, color.Alpha / 255.0
			};
		};
		const std::array<double, 4> color = channels(control.Color);
		const std::array<double, 4> background = channels(control.BackgroundColor);
		const auto byte = [](double value) {
			return static_cast<uint8_t>(std::lround(std::clamp(value, 0.0, 1.0) * 255.0));
		};
		for (uint32_t y = 0; y < output.Height; y++) {
			for (uint32_t x = 0; x < output.Width; x++) {
				const double u = (x + 0.5) / output.Width;
				const double v = (y + 0.5) / output.Height;
				double distance = 0.0;
				const ShapeStatus status =
					ShapeDistance(control.Geometry, output.Width, output.Height, u, v, distance);
				if (status == ShapeStatus::InvalidControl) return ShapeRenderStatus::InvalidControl;
				if (status == ShapeStatus::UndefinedDivision) return ShapeRenderStatus::UndefinedDistance;
				double coverage = distance < 0.0 ? 1.0 : 0.0;
				if (control.Antialias) {
					const double aa = 1.0 / std::max(output.Width, output.Height);
					const double t = std::clamp((distance - aa) / (-2.0 * aa), 0.0, 1.0);
					coverage = t * t * (3.0 - 2.0 * t);
				}
				const double intensity =
					std::clamp(
						(-distance - control.LevelIn) / (control.LevelOut - control.LevelIn), 0.0, 1.0
					) *
					coverage;
				const std::array<double, 4> heightPixel =
					control.Opacity
						? std::array<double, 4>{color[0], color[1], color[2], color[3] * intensity}
						: std::array<double, 4>{
							  color[0] * intensity,
							  color[1] * intensity,
							  color[2] * intensity,
							  color[3] * coverage
						  };
				std::array<double, 4> foreground = color;
				if (control.Height) foreground = heightPixel;
				if (mask != nullptr) {
					const size_t index = SampleOffset(*mask, x, y, output.Width, output.Height);
					const double alpha = mask->Pixels[index + 3] / 255.0;
					const double amount =
						control.MaskAlphaOnly
							? alpha
							: alpha *
								  (mask->Pixels[index] + mask->Pixels[index + 1] + mask->Pixels[index + 2]) /
								  (3.0 * 255.0);
					for (double &channel : foreground)
						channel *= amount;
				}
				std::array<double, 4> result = foreground;
				if (backgroundMode == 0) {
					result[3] *= coverage;
				} else {
					std::array<double, 4> bg = background;
					if (backgroundMode == 2) {
						const size_t index =
							SampleOffset(*backgroundSurface, x, y, output.Width, output.Height);
						for (size_t channel = 0; channel < 4; channel++)
							bg[channel] = backgroundSurface->Pixels[index + channel] / 255.0;
					}
					for (size_t channel = 0; channel < 4; channel++) {
						const double fg = foreground[channel] * coverage;
						switch (control.BackgroundBlend) {
						case 0:
							result[channel] = bg[channel] * (1.0 - coverage) + foreground[channel] * coverage;
							break;
						case 1:
							result[channel] = std::max(bg[channel], fg);
							break;
						case 2:
							result[channel] = bg[channel] + fg;
							break;
						case 3:
							result[channel] = bg[channel] * fg;
							break;
						case 4:
							result[channel] = bg[channel] - fg;
							break;
						}
					}
				}
				if (control.MultiplyAlpha)
					for (size_t channel = 0; channel < 3; channel++)
						result[channel] *= result[3];
				const size_t index = (static_cast<size_t>(y) * output.Width + x) * 4;
				for (size_t channel = 0; channel < 4; channel++)
					output.Pixels[index + channel] = byte(result[channel]);
				if (auxiliary.Mask != nullptr) {
					const uint8_t value = byte(coverage);
					for (size_t channel = 0; channel < 3; channel++)
						auxiliary.Mask->Pixels[index + channel] = value;
					auxiliary.Mask->Pixels[index + 3] = 255;
				}
				if (auxiliary.Height != nullptr) {
					for (size_t channel = 0; channel < 4; channel++)
						auxiliary.Height->Pixels[index + channel] = byte(heightPixel[channel]);
				}
				if (auxiliary.UV != nullptr) {
					const double cosine = std::cos(control.Geometry.RotationRadians);
					const double sine = std::sin(control.Geometry.RotationRadians);
					const double dx = u - control.Geometry.Center.X;
					const double dy = v - control.Geometry.Center.Y;
					const double rotatedX = dx * cosine - dy * sine;
					const double rotatedY = dx * sine + dy * cosine;
					const double ratio = double(output.Width) / output.Height;
					const double coordinateX = control.Geometry.Kind == ShapeKind::Rectangle
												   ? rotatedX * ratio / control.Geometry.HalfSize.X
												   : rotatedX / control.Geometry.HalfSize.X;
					const double coordinateY = rotatedY / control.Geometry.HalfSize.Y;
					auxiliary.UV->Pixels[index] = byte(0.5 + coordinateX * 0.5);
					auxiliary.UV->Pixels[index + 1] = byte(0.5 + coordinateY * 0.5);
					auxiliary.UV->Pixels[index + 2] = 0;
					auxiliary.UV->Pixels[index + 3] = byte(foreground[3] * coverage);
				}
			}
		}
		return ShapeRenderStatus::Ok;
	}
}
