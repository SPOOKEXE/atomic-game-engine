#include "Curve.hpp"
#include "Gradient.hpp"
#include "Source2DGenerator.hpp"

#include <cmath>

namespace engine::imagegraph::detail {
	bool Checker(NodeContext &context) {
		const int64_t render = context.Integer("type");
		if (render < 0 || render > 2)
			return context.Fail(Status::InvalidValue, "Checker Type is invalid", "type");
		const Rgba colour1 = source2d::InputColour(context, "color_1", {255, 255, 255, 255}),
				   colour2 = source2d::InputColour(context, "color_2", {0, 0, 0, 255});
		return source2d::RunGenerator(
			context, [&](uint32_t width, uint32_t height, uint32_t, uint32_t, double u, double v) {
				double alpha = 1;
				const Vector2 uv = source2d::GeneratorUv(context, u, v, alpha);
				const Vector2 position = source2d::PixelPosition(context, "position", width, height);
				Vector2 coordinate{
					(uv.X - position.X) * width / height, (uv.Y - position.Y) * context.Scalar("aspect", 1)
				};
				const double size = MappedScalar(context, "size", u, v) * UnitScale(context, "size", width);
				double amount = width / size,
					   angle = MappedScalar(context, "angle", u, v) * std::acos(-1.0) / 180.0;
				double check = 0;
				if (!context.Boolean("diagonal") || render != 0) {
					if (context.Boolean("diagonal")) {
						angle += std::acos(-1.0) / 4;
						amount *= std::sqrt(2.0);
					}
					coordinate = source2d::Rotate(coordinate, angle);
					const double interval = 1.0 / amount;
					const double cellX = std::floor(coordinate.X / interval),
								 cellY = std::floor(coordinate.Y / interval);
					const double centreX = (cellX + 0.5) * interval, centreY = (cellY + 0.5) * interval;
					const double distance =
						0.5 - std::max(std::abs(centreX - coordinate.X), std::abs(centreY - coordinate.Y)) /
								  interval;
					const double parity = cellX + cellY - 2.0 * std::floor((cellX + cellY) / 2.0);
					check = parity < 0.5 ? 0.5 + distance : 0.5 - distance;
				} else {
					const double sideX = std::floor(width / amount), sideY = std::floor(height / amount);
					double x = std::floor(width * coordinate.X), y = std::floor(height * coordinate.Y);
					x -= sideX * std::floor(x / sideX);
					y -= sideY * std::floor(y / sideY);
					const bool first = x > y, second = sideX - x > y;
					check = first == second ? 1.0 : 0.0;
				}
				if (!std::isfinite(check)) {
					context.Fail(Status::InvalidValue, "Checker coordinates must be finite", "size");
					return Rgba{};
				}
				if (render == 0)
					check = check < 0.5001 ? 0.0 : 1.0;
				else if (render == 2) {
					const double edge = 2.0 / std::max(width, height);
					const double phase = std::clamp((check - 0.5 + edge) / (2 * edge), 0.0, 1.0);
					check = phase * phase * (3 - 2 * phase);
				}
				Rgba result{};
				for (size_t channel = 0; channel < 4; ++channel)
					result[channel] = colour1[channel] + (colour2[channel] - colour1[channel]) * check;
				result[3] *= alpha;
				return result;
			}
		);
	}
}

namespace engine::imagegraph::detail {
	bool Quasicrystal(NodeContext &context) {
		return source2d::RunGenerator(
			context, [&](uint32_t width, uint32_t height, uint32_t, uint32_t, double u, double v) {
				const double amount =
					double(width) /
					(MappedScalar(context, "scale", u, v) * UnitScale(context, "scale", width)) * 2;
				const double angle = MappedScalar(context, "angle", u, v) * std::acos(-1.0) / 180;
				const double phase = MappedScalar(context, "phase", u, v) * 2 * std::acos(-1.0);
				const Vector2 position = source2d::PixelPosition(context, "position", width, height);
				double alpha = 1;
				const Vector2 uv = source2d::GeneratorUv(context, u, v, alpha);
				Vector2 point =
					source2d::Rotate({uv.X - position.X, uv.Y * double(height) / width - position.Y}, angle);
				point.X *= amount;
				point.Y *= amount;
				const Vector2 range = context.Vec2("angle_range", {0, 180});
				double wave = 0;
				for (int i = 0; i < 4; ++i) {
					const double direction =
						(range.X + (range.Y - range.X) * double(i) / 4) * std::acos(-1.0) / 180;
					wave += std::sin(point.X * std::sin(direction) + point.Y * std::cos(direction) + phase);
				}
				const double blend = 1 + std::sin(wave * std::acos(-1.0) / 2);
				const Rgba first = source2d::InputColour(context, "color_1", {255, 255, 255, 255}),
						   second = source2d::InputColour(context, "color_2", {0, 0, 0, 255});
				Rgba result{};
				for (size_t channel = 0; channel < 4; ++channel)
					result[channel] = first[channel] + (second[channel] - first[channel]) * blend;
				result[3] *= alpha;
				return result;
			}
		);
	}
}

namespace engine::imagegraph::detail {
	bool WaveInterference(NodeContext &context) {
		const auto *gradient = std::get_if<Gradient>(context.Find("color"));
		const auto *curve = std::get_if<Curve>(context.Find("curve"));
		if (!gradient || !curve)
			return context.Fail(Status::InvalidValue, "Wave color gradient and curve are required");
		const auto colours = ReadGradient(context, "color", *gradient);
		const int64_t wave = context.Integer("wave"), post = context.Integer("post_process"),
					  blend = context.Integer("blend_mode"), pattern = context.Integer("pattern");
		const Vector2 scale = context.Vec2("scale", {4, 4}), phases = context.Vec2("phases");
		const double amplitude = context.Scalar("amplitude", 0.5),
					 angle = context.Scalar("rotation") * std::acos(-1.0) / 180;
		const auto process = [&](double value) {
			double result = 0;
			if (wave == 0)
				result = std::sin(value * 2 * std::acos(-1.0));
			else if (wave == 1)
				result = std::abs(ShaderFract(value) * 2 - 1) * 2 - 1;
			else if (wave == 2)
				result = (ShaderFract(value) >= 0.5 ? 1.0 : -1.0);
			else
				result = EvalShaderCurve(*curve, ShaderFract(value));
			result *= amplitude;
			if (post == 1)
				result = std::abs(result);
			else if (post == 2)
				result = 0.5 + result * 0.5;
			return result;
		};
		return source2d::RunGenerator(
			context, [&](uint32_t width, uint32_t height, uint32_t, uint32_t, double u, double v) {
				double alpha = 1;
				const Vector2 uv = source2d::GeneratorUv(context, u, v, alpha);
				Vector2 coordinates{};
				if (pattern == 0) {
					const Vector2 position = source2d::PixelPosition(context, "position", width, height);
					coordinates = source2d::Rotate({uv.X - position.X, uv.Y - position.Y}, angle);
				} else {
					const Vector2 first = source2d::PixelPosition(context, "polar_center_1", width, height),
								  second = source2d::PixelPosition(context, "polar_center_2", width, height);
					coordinates = {
						std::hypot(uv.X - first.X, uv.Y - first.Y),
						std::hypot(uv.X - second.X, uv.Y - second.Y)
					};
				}
				const double first = process(coordinates.X * scale.X + phases.X),
							 second = process(coordinates.Y * scale.Y + phases.Y);
				const double level = blend == 0	  ? first + second
									 : blend == 1 ? first * second
												  : std::max(first, second);
				Rgba result =
					GradientEval(colours, ShaderFract(ShaderFract(level + context.Scalar("shift")) + 1));
				result[3] *= alpha;
				return result;
			}
		);
	}
}

namespace engine::imagegraph::detail {
	bool Zigzag(NodeContext &context) {
		const int64_t type = context.Integer("type");
		const Vector2 scale = context.Vec2("scale", {1, 1});
		const Rgba first = source2d::InputColour(context, "color_1", {255, 255, 255, 255}),
				   second = source2d::InputColour(context, "color_2", {0, 0, 0, 255});
		return source2d::RunGenerator(
			context, [&](uint32_t width, uint32_t height, uint32_t, uint32_t, double u, double v) {
				const double amount =
					double(width) / (MappedScalar(context, "size", u, v) * UnitScale(context, "size", width));
				const double angle = MappedScalar(context, "angle", u, v) * std::acos(-1.0) / 180;
				double alpha = 1;
				const Vector2 uv = source2d::GeneratorUv(context, u, v, alpha),
							  position = source2d::PixelPosition(context, "position", width, height);
				Vector2 point{
					(std::floor(uv.X * width) / width - position.X) * double(width) / height,
					std::floor(uv.Y * height) / height - position.Y
				};
				point.Y += context.Scalar("offset") / amount;
				point = source2d::Rotate(point, angle);
				point.X /= scale.X;
				point.Y /= scale.Y;
				if (!std::isfinite(point.X * amount) || !std::isfinite(point.Y * amount)) {
					context.Fail(Status::InvalidValue, "Zigzag coordinates must be finite", "size");
					return Rgba{};
				}
				const double column = std::floor(point.X * amount * 2),
							 row = std::floor(point.Y * amount * 2);
				double x = ShaderFract(point.X * amount * 2), y = ShaderFract(point.Y * amount * 2);
				if (column - std::floor(column / 2) * 2 == 1) x = 1 - x;
				double level = x > y ? y + (1 - x) : y - x;
				const double index = x > y ? row + 1 : row;
				const bool flip = index - std::floor(index / 2) * 2 == 1;
				if (type == 2)
					level = level * .5 + (flip ? .5 : 0);
				else {
					if (flip) level = 1 - level;
					if (type == 0)
						level = level < context.Scalar("threshold", .5) ? 0 : 1;
					else if (type == 3) {
						const double pixel = 1.0 / std::max(width, height),
									 threshold = context.Scalar("threshold", .5);
						level = std::clamp((level - threshold + pixel) / (2 * pixel), 0.0, 1.0);
						level = level * level * (3 - 2 * level);
					}
				}
				Rgba result{};
				for (size_t channel = 0; channel < 4; ++channel)
					result[channel] = first[channel] + (second[channel] - first[channel]) * level;
				result[3] *= alpha;
				return result;
			}
		);
	}
}

namespace engine::imagegraph::detail {
	bool BoxPattern(NodeContext &context) {
		const int64_t pattern = context.Integer("pattern"), render = context.Integer("render_type"),
					  iterations = context.Integer("iteration", 4);
		if (iterations < 0)
			return context.Fail(
				Status::InvalidValue, "Box pattern iteration must be nonnegative", "iteration"
			);
		const Rgba first = source2d::InputColour(context, "color_1", {255, 255, 255, 255}),
				   second = source2d::InputColour(context, "color_2", {0, 0, 0, 255});
		return source2d::RunGenerator(
			context, [&](uint32_t width, uint32_t height, uint32_t, uint32_t, double u, double v) {
				if (pattern == 1 && uint64_t(iterations) > 64'000'000 / width / height) {
					context.Fail(
						Status::LimitExceeded, "Box pattern exceeds iteration work budget", "iteration"
					);
					return Rgba{};
				}
				const double amount = double(width) / (MappedScalar(context, "scale", u, v) *
													   UnitScale(context, "scale", width)),
							 angle = MappedScalar(context, "angle", u, v) * std::acos(-1.0) / 180;
				const Vector2 position = source2d::PixelPosition(context, "position", width, height);
				double alpha = 1;
				const Vector2 uv = source2d::GeneratorUv(context, u, v, alpha);
				Vector2 point = source2d::Rotate(
					{(uv.X - position.X) * double(width) / height, uv.Y - position.Y}, angle
				);
				point.X *= amount;
				point.Y *= amount;
				if (!std::isfinite(point.X) || !std::isfinite(point.Y)) {
					context.Fail(Status::InvalidValue, "Box pattern coordinates must be finite", "scale");
					return Rgba{};
				}
				double level = 0;
				if (pattern == 0) {
					const double thickness = MappedScalar(context, "width", u, v) / 2;
					const Vector2 corner{
						std::abs(ShaderFract(std::abs(point.X)) - .5),
						std::abs(ShaderFract(std::abs(point.Y)) - .5)
					};
					const auto edge = [&](double value) {
						if (render == 0) return value >= thickness ? 1.0 : 0.0;
						if (render == 1) return std::abs(thickness - value);
						const double pixel = 1.0 / std::max(width, height),
									 progress =
										 std::clamp((value - thickness + pixel) / (2 * pixel), 0.0, 1.0);
						return progress * progress * (3 - 2 * progress);
					};
					const double sum = edge(corner.X) + edge(corner.Y);
					level = sum - std::floor(sum / 2) * 2;
				} else {
					for (int64_t i = 0; i < iterations; ++i) {
						const double sum = std::floor(point.X) + std::floor(point.Y);
						level = (level + sum - std::floor(sum / 2) * 2) * .5;
						point.X *= .5;
						point.Y *= .5;
					}
				}
				Rgba result{};
				for (size_t channel = 0; channel < 4; ++channel)
					result[channel] = first[channel] + (second[channel] - first[channel]) * level;
				result[3] *= alpha;
				return result;
			}
		);
	}
}
