#pragma once

// Native scalar form of the pinned public Simplex shader's greyscale path.
// The shader calls its hash-based kernel iq_noise; its unused ian_noise is not run.

#include <engine/imagegraph/Document.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace engine::imagegraph::detail {
	enum class NoiseStatus : uint8_t { Ok, InvalidControl, InvalidImage, UndefinedDivision };

	struct NoisePoint {
		double X = 0.0;
		double Y = 0.0;
	};

	struct SimplexControls {
		double Seed = 0.0;
		int64_t Iterations = 1;
		bool Tile = true;
		NoisePoint Position{};
		double RotationRadians = 0.0;
		NoisePoint Scale{0.25, 0.25};
		double IterationScaling = 2.0;
		double IterationAmplitude = 0.5;
		NoisePoint LevelIn{0.0, 1.0};
		NoisePoint LevelOut{0.0, 1.0};
	};

	inline double NoiseFract(double value) {
		return value - std::floor(value);
	}
	inline double NoiseDot(NoisePoint a, NoisePoint b) {
		return a.X * b.X + a.Y * b.Y;
	}

	inline NoisePoint SimplexHash(NoisePoint point, double seed) {
		const double x = point.X * 127.1 + point.Y * 311.7;
		const double y = point.X * 269.5 + point.Y * 183.3;
		return {
			-1.0 + 2.0 * NoiseFract(std::sin(x) * (seed / 100.0)),
			-1.0 + 2.0 * NoiseFract(std::sin(y) * (seed / 100.0)),
		};
	}

	inline double SimplexKernel(NoisePoint point, double seed) {
		constexpr double k1 = 0.366025404;
		constexpr double k2 = 0.211324865;
		const double skew = (point.X + point.Y) * k1;
		const NoisePoint grid{std::floor(point.X + skew), std::floor(point.Y + skew)};
		const double unskew = (grid.X + grid.Y) * k2;
		const NoisePoint a{point.X - grid.X + unskew, point.Y - grid.Y + unskew};
		const NoisePoint corner = a.X >= a.Y ? NoisePoint{1.0, 0.0} : NoisePoint{0.0, 1.0};
		const NoisePoint b{a.X - corner.X + k2, a.Y - corner.Y + k2};
		const NoisePoint c{a.X - 1.0 + 2.0 * k2, a.Y - 1.0 + 2.0 * k2};
		const auto contribution = [&](NoisePoint delta, NoisePoint lattice) {
			const double h = std::max(0.5 - NoiseDot(delta, delta), 0.0);
			return h * h * h * h * NoiseDot(delta, SimplexHash(lattice, seed));
		};
		return 70.0 *
				   (contribution(a, grid) + contribution(b, {grid.X + corner.X, grid.Y + corner.Y}) +
					contribution(c, {grid.X + 1.0, grid.Y + 1.0})) *
				   0.7 +
			   0.6;
	}

	inline double SimplexOctaves(NoisePoint point, const SimplexControls &control) {
		point.X *= 0.5;
		point.Y *= 0.5;
		const double inverse = 1.0 / control.IterationAmplitude;
		double amplitude = std::pow(inverse, double(control.Iterations) - 1.0) /
						   (std::pow(inverse, double(control.Iterations)) - 1.0);
		double value = 0.0;
		for (int64_t octave = 0; octave < control.Iterations; octave++) {
			value += SimplexKernel(point, control.Seed) * amplitude;
			point.X *= control.IterationScaling;
			point.Y *= control.IterationScaling;
			amplitude *= control.IterationAmplitude;
		}
		return value;
	}

	inline NoisePoint SimplexCoordinate(
		NoisePoint uv, NoisePoint position, NoisePoint scale, double cosine, double sine, double dx, double dy
	) {
		const double x = uv.X - position.X + dx;
		const double y = uv.Y - position.Y + dy;
		return {(x * cosine - y * sine) * scale.X, (x * sine + y * cosine) * scale.Y};
	}

	inline NoiseStatus SimplexGrey(const SimplexControls &control, Image &output) {
		const auto finite = [](NoisePoint point) { return std::isfinite(point.X) && std::isfinite(point.Y); };
		if (output.Width == 0 || output.Height == 0 ||
			output.Pixels.size() != uint64_t(output.Width) * output.Height * 4)
			return NoiseStatus::InvalidImage;
		if (!std::isfinite(control.Seed) || !std::isfinite(control.RotationRadians) ||
			!std::isfinite(control.IterationScaling) || !std::isfinite(control.IterationAmplitude) ||
			!finite(control.Position) || !finite(control.Scale) || !finite(control.LevelIn) ||
			!finite(control.LevelOut) || control.Iterations < 1 || control.Iterations > 16 ||
			std::abs(control.Seed) > 1'000'000.0 || std::abs(control.IterationScaling) > 16.0 ||
			control.IterationAmplitude < 0.0 || control.IterationAmplitude > 1.0 ||
			std::abs(control.Position.X) > 1'000'000.0 || std::abs(control.Position.Y) > 1'000'000.0)
			return NoiseStatus::InvalidControl;
		if (control.Scale.X == 0.0 || control.Scale.Y == 0.0 || std::abs(control.Scale.X) < 0.000001 ||
			std::abs(control.Scale.Y) < 0.000001 || control.IterationAmplitude == 0.0 ||
			control.IterationAmplitude == 1.0 || (control.Tile && control.LevelIn.X == control.LevelIn.Y))
			return NoiseStatus::UndefinedDivision;
		const NoisePoint position{control.Position.X / output.Width, control.Position.Y / output.Height};
		const NoisePoint scale{output.Width / control.Scale.X, output.Height / control.Scale.Y};
		const double cosine = std::cos(control.RotationRadians);
		const double sine = std::sin(control.RotationRadians);
		for (uint32_t y = 0; y < output.Height; y++) {
			for (uint32_t x = 0; x < output.Width; x++) {
				const NoisePoint uv{
					(x + 0.5) / output.Width, ((y + 0.5) / output.Height) * output.Height / output.Width
				};
				const auto sample = [&](double dx, double dy) {
					return SimplexOctaves(
						SimplexCoordinate(uv, position, scale, cosine, sine, dx, dy), control
					);
				};
				double value = sample(0.0, 0.0);
				if (control.Tile) {
					const double s01 = sample(0.0, 1.0);
					const double s10 = sample(1.0, 0.0);
					const double s11 = sample(1.0, 1.0);
					const double alongX0 = value * uv.X + s10 * (1.0 - uv.X);
					const double alongX1 = s01 * uv.X + s11 * (1.0 - uv.X);
					value = alongX0 * uv.Y + alongX1 * (1.0 - uv.Y);
					value = control.LevelOut.X + (control.LevelOut.Y - control.LevelOut.X) *
													 (value - control.LevelIn.X) /
													 (control.LevelIn.Y - control.LevelIn.X);
				}
				if (!std::isfinite(value)) return NoiseStatus::UndefinedDivision;
				const uint8_t grey = static_cast<uint8_t>(std::lround(std::clamp(value, 0.0, 1.0) * 255.0));
				const size_t index = (size_t(y) * output.Width + x) * 4;
				output.Pixels[index] = output.Pixels[index + 1] = output.Pixels[index + 2] = grey;
				output.Pixels[index + 3] = 255;
			}
		}
		return NoiseStatus::Ok;
	}
}
