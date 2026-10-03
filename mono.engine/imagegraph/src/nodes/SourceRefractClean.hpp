#pragma once
#include "Sampler.hpp"
namespace engine::imagegraph::detail::source_refract_clean {
	struct Point {
		double X = 0, Y = 0;
	};
	inline double ColorDistance(Rgba a, Rgba b) {
		double n = 0;
		for (size_t i = 0; i < 4; ++i)
			n += (a[i] - b[i]) * (a[i] - b[i]);
		return std::sqrt(n);
	}
	inline bool Higher(Rgba a, Rgba b) {
		double x = 0, y = 0;
		for (size_t i = 0; i < 4; ++i) {
			x += a[i] * a[i];
			y += b[i] * b[i];
		}
		return std::sqrt(x) > std::sqrt(y);
	}
	template <class... T> bool Similar(Rgba a, T... rest) {
		return ((a == rest) && ...);
	}
	inline double LineDistance(Point p, Point a, Point b, Point dir) {
		const Point perp{b.Y - a.Y, -(b.X - a.X)}, to{a.X - p.X, a.Y - p.Y};
		const double length = std::sqrt(perp.X * perp.X + perp.Y * perp.Y);
		return (perp.X * dir.X + perp.Y * dir.Y > 0 ? 1. : -1.) *
			   ((perp.X / length) * to.X + (perp.Y / length) * to.Y);
	}
	inline Rgba Slice(
		Point point,
		Point main,
		Point direction,
		Rgba u,
		Rgba uf,
		Rgba uff,
		Rgba b,
		Rgba c,
		Rgba f,
		Rgba ff,
		Rgba db,
		Rgba d,
		Rgba df,
		Rgba dff,
		Rgba ddb,
		Rgba dd,
		Rgba ddf
	) {
		constexpr Rgba rejected{-1, -1, -1, -1};
		constexpr double width = 1.;
		point = {main.X * (point.X - .5) + .5, main.Y * (point.Y - .5) + .5};
		const double against = 4 * ColorDistance(f, d) + ColorDistance(uf, c) + ColorDistance(c, db) +
							   ColorDistance(ff, df) + ColorDistance(df, dd),
					 towards = 4 * ColorDistance(c, df) + ColorDistance(u, f) + ColorDistance(f, dff) +
							   ColorDistance(b, d) + ColorDistance(d, ddf);
		bool slice = against < towards || ((against < towards + .001) && !Higher(c, f));
		if (Similar(f, d, b, u) && Similar(uf, df, db) && !Similar(c, f)) slice = false;
		if (!slice) return rejected;
		bool flip = false;
		double dist = 1;
		const auto line = [&](Point a, Point z, bool reverse) {
			return LineDistance(
				point,
				{.5 + a.X * direction.X, .5 + a.Y * direction.Y},
				{.5 + z.X * direction.X, .5 + z.Y * direction.Y},
				reverse ? Point{-direction.X, -direction.Y} : direction
			);
		};
		if (Similar(f, d, db) && !Similar(f, d, b) && !Similar(uf, db)) {
			if (!(Similar(c, df) && Higher(c, f))) {
				if (Higher(c, f)) flip = true;
				if (Similar(u, f) && !Similar(c, df) && !Higher(c, u)) flip = true;
			}
			dist = flip ? width - line({1.5, -1}, {-.5, 0}, true) : line({1.5, 0}, {-.5, 1}, false);
			return dist - width / 2 <= 0 ? (ColorDistance(c, f) <= ColorDistance(c, d) ? f : d) : rejected;
		}
		if (Similar(uf, f, d) && !Similar(u, f, d) && !Similar(uf, db)) {
			if (!(Similar(c, df) && Higher(c, d))) {
				if (Higher(c, d)) flip = true;
				if (Similar(b, d) && !Similar(c, df) && !Higher(c, d)) flip = true;
			}
			dist = flip ? width - line({0, -.5}, {-1, 1.5}, true) : line({1, -.5}, {0, 1.5}, false);
			return dist - width / 2 <= 0 ? (ColorDistance(c, f) <= ColorDistance(c, d) ? f : d) : rejected;
		}
		if (Similar(f, d)) {
			if (Similar(c, df) && Higher(c, f)) {
				if (!Similar(c, dd) && !Similar(c, ff)) flip = true;
			} else {
				if (Higher(c, f)) flip = true;
				if (!Similar(c, b) && Similar(b, f, d, u)) flip = true;
			}
			if (((Similar(f, db) && Similar(u, f, df)) || (Similar(uf, d) && Similar(b, d, df))) &&
				!Similar(c, df))
				flip = true;
			dist = flip ? width - line({1, -1}, {-1, 1}, true) : line({1, 0}, {0, 1}, false);
			return dist - width / 2 <= 0 ? (ColorDistance(c, f) <= ColorDistance(c, d) ? f : d) : rejected;
		}
		if (Similar(ff, df, d) && !Similar(ff, df, c) && !Similar(uff, d)) {
			if (!(Similar(f, dff) && Higher(f, ff))) {
				if (Higher(f, ff)) flip = true;
				if (Similar(uf, ff) && !Similar(f, dff) && !Higher(f, uf)) flip = true;
			}
			dist = flip ? width - line({2.5, -1}, {.5, 0}, true) : line({2.5, 0}, {.5, 1}, false);
			return dist - width / 2 <= 0 ? (ColorDistance(f, ff) <= ColorDistance(f, df) ? ff : df)
										 : rejected;
		}
		if (Similar(f, df, dd) && !Similar(c, df, dd) && !Similar(f, ddb)) {
			if (!(Similar(d, ddf) && Higher(d, dd))) {
				if (Higher(d, dd)) flip = true;
				if (Similar(db, dd) && !Similar(d, ddf) && !Higher(d, dd)) flip = true;
			}
			dist = flip ? width - line({0, .5}, {-1, 2.5}, true) : line({1, .5}, {0, 2.5}, false);
			return dist - width / 2 <= 0 ? (ColorDistance(d, df) <= ColorDistance(d, dd) ? df : dd)
										 : rejected;
		}
		return rejected;
	}
	inline Rgba Texture(const Image &image, double u, double v) {
		const Point size{image.Width + .0001, image.Height + .0001};
		Point px{u * size.X, v * size.Y};
		const Point local{Fract(px.X), Fract(px.Y)},
			direction{std::floor(local.X + .5) * 2 - 1, std::floor(local.Y + .5) * 2 - 1};
		px = {std::ceil(px.X), std::ceil(px.Y)};
		const auto sample = [&](double x, double y) {
			return SampleNearest(image, (px.X + x * direction.X) / size.X, (px.Y + y * direction.Y) / size.Y);
		};
		// The pinned shader reads 21 neighbors, then tests Up, Back and Corner in that order.
		const auto uub = sample(-1, -2), uu = sample(0, -2), uuf = sample(1, -2), ubb = sample(-2, -2),
				   ub = sample(-1, -1), up = sample(0, -1), uf = sample(1, -1), uff = sample(2, -1),
				   bb = sample(-2, 0), b = sample(-1, 0), c = sample(0, 0), f = sample(1, 0),
				   ff = sample(2, 0), dbb = sample(-2, 1), db = sample(-1, 1), d = sample(0, 1),
				   df = sample(1, 1), dff = sample(2, 1), ddb = sample(-1, 2), dd = sample(0, 2),
				   ddf = sample(1, 2);
		const auto us =
			Slice(local, {1, -1}, direction, d, df, dff, b, c, f, ff, ub, up, uf, uff, uub, uu, uuf);
		if (us[0] >= 0) return us;
		const auto bs =
			Slice(local, {-1, 1}, direction, up, ub, ubb, f, c, b, bb, df, d, db, dbb, ddf, dd, ddb);
		if (bs[0] >= 0) return bs;
		const auto cs =
			Slice(local, {1, 1}, direction, up, uf, uff, b, c, f, ff, db, d, df, dff, ddb, dd, ddf);
		if (cs[0] >= 0) return cs;
		return c;
	}
	inline Rgba Sample(const Image &image, double u, double v, const SamplerSettings &settings) {
		if (settings.Interpolation != 6) return SampleTexture(image, u, v, settings);
		if (u >= 0 && u <= 1 && v >= 0 && v <= 1) return Texture(image, u, v);
		constexpr Rgba empty{}, black{0, 0, 0, 1};
		switch (settings.Oversample) {
		case 2:
			return black;
		case 3:
			return Texture(image, std::clamp(u, 0., 1.), std::clamp(v, 0., 1.));
		case 4:
			return Texture(image, Fract(u), Fract(v));
		case 6:
			return v < 0 || v > 1 ? empty : Texture(image, Fract(u), v);
		case 7:
			return v < 0 || v > 1 ? black : Texture(image, Fract(u), v);
		case 8:
			return Texture(image, Fract(u), std::clamp(v, 0., 1.));
		case 10:
			return u < 0 || u > 1 ? empty : Texture(image, u, Fract(v));
		case 11:
			return u < 0 || u > 1 ? black : Texture(image, u, Fract(v));
		case 12:
			return Texture(image, std::clamp(u, 0., 1.), Fract(v));
		default:
			return empty;
		}
	}
}
