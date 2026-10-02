#include "Source2DGenerator.hpp"

namespace engine::imagegraph::detail {
	bool FoldNoise(NodeContext &context) {
		const int64_t iterations = context.Integer("iteration", 2);
		if (iterations < 0)
			return context.Fail(Status::InvalidValue, "Fold iteration must be nonnegative", "iteration");
		const Vector2 scale = context.Vec2("scale", {2, 2}), density = context.Vec2("detail", {3, 1}),
					  inputLevel = context.Vec2("level_in", {0, 1}),
					  outputLevel = context.Vec2("level_out", {0, 1});
		const double stretch = context.Scalar("stretch", 2), amplitude = context.Scalar("amplitude", 1.3),
					 angle = context.Scalar("rotation") * std::acos(-1.0) / 180;
		const int64_t mode = context.Integer("mode");
		const auto level = [&](double value) {
			return outputLevel.X +
				   (outputLevel.Y - outputLevel.X) * (value - inputLevel.X) / (inputLevel.Y - inputLevel.X);
		};
		return source2d::RunGenerator(
			context, [&](uint32_t width, uint32_t height, uint32_t, uint32_t, double u, double v) {
				if (uint64_t(iterations) > 64'000'000 / width / height) {
					context.Fail(
						Status::LimitExceeded, "Fold noise exceeds iteration work budget", "iteration"
					);
					return Rgba{};
				}
				double alpha = 1;
				const Vector2 uv = source2d::GeneratorUv(context, u, v, alpha),
							  position = source2d::PixelPosition(context, "position", width, height);
				Vector2 point =
					source2d::Rotate({uv.X - position.X, uv.Y * double(height) / width - position.Y}, angle);
				point.X *= scale.X;
				point.Y *= scale.Y;
				for (int64_t i = 0; i < iterations; ++i) {
					const Vector2 cosine{
						std::cos(point.Y * density.X) / 3, std::cos(point.X * density.X + stretch) / 3
					};
					point.X += cosine.X;
					point.Y += cosine.Y;
					const Vector2 sine{
						std::sin(point.Y * density.Y + stretch) / 2, std::sin(point.X * density.Y) / 2
					};
					point.X = (point.X + sine.X) * amplitude;
					point.Y = (point.Y + sine.Y) * amplitude;
				}
				const double red = std::abs(point.X - std::floor(point.X / 2) * 2 - 1),
							 green = std::abs(point.Y - std::floor(point.Y / 2) * 2 - 1);
				if (mode == 0) {
					const double value = std::hypot(red, green);
					return Rgba{level(value), level(value), level(value), (1 + value) * alpha};
				}
				return Rgba{level(red), level(green), level(0), alpha};
			}
		);
	}
}

namespace engine::imagegraph::detail {
	bool GaussianNoise(NodeContext &context) {
		const bool conversion = context.Boolean("use_conversion");
		const Image *first = context.Input("conv_surf_1"), *second = context.Input("conv_surf_2");
		if (conversion && (!first || !second))
			return context.Fail(Status::InvalidValue, "Both conversion surfaces are required");
		if (!conversion && !context.Find("seed"))
			return context.Fail(Status::InvalidValue, "Gaussian noise requires a resolved seed", "seed");
		const double seed = context.Scalar("seed"), mean = context.Scalar("mean", 0.5),
					 variance = context.Scalar("varience", 0.5),
					 angle = context.Scalar("rotation") * std::acos(-1.0) / 180;
		const Vector2 position = context.Vec2("position"), scale = context.Vec2("scale", {1, 1}),
					  inputLevel = context.Vec2("level_in", {0, 1}),
					  outputLevel = context.Vec2("level_out", {0, 1});
		const auto random = [seed](Vector2 point) {
			const double phase = std::sin(point.X * 78.233 + point.Y * 128.852) *
								 (43758.5453 + (seed - std::floor(seed / 100000) * 100000) / 10);
			return phase - std::floor(phase);
		};
		return source2d::RunGenerator(
			context, [&](uint32_t, uint32_t, uint32_t, uint32_t, double u, double v) {
				Vector2 point = source2d::Rotate({u, v}, angle);
				point = {point.X * scale.X - position.X, point.Y * scale.Y - position.Y};
				const double firstSample =
					conversion ? SampleNearest(*first, point.X, point.Y)[0]
							   : std::max(random({point.X + 3.9613, point.Y + 1.6452}), 0.001);
				const double secondSample = conversion ? SampleNearest(*second, point.X, point.Y)[0]
													   : random({point.X + 0.1654, point.Y + 2.9873});
				const double normal = mean + std::sqrt(-2 * std::log(firstSample)) *
												 std::cos(2 * std::acos(-1.0) * secondSample) * variance;
				const double value = outputLevel.X + (outputLevel.Y - outputLevel.X) *
														 (normal - inputLevel.X) /
														 (inputLevel.Y - inputLevel.X);
				return Rgba{value, value, value, 1};
			}
		);
	}
}
