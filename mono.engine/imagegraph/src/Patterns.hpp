#pragma once

// Deterministic procedural patterns shared by CPU evaluation. Integer hashing keeps a
// seed's lattice identical on every platform; imagegraph.comp mirrors these in float.

#include <engine/imagegraph/Document.hpp>

#include <cmath>
#include <limits>
#include <numbers>

namespace engine::imagegraph::detail {
	inline uint32_t Mix(uint32_t x) {
		x ^= x >> 16;
		x *= 0x7feb352du;
		x ^= x >> 15;
		x *= 0x846ca68bu;
		x ^= x >> 16;
		return x;
	}
	inline uint32_t Lattice(int64_t x, int64_t y, uint32_t seed) {
		return Mix(static_cast<uint32_t>(x) ^ Mix(static_cast<uint32_t>(y) ^ Mix(seed)));
	}
	inline double Unit(uint32_t hash) {
		return hash * (1.0 / 4294967296.0);
	}
	inline double Fade(double t) {
		return t * t * t * (t * (t * 6 - 15) + 10);
	}
	inline double Lerp(double a, double b, double t) {
		return a + (b - a) * t;
	}
	inline double LatticeNoise(NoiseType type, double x, double y, uint32_t seed) {
		const double fx = std::floor(x), fy = std::floor(y);
		const auto ix = static_cast<int64_t>(fx), iy = static_cast<int64_t>(fy);
		const double tx = x - fx, ty = y - fy;
		const auto corner = [&](int64_t dx, int64_t dy) {
			const uint32_t hash = Lattice(ix + dx, iy + dy, seed);
			if (type == NoiseType::Value) return Unit(hash);
			const double angle = Unit(hash) * 2 * std::numbers::pi;
			return std::cos(angle) * (tx - dx) + std::sin(angle) * (ty - dy);
		};
		const double u = Fade(tx), v = Fade(ty);
		const double value =
			Lerp(Lerp(corner(0, 0), corner(1, 0), u), Lerp(corner(0, 1), corner(1, 1), u), v);
		// Gradient noise spans about +-sqrt(0.5); map it onto [0, 1].
		return type == NoiseType::Value ? value : 0.5 + value * std::numbers::sqrt2 * 0.5;
	}
	inline double Fractal(
		NoiseType type, double x, double y, uint32_t octaves, double lacunarity, double gain, uint32_t seed
	) {
		double sum = 0, amplitude = 1, norm = 0, frequency = 1;
		for (uint32_t octave = 0; octave < octaves; ++octave) {
			sum += amplitude * LatticeNoise(type, x * frequency, y * frequency, seed + octave * 0x9e3779b9u);
			norm += amplitude;
			amplitude *= gain;
			frequency *= lacunarity;
		}
		return norm > 0 ? sum / norm : 0;
	}

	struct CellDistances {
		double F1 = 0, F2 = 0, Edge = 0, Cell = 0;
	};
	inline void CellPoint(int64_t x, int64_t y, double randomness, uint32_t seed, double &px, double &py) {
		const uint32_t hash = Lattice(x, y, seed);
		px = 0.5 + (Unit(hash) - 0.5) * randomness;
		py = 0.5 + (Unit(Mix(hash)) - 0.5) * randomness;
	}
	inline CellDistances Cells(double x, double y, double randomness, uint32_t seed, bool edge) {
		const double fx = std::floor(x), fy = std::floor(y);
		const auto cx = static_cast<int64_t>(fx), cy = static_cast<int64_t>(fy);
		const double ox = x - fx, oy = y - fy;
		CellDistances result{std::numeric_limits<double>::max(), std::numeric_limits<double>::max(), 0, 0};
		int64_t bestX = 0, bestY = 0;
		double nearX = 0, nearY = 0;
		for (int64_t j = -1; j <= 1; ++j)
			for (int64_t i = -1; i <= 1; ++i) {
				double px, py;
				CellPoint(cx + i, cy + j, randomness, seed, px, py);
				const double dx = i + px - ox, dy = j + py - oy;
				const double distance = std::sqrt(dx * dx + dy * dy);
				if (distance < result.F1) {
					result.F2 = result.F1;
					result.F1 = distance;
					bestX = i;
					bestY = j;
					nearX = dx;
					nearY = dy;
				} else if (distance < result.F2)
					result.F2 = distance;
			}
		result.Cell = Unit(Mix(Lattice(cx + bestX, cy + bestY, seed) ^ 0x68bc21ebu));
		if (edge) {
			result.Edge = std::numeric_limits<double>::max();
			for (int64_t j = -2; j <= 2; ++j)
				for (int64_t i = -2; i <= 2; ++i) {
					if (i == bestX && j == bestY) continue;
					double px, py;
					CellPoint(cx + i, cy + j, randomness, seed, px, py);
					const double dx = i + px - ox, dy = j + py - oy;
					const double ex = dx - nearX, ey = dy - nearY;
					const double length = std::sqrt(ex * ex + ey * ey);
					if (length < 1e-9) continue;
					result.Edge =
						std::min(result.Edge, (0.5 * (nearX + dx) * ex + 0.5 * (nearY + dy) * ey) / length);
				}
		}
		return result;
	}
	inline double WaveAt(const Wave &wave, double u, double v) {
		double d;
		if (wave.Type == WaveType::Bands) {
			const double radians = std::remainder(wave.Degrees, 360.0) * std::numbers::pi / 180.0;
			d = (u * std::cos(radians) + v * std::sin(radians)) * wave.Scale;
		} else
			d = std::hypot(u - 0.5, v - 0.5) * wave.Scale;
		if (wave.Distortion != 0)
			d += wave.Distortion * (Fractal(
										NoiseType::Value,
										u * wave.DetailScale,
										v * wave.DetailScale,
										wave.Detail,
										2,
										0.5,
										wave.Seed
									) * 2 -
									1);
		d += wave.Phase;
		const double fraction = d - std::floor(d);
		switch (wave.Profile) {
		case WaveProfile::Sine:
			return 0.5 - 0.5 * std::cos(2 * std::numbers::pi * d);
		case WaveProfile::Saw:
			return fraction;
		case WaveProfile::Triangle:
			return 1 - std::abs(2 * fraction - 1);
		}
		return 0;
	}
}
