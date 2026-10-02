#pragma once
#include "nodes/Gradient.hpp"
namespace engine::imagegraph::detail {
	inline double SourceRoundEven(double n) {
		const double f = std::floor(n), r = n - f;
		return f + (r > .5 || (r == .5 && std::fmod(f, 2) != 0));
	}
	inline uint8_t SourceColorByte(double n) {
		return uint8_t(std::clamp(SourceRoundEven(n), 0.0, 255.0));
	}
	inline Rgb3 SourceColorHsv(Colour c) {
		const double r = c.Red / 255.0, g = c.Green / 255.0, b = c.Blue / 255.0, v = std::max({r, g, b}),
					 d = v - std::min({r, g, b});
		double h = 0, s = v == 0 ? 0 : d / v;
		if (s != 0) {
			h = r == v ? 60 * (g - b) / d : (g == v ? 120 + 60 * (b - r) / d : 240 + 60 * (r - g) / d);
			if (h < 0) h += 360;
		}
		return {h / 360, s, v};
	}
	inline std::optional<Colour> SourceGradientAt(const Gradient &gradient, double ratio) {
		if (gradient.Keys.empty()) return Colour{0, 0, 0, 0};
		if (gradient.Keys.size() == 1 || ratio <= gradient.Keys.front().Time)
			return gradient.Keys.front().Color;
		if (ratio >= gradient.Keys.back().Time) return gradient.Keys.back().Color;
		for (size_t i = 1; i < gradient.Keys.size(); ++i) {
			const auto &a = gradient.Keys[i - 1];
			const auto &b = gradient.Keys[i];
			if (b.Time < ratio) continue;
			if (b.Time == ratio) return b.Color;
			if (gradient.Mode == 1) return a.Color;
			const double t = (ratio - a.Time) / (b.Time - a.Time);
			Rgb3 mixed;
			if (gradient.Mode == 2 || gradient.Mode == 5) {
				const auto x = SourceColorHsv(a.Color), y = SourceColorHsv(b.Color);
				const double da = (y[0] - x[0]) - std::trunc(y[0] - x[0]);
				double ds = (2 * da) - std::trunc(2 * da) - da;
				if (gradient.Mode == 5) ds -= double(ds > 0) - double(ds < 0);
				const double h = SourceColorByte((x[0] + ds * t) * 255) / 255.0,
							 s = SourceColorByte((x[1] + (y[1] - x[1]) * t) * 255) / 255.0,
							 v = SourceColorByte((x[2] + (y[2] - x[2]) * t) * 255) / 255.0;
				double hh = h * 6;
				if (hh == 6) hh = 0;
				const int sector = int(std::floor(hh));
				const double f = hh - sector, p = v * (1 - s), q = v * (1 - s * f), z = v * (1 - s * (1 - f));
				switch (sector) {
				case 0:
					mixed = {v, z, p};
					break;
				case 1:
					mixed = {q, v, p};
					break;
				case 2:
					mixed = {p, v, z};
					break;
				case 3:
					mixed = {p, q, v};
					break;
				case 4:
					mixed = {z, p, v};
					break;
				default:
					mixed = {v, p, q};
				}
				for (double &channel : mixed)
					channel = std::clamp(std::floor(channel * 255 + .5), 0.0, 255.0) / 255;
			} else
				mixed = GradientMix(
					{a.Color.Red / 255.0, a.Color.Green / 255.0, a.Color.Blue / 255.0},
					{b.Color.Red / 255.0, b.Color.Green / 255.0, b.Color.Blue / 255.0},
					t,
					gradient.Mode
				);
			for (double channel : mixed)
				if (!std::isfinite(channel)) return std::nullopt;
			return Colour{
				SourceColorByte(mixed[0] * 255),
				SourceColorByte(mixed[1] * 255),
				SourceColorByte(mixed[2] * 255),
				SourceColorByte(a.Color.Alpha + (b.Color.Alpha - a.Color.Alpha) * t)
			};
		}
		return gradient.Keys.back().Color;
	}
	// gradientObject.cache() stores 129 colors. evalFast rounds its index before selecting that cache entry.
	inline std::optional<Colour> SourceCachedGradient(const Gradient &gradient, double ratio) {
		return SourceGradientAt(gradient, SourceRoundEven(ratio * 128) / 128);
	}
}
