#include "Gradient.hpp"
#include "SourcePatternAdmission.hpp"

#include <numbers>
namespace engine::imagegraph::detail {
	namespace {
		double HoneyHash(double x, double seed) {
			const double offset = (seed - 10000 * std::floor(seed / 10000)) / 100;
			return ShaderFract(
				ShaderFract(x * (.3183098861 + offset)) * ShaderFract(x * (.15915494309 + offset)) *
				265871.1723
			);
		}
		double Honey(Vector2 p, double seed, bool star) {
			p.X *= star ? 1.154700538379251 : 1.15470053837925;
			const double bx = p.Y + p.X * .5, by = p.Y - p.X * .5;
			const Vector2 cell{std::floor(p.X) + 1, std::floor(bx) + std::floor(by)};
			const auto hash = [&](double x, double y) {
				x -= 100 * std::floor(x / 100);
				y -= 100 * std::floor(y / 100);
				return HoneyHash(x * 127.1 + y * 311.7, seed);
			};
			const std::array<double, 3> m1{
				hash(cell.X + 1, cell.Y), hash(cell.X - 1, cell.Y - 1), hash(cell.X - 1, cell.Y + 1)
			};
			const std::array<double, 3> m2{
				hash(cell.X, cell.Y), hash(cell.X, cell.Y + 1), hash(cell.X, cell.Y - 1)
			};
			const std::array<double, 3> m3{
				hash(cell.X - 1, cell.Y), hash(cell.X + 1, cell.Y + 1), hash(cell.X + 1, cell.Y - 1)
			},
				m4{m2[0], m2[2], m2[1]};
			std::array<double, 3> w1{ShaderFract(p.X), 1 - ShaderFract(bx), ShaderFract(by)},
				w2{1 - ShaderFract(p.X), ShaderFract(bx), 1 - ShaderFract(by)};
			if (star)
				for (auto *weights : {&w1, &w2})
					for (auto &v : *weights)
						v = v * v * (3 - 2 * v);
			const auto dot = [](const auto &a, const auto &b) {
				return ShaderFract(a[0] * b[0] + a[1] * b[1] + a[2] * b[2]);
			};
			const double dx = ShaderFract(cell.X * .5) * 2, dy = ShaderFract(cell.Y * .5) * 2;
			const double a = dot(m3, w2) + (dot(m4, w1) - dot(m3, w2)) * dx;
			const double b = dot(m1, w1) + (dot(m2, w2) - dot(m1, w1)) * dx;
			return ShaderFract(a + (b - a) * dy);
		}
	}
	bool SourceHoneycombNoise(NodeContext &c) {
		ENGINE_PROFILE("imagegraph.source.honeycomb_noise");
		double maximumIteration = 1;
		auto observe = [&](const auto &leaf) {
			std::visit(
				[&](const auto &value) {
					using T = std::decay_t<decltype(value)>;
					const auto number = [&](double n) {
						maximumIteration = std::max(maximumIteration, std::ceil(n));
					};
					if constexpr (std::is_same_v<T, double> || std::is_same_v<T, int64_t> ||
								  std::is_same_v<T, bool>)
						number(double(value));
					else if constexpr (std::is_same_v<T, EnumValue>)
						number(double(value.Value));
					else if constexpr (std::is_same_v<T, Colour>)
						number(
							double(
								uint32_t(value.Red) | uint32_t(value.Green) << 8 |
								uint32_t(value.Blue) << 16 | uint32_t(value.Alpha) << 24
							)
						);
					else if constexpr (std::is_same_v<T, Vector2> || std::is_same_v<T, Vector3> ||
									   std::is_same_v<T, Vector4>) {
						number(value.X);
						number(value.Y);
						if constexpr (!std::is_same_v<T, Vector2>) number(value.Z);
						if constexpr (std::is_same_v<T, Vector4>) number(value.W);
					}
				},
				leaf
			);
		};
		if (auto *original = source2d::GeneratorOriginal(c, "iteration"))
			source_pattern::PatternLeaves(*original, observe);
		maximumIteration = std::max(0., maximumIteration);
		if (maximumIteration > 1023)
			return c.Fail(
				Status::LimitExceeded, "Honeycomb iteration exceeds finite shader octave profile", "iteration"
			);
		if (!source_pattern::PatternBatchAdmission(c, 128 + 512 * uint64_t(maximumIteration))) return false;
		auto format = ResolveProcessorSurfaceFormat(c, nullptr);
		if (!format) return false;
		uint32_t w = 0, h = 0;
		Vector2 dimension, position;
		if (!source_pattern::Dimensions(c, w, h, dimension) ||
			!source_pattern::Position(c, dimension, {0, 0}, position))
			return false;
		if (!c.Find("seed"))
			return c.Fail(Status::UnsupportedExecution, "Honeycomb requires an authored source seed", "seed");
		const double seed = c.Scalar("seed"), angle = c.Scalar("rotation") * std::numbers::pi / 180;
		const auto scale = c.Vec2("scale", {2, 2}), in = c.Vec2("level_in", {0, 1}),
				   out = c.Vec2("level_out", {0, 1});
		const auto mode = c.Integer("mode"), iteration = c.Integer("iteration", 1);
		if (in.X == in.Y)
			return c.Fail(Status::UnsupportedExecution, "Honeycomb Level In divides by zero", "level_in");
		if (mode < 0 || mode > 1) return c.Fail(Status::InvalidValue, "Honeycomb mode is invalid", "mode");
		Image *image = c.NewImage("surface_out", w, h, *format);
		if (!image) return false;
		for (uint32_t y = 0; y < h; ++y)
			for (uint32_t x = 0; x < w; ++x) {
				const double u = (x + .5) / w, v = (y + .5) / h;
				double alpha = 1;
				auto uv = source2d::GeneratorUv(c, u, v, alpha);
				uv.Y *= dimension.Y / dimension.X;
				const double px = uv.X - position.X / dimension.X, py = uv.Y - position.Y / dimension.Y;
				Vector2 p{
					(px * std::cos(angle) - py * std::sin(angle)) * scale.X * 4,
					(px * std::sin(angle) + py * std::cos(angle)) * scale.Y * 4
				};
				double noise = 0, amplitude = iteration > 0 ? std::pow(2., double(iteration) - 1) /
																  (std::pow(2., double(iteration)) - 1)
															: 0;
				for (int64_t oct = 0; oct < iteration; ++oct) {
					noise += Honey(p, seed, mode == 1) * amplitude;
					amplitude *= .5;
					p.X *= 2;
					p.Y *= 2;
				}
				const double n = out.X + (out.Y - out.X) * (noise - in.X) / (in.Y - in.X);
				if (!WritePixel(*image, x, y, {n, n, n, alpha}))
					return c.Fail(
						Status::UnsupportedExecution, "Honeycomb shader sample is undefined", "surface_out"
					);
				if (!source_pattern::Mask(c, *image, x, y, u, v)) return false;
			}
		return true;
	}
}
