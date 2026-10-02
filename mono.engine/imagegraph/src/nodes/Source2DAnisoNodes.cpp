#include "Source2DGenerator.hpp"

namespace engine::imagegraph::detail {
	bool AnisotropicNoise(NodeContext &context) {
		const int64_t mode = context.Integer("render_mode");
		if (!context.Find("seed") || (mode == 0 && !context.Find("seed_2")))
			return context.Fail(
				Status::InvalidValue, "Anisotropic noise requires its resolved source seeds", "seed"
			);
		const double seed = context.Scalar("seed"), colourSeed = context.Scalar("seed_2");
		const Vector2 input = context.Vec2("level_in", {0, 1}), output = context.Vec2("level_out", {0, 1});
		if (input.X == input.Y)
			return context.Fail(
				Status::UnsupportedExecution,
				"Anisotropic noise source level division is undefined",
				"level_in"
			);
		const auto fract = [](double value) { return value - std::floor(value); };
		const auto random1 = [&](Vector2 point, double value) {
			const double shifted = value + 453.456;
			return fract(
				std::sin(
					(point.X * 12.9898 + point.Y * 78.233) * (shifted - std::floor(shifted / 100) * 100) *
					12.588
				) *
				43758.5453123
			);
		};
		const auto random = [&](Vector2 point, double value) {
			const double first = random1(point, std::floor(value)),
						 last = random1(point, std::floor(value) + 1);
			return first + (last - first) * fract(value);
		};
		return source2d::RunGenerator(
			context, [&](uint32_t width, uint32_t height, uint32_t, uint32_t, double u, double v) {
				double alpha = 1;
				const Vector2 uv = source2d::GeneratorUv(context, u, v, alpha),
							  position = source2d::PixelPosition(context, "position", width, height);
				const double radians = MappedScalar(context, "rotation", u, v) * std::acos(-1.0) / 180;
				const Vector2 point = source2d::Rotate(
					{uv.X - position.X, uv.Y * double(height) / width - position.Y}, radians
				);
				double yy = std::floor(point.Y * MappedScalar(context, "y_amount", u, v));
				yy = (std::abs(yy) - std::floor(std::abs(yy) / 289.653) * 289.653) * (yy < 0   ? -1
																					  : yy > 0 ? 1
																							   : 0);
				double xx = (point.X + random({1, yy}, seed)) * MappedScalar(context, "x_amount", u, v);
				if (context.Boolean("tile")) xx = fract(fract(xx / 2) + 1) * 2;
				const double x0 = std::floor(xx),
							 progress =
								 output.X + (output.Y - output.X) * (xx - x0 - input.X) / (input.Y - input.X);
				double value = progress;
				if (mode == 0) {
					const double first = random({x0, yy}, colourSeed),
								 last = random({x0 + 1, yy}, colourSeed);
					value = first + (last - first) * progress;
				}
				return Rgba{value, value, value, alpha};
			}
		);
	}
} // namespace engine::imagegraph::detail
