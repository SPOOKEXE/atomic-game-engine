// Bounded CPU evaluation of the pinned Kisrhombille shader and grouping controls.
#include "Families.hpp"
#include "Gradient.hpp"
#include "Source2DGenerator.hpp"

namespace engine::imagegraph::detail {
	bool SourceKisrhombille(NodeContext &context) {
		const int64_t grouping = context.Integer("grouping"), mode = context.Integer("render_type");
		if (grouping < 0 || grouping > 3 || mode < 0 || mode > 1)
			return context.Fail(Status::InvalidValue, "Kisrhombille grouping or render type is invalid");
		if (mode == 1 && !context.Find("seed"))
			return context.Fail(Status::InvalidValue, "Kisrhombille requires its resolved seed", "seed");
		const auto *value = context.Find("colors");
		const auto *keys = value ? std::get_if<Gradient>(value) : nullptr;
		if (mode == 1 && (!keys || keys->Keys.empty() || keys->Keys.size() > GRADIENT_KEY_SLOTS))
			return context.Fail(Status::InvalidValue, "Kisrhombille requires a bounded gradient", "colors");
		const GradientSampler gradient{keys};
		const double radians = context.Scalar("angle") * std::acos(-1.) / 180;
		const double cosine = std::cos(radians), sine = std::sin(radians);
		const double seed = context.Scalar("seed"), shift = context.Scalar("shift");
		if (!std::isfinite(radians) || !std::isfinite(seed) || !std::isfinite(shift))
			return context.Fail(Status::InvalidValue, "Kisrhombille controls must be finite");
		Vector2 dimension = context.Vec2("dimension", {1, 1});
		if (!context.IsLinked("dimension")) {
			const int64_t unit = context.Integer("dimension_unit", 1);
			if (unit == 1)
				dimension = {
					dimension.X * context.Project.SurfaceWidth, dimension.Y * context.Project.SurfaceHeight
				};
			else if (unit == 2) {
				const auto *mask = context.Input("mask");
				if (!mask)
					return context.Fail(Status::InvalidValue, "Mask dimensions require a mask", "mask");
				dimension = {dimension.X * mask->Width, dimension.Y * mask->Height};
			}
		}
		if (dimension.X == 0 || dimension.Y == 0)
			return context.Fail(
				Status::UnsupportedExecution,
				"Kisrhombille source dimension division is undefined",
				"dimension"
			);
		return source2d::RunGenerator(
			context, [&](uint32_t, uint32_t, uint32_t, uint32_t, double u, double v) {
				Vector2 position = context.Vec2("position", {.5, .5});
				if (!context.IsLinked("position") && context.Integer("position_unit", 1) == 1)
					position = {position.X * dimension.X, position.Y * dimension.Y};
				position = {position.X / dimension.X, position.Y / dimension.Y};
				Vector2 scale = context.Vec2("scale", {.125, .125});
				if (!context.IsLinked("scale") && context.Integer("scale_unit", 1) == 1)
					scale = {scale.X * dimension.X, scale.Y * dimension.Y};
				if (!std::isfinite(scale.X) || !std::isfinite(scale.Y) || scale.X == 0 || scale.Y == 0) {
					context.Fail(
						Status::InvalidValue, "Kisrhombille scale must be finite and nonzero", "scale"
					);
					return Rgba{};
				}
				double alpha = 1;
				Vector2 uv = source2d::GeneratorUv(context, u, v, alpha);
				uv = {uv.X - position.X, uv.Y - position.Y};
				const Vector2 point{
					(uv.X * cosine - uv.Y * sine) * dimension.X / scale.X,
					(uv.X * sine + uv.Y * cosine) * dimension.Y / scale.Y
				};
				if (!std::isfinite(point.X) || !std::isfinite(point.Y) || !std::isfinite(alpha)) {
					context.Fail(Status::InvalidValue, "Kisrhombille transformed coordinates must be finite");
					return Rgba{};
				}
				constexpr double ROOT_THREE = 1.73205080757;
				const Vector2 cellSize{ROOT_THREE + 1, ROOT_THREE};
				Vector2 cell{std::floor(point.X / cellSize.X), std::floor(point.Y / cellSize.Y)};
				Vector2 local{ShaderFract(point.X / cellSize.X), ShaderFract(point.Y / cellSize.Y)};
				const double sum = cell.X + cell.Y;
				const bool flip = sum - std::floor(sum / 2) * 2 == 1;
				if (flip) local.X = 1 - local.X;
				const bool diagonal = local.X > local.Y;
				if (diagonal) local = {1 - local.X, 1 - local.Y};
				local = {local.X * cellSize.X, local.Y * cellSize.Y};
				int index = 2;
				if (local.X < local.Y / ROOT_THREE)
					index = 0;
				else if (local.X < ROOT_THREE - local.Y / ROOT_THREE * (ROOT_THREE - 1))
					index = 1;
				double colourIndex = index;
				switch (grouping) {
				case 0:
					if (mode == 0) {
						if (index == 2) index = 0;
						if (flip) index = 1 - index;
						colourIndex = index;
					} else
						colourIndex = index * (flip ? 3.123 : 1.545);
					break;
				case 1:
					colourIndex = index == 2 ? 1 : 0;
					if (mode == 1 && diagonal && index == 2) --cell.Y;
					break;
				case 2:
					colourIndex = index == 0 ? 1 : 0;
					if (mode == 1 && index == 0) {
						if (flip) {
							if (diagonal)
								--cell.Y;
							else
								++cell.X;
						} else if (diagonal) {
							++cell.X;
							--cell.Y;
						}
					}
					break;
				case 3:
					colourIndex = index == 0 ? 1 : 0;
					if (mode == 1) {
						if (index == 0) {
							if (diagonal) --cell.Y;
						} else if (diagonal)
							colourIndex += 5.486;
					}
					break;
				}
				Rgba colour;
				if (mode == 0)
					colour = source2d::InputColour(context, colourIndex == 0 ? "color_1" : "color_2");
				else {
					const double localSeed = seed + colourIndex;
					const double offset = (localSeed - std::floor(localSeed / 100000) * 100000) / 10;
					const double random = ShaderFract(
						std::sin((cell.X + offset) * 1892.9898 + (cell.Y + offset) * 78.23453) * 437.54123
					);
					colour = GradientEval(gradient, ShaderFract(ShaderFract(random + shift) + 1));
				}
				colour[3] *= alpha;
				return colour;
			}
		);
	}
}
