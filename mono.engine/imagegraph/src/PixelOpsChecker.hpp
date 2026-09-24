#pragma once

#include <engine/imagegraph/Document.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace engine::imagegraph::detail {
	enum class CheckerStatus : uint8_t { Ok, InvalidImage, InvalidControl, UndefinedRange };

	struct CheckerControls {
		double Size = 0.5;
		double Aspect = 1.0;
		double AngleDegrees = 0.0;
		Vector2 Position{0.5, 0.5};
		bool Diagonal = false;
		Colour First{255, 255, 255, 255};
		Colour Second{0, 0, 0, 255};
	};

	// The direct, solid-color branch of the pinned sh_checkerboard shader.
	inline CheckerStatus RenderChecker(Image &output, const CheckerControls &control) {
		if (output.Width == 0 || output.Height == 0 || output.Width > Limits::MaximumDimension ||
			output.Height > Limits::MaximumDimension ||
			output.Pixels.size() != uint64_t(output.Width) * output.Height * 4)
			return CheckerStatus::InvalidImage;
		if (!std::isfinite(control.Size) || control.Size < 0.000001 || control.Size > 1'000'000.0 ||
			!std::isfinite(control.Aspect) || std::abs(control.Aspect) > 1'000'000.0 ||
			!std::isfinite(control.AngleDegrees) || std::abs(control.AngleDegrees) > 1'000'000.0 ||
			!std::isfinite(control.Position.X) || !std::isfinite(control.Position.Y) ||
			std::abs(control.Position.X) > 1'000'000.0 || std::abs(control.Position.Y) > 1'000'000.0)
			return CheckerStatus::InvalidControl;
		const double amount = 1.0 / control.Size;
		const double period = 1.0 / amount;
		const double xPeriodPixels = std::floor(output.Width / amount);
		const double yPeriodPixels = std::floor(output.Height / amount);
		if (control.Diagonal && (xPeriodPixels == 0.0 || yPeriodPixels == 0.0))
			return CheckerStatus::UndefinedRange;
		const double radians = control.AngleDegrees * std::acos(-1.0) / 180.0;
		const double cosine = std::cos(radians);
		const double sine = std::sin(radians);
		const auto mod = [](double value, double divisor) {
			return value - divisor * std::floor(value / divisor);
		};
		for (uint32_t y = 0; y < output.Height; ++y) {
			for (uint32_t x = 0; x < output.Width; ++x) {
				const double u = (double(x) + 0.5) / output.Width;
				const double v = (double(y) + 0.5) / output.Height;
				const double cx = (u - control.Position.X) * output.Width / output.Height;
				const double cy = (v - control.Position.Y) * control.Aspect;
				double check = 0.0;
				if (control.Diagonal) {
					const double px = mod(std::floor(output.Width * cx), xPeriodPixels);
					const double py = mod(std::floor(output.Height * cy), yPeriodPixels);
					const bool d1 = px > py;
					const bool d2 = xPeriodPixels - px > py;
					check = (d1 && d2) || (!d1 && !d2) ? 1.0 : 0.0;
				} else {
					const double rx = cx * cosine - cy * sine;
					const double ry = cx * sine + cy * cosine;
					const double cellX = std::floor(rx / period);
					const double cellY = std::floor(ry / period);
					const double centerX = (cellX + 0.5) * period;
					const double centerY = (cellY + 0.5) * period;
					const double delta =
						1.0 - (std::max(std::abs(centerX - rx), std::abs(centerY - ry)) / period + 0.5);
					check = mod(cellX + cellY, 2.0) < 0.5 ? 0.5 + delta : 0.5 - delta;
				}
				const Colour colour = check < 0.5001 ? control.First : control.Second;
				const size_t offset = (size_t(y) * output.Width + x) * 4;
				output.Pixels[offset] = colour.Red;
				output.Pixels[offset + 1] = colour.Green;
				output.Pixels[offset + 2] = colour.Blue;
				output.Pixels[offset + 3] = colour.Alpha;
			}
		}
		return CheckerStatus::Ok;
	}
}
