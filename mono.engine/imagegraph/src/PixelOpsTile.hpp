#pragma once

// Pixel-center native form of the pinned public Tile shader without UV Map.
// The evaluator computes the requested output size before calling this helper.

#include <engine/imagegraph/Document.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace engine::imagegraph::detail {
	enum class TileStatus : uint8_t { Ok, InvalidControl, InvalidImage, UndefinedDivision };

	struct TilePoint {
		double X = 0.0;
		double Y = 0.0;
	};

	struct TileControls {
		TilePoint Spacing{};
		TilePoint Position{};
		double RotationRadians = 0.0;
		TilePoint Scale{1.0, 1.0};
		int64_t ShiftAxis = 0;
		double Shift = 0.0;
		int64_t Pattern = 0;
	};

	inline double TileMod(double value, double period) {
		return value - std::floor(value / period) * period;
	}

	inline TileStatus TileImage(const Image &source, Image &output, const TileControls &control) {
		const auto validImage = [](const Image &image) {
			return image.Width > 0 && image.Height > 0 &&
				   image.Pixels.size() == uint64_t(image.Width) * image.Height * 4;
		};
		const auto finite = [](TilePoint point) { return std::isfinite(point.X) && std::isfinite(point.Y); };
		if (!validImage(source) || !validImage(output)) return TileStatus::InvalidImage;
		if (!finite(control.Spacing) || !finite(control.Position) || !finite(control.Scale) ||
			!std::isfinite(control.RotationRadians) || !std::isfinite(control.Shift) ||
			control.ShiftAxis < 0 || control.ShiftAxis > 1 || control.Pattern < 0 || control.Pattern > 2 ||
			std::abs(control.Spacing.X) > 1'000'000.0 || std::abs(control.Spacing.Y) > 1'000'000.0 ||
			std::abs(control.Position.X) > 1'000'000.0 || std::abs(control.Position.Y) > 1'000'000.0 ||
			std::abs(control.Shift) > 1'000'000.0)
			return TileStatus::InvalidControl;
		const double repeatX = source.Width + control.Spacing.X;
		const double repeatY = source.Height + control.Spacing.Y;
		if (control.Scale.X == 0.0 || control.Scale.Y == 0.0 || repeatX == 0.0 || repeatY == 0.0)
			return TileStatus::UndefinedDivision;
		const double cosine = std::cos(control.RotationRadians);
		const double sine = std::sin(control.RotationRadians);
		for (uint32_t y = 0; y < output.Height; y++) {
			for (uint32_t x = 0; x < output.Width; x++) {
				const double rawX = (x + 0.5 - control.Position.X) / control.Scale.X;
				const double rawY = (y + 0.5 - control.Position.Y) / control.Scale.Y;
				double tx = rawX * cosine - rawY * sine;
				double ty = rawX * sine + rawY * cosine;
				const double firstTileX = std::floor(tx / repeatX);
				const double firstTileY = std::floor(ty / repeatY);
				if (control.ShiftAxis == 0 && TileMod(firstTileY, 2.0) >= 1.0)
					tx += source.Width * control.Shift;
				else if (control.ShiftAxis == 1 && TileMod(firstTileX, 2.0) >= 1.0)
					ty += source.Height * control.Shift;
				const double tileX = std::floor(tx / repeatX);
				const double tileY = std::floor(ty / repeatY);
				double u = (tx - tileX * repeatX) / source.Width;
				double v = (ty - tileY * repeatY) / source.Height;
				if (control.Pattern == 1) {
					if (TileMod(tileX, 2.0) >= 1.0) u = 1.0 - u;
					if (TileMod(tileY, 2.0) >= 1.0) v = 1.0 - v;
				} else if (control.Pattern == 2) {
					const double halfPi = std::acos(-1.0) / 2.0;
					const double angle = TileMod(tileY, 2.0) >= 1.0
											 ? std::acos(-1.0) + halfPi - halfPi * tileX
											 : halfPi * tileX;
					const double localX = u - 0.5;
					const double localY = v - 0.5;
					u = 0.5 + localX * std::cos(angle) - localY * std::sin(angle);
					v = 0.5 + localX * std::sin(angle) + localY * std::cos(angle);
				}
				const size_t target = (size_t(y) * output.Width + x) * 4;
				if (u < 0.0 || v < 0.0 || u > 1.0 || v > 1.0) {
					std::fill_n(output.Pixels.begin() + target, 4, uint8_t{0});
					continue;
				}
				const uint32_t sourceX = std::min(uint32_t(u * source.Width), source.Width - 1);
				const uint32_t sourceY = std::min(uint32_t(v * source.Height), source.Height - 1);
				const size_t sample = (size_t(sourceY) * source.Width + sourceX) * 4;
				std::copy_n(source.Pixels.begin() + sample, 4, output.Pixels.begin() + target);
			}
		}
		return TileStatus::Ok;
	}
}
