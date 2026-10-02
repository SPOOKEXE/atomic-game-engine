#include "PixelBuilderEffects.hpp"

#include <algorithm>
#include <numeric>

namespace engine::imagegraph::detail {
	namespace {
		double Mod(double value, double divisor) {
			return value - divisor * std::floor(value / divisor);
		}
		Rgba Mix(Rgba from, const Rgba &to, double amount) {
			for (size_t channel = 0; channel < 4; ++channel)
				from[channel] += (to[channel] - from[channel]) * amount;
			return from;
		}
		double Quant(double value, double steps) {
			return std::floor(value * steps + 0.5) / steps;
		}
		Rgba Pattern(const PixelBuilderEffect &effect, Vector2 uv, Vector2 dimension, const Rgba &base) {
			const auto scale = effect.PatternScale, position = effect.PatternPosition;
			const Vector2 raw{
				std::floor(uv.X * dimension.X - position.X), std::floor(uv.Y * dimension.Y - position.Y)
			};
			const Vector2 pixel{std::floor(raw.X / scale.X), std::floor(raw.Y / scale.Y)};
			const Rgba other = Mix(base, effect.PatternColor, effect.PatternIntensity);
			const double diagonal = dimension.X + dimension.Y;
			const int64_t pattern = effect.Pattern;
			bool second = false;
			if (pattern >= 2 && pattern <= 5) {
				const double coordinate = pattern == 2	 ? raw.X
										  : pattern == 3 ? raw.Y
										  : pattern == 4 ? raw.X + raw.Y
														 : raw.X - raw.Y;
				return Mod(coordinate, scale.X + scale.Y) < scale.Y ? base : other;
			}
			if (pattern == 7) return Mod(pixel.X + pixel.Y, 2) < 1 ? base : other;
			if (pattern == 8) {
				const Vector2 cell{Mod(raw.X, scale.X * 2 + 1), Mod(raw.Y, scale.Y * 2 + 1)};
				const bool a = cell.X > cell.Y, b = (scale.X * 2 - cell.X) > cell.Y;
				return a == b ? base : other;
			}
			if (pattern == 10)
				second = Mod(raw.X, scale.X + 1) < 1 || Mod(raw.Y, scale.Y + 1) < 1;
			else if (pattern == 11) {
				const Vector2 cell{Mod(raw.X, scale.X), Mod(raw.Y, scale.Y)};
				second = std::abs(cell.X - cell.Y) < 1 || std::abs(cell.X - (scale.X - 2 - cell.Y)) < 1;
			} else if (pattern == 13)
				second = raw.Y >= dimension.Y / 2;
			else if (pattern == 14)
				second = raw.X >= dimension.X / 2;
			else if (pattern == 15)
				second = raw.X + pixel.Y >= diagonal / 2;
			else if (pattern == 16)
				second = raw.X + (dimension.X - pixel.Y) >= diagonal / 2;
			else if ((pattern >= 18 && pattern <= 21) || (pattern >= 23 && pattern <= 26)) {
				const int64_t axis = pattern >= 23 ? pattern - 23 : pattern - 18;
				const double coordinate = axis == 0	  ? pixel.X / dimension.X
										  : axis == 1 ? pixel.Y / dimension.Y
										  : axis == 2 ? (pixel.X + pixel.Y) / diagonal
													  : (pixel.X + (dimension.X - pixel.Y)) / diagonal;
				double progress = std::clamp(coordinate, 0.0, 1.0);
				if (pattern >= 23) progress = std::abs(progress - 0.5) * 2;
				return Mix(base, other, Quant(progress, effect.Modify));
			} else if (pattern == 28) {
				const double dx = uv.X - position.X - 0.5, dy = uv.Y - position.Y - 0.5;
				return Mix(
					other, base, Quant(std::sqrt(dx * dx / scale.X + dy * dy / scale.Y) * 2, effect.Modify)
				);
			} else if (pattern == 29) {
				const double angle =
					std::atan2((uv.Y - position.Y - 0.5) / scale.Y, (uv.X - position.X - 0.5) / scale.X);
				return Mix(base, other, Quant(Mod(angle, 2 * std::acos(-1.0)) / (2 * std::acos(-1.0)), 4));
			} else if (pattern == 31 || pattern == 32) {
				const Vector2 p = pattern == 31 ? raw : Vector2{raw.Y, raw.X};
				second = Mod(p.X + Mod(std::floor(p.Y / (scale.Y + 1)), 2) * scale.X, scale.X * 2 + 1) < 1 ||
						 Mod(p.Y, scale.Y + 1) < 1;
			} else if (pattern == 34 || pattern == 35) {
				const Vector2 p = pattern == 34 ? raw : Vector2{raw.Y, raw.X};
				Vector2 cell{Mod(p.X, scale.X), Mod(p.Y, scale.X)};
				if (Mod(std::floor(p.X / scale.X), 2) < 1) cell.X = scale.X - cell.X - 1;
				second = std::abs(cell.X - cell.Y) < scale.Y / scale.X;
			} else if (pattern == 36 || pattern == 37) {
				const Vector2 p = pattern == 36 ? raw : Vector2{raw.Y, raw.X};
				const double threshold = dimension.Y / 2 - scale.Y / 2 +
										 std::abs(Mod(p.X, scale.X * 2) - scale.X) * scale.Y / scale.X;
				second = p.Y > threshold;
			} else if (pattern == 39 || pattern == 40) {
				const Vector2 p = pattern == 39 ? raw : Vector2{raw.Y, raw.X};
				const double threshold =
					dimension.Y / 2 +
					std::sin(p.X / dimension.X * scale.X * 2 * std::acos(-1.0)) * scale.Y / 2;
				second = p.Y > threshold;
			} else if (pattern == 42) {
				const double seed = Mod(effect.Seed, 100000) / 10;
				const double value =
					std::sin((pixel.X + seed) * 1892.9898 + (pixel.Y + seed) * 78.23453) * 437.54123;
				return Mod(value, 1) < effect.PatternIntensity ? effect.PatternColor : base;
			}
			return second ? other : base;
		}
		Rgba EffectPixel(
			const Image &previous,
			Vector2 uv,
			const std::array<double, 4> &bounds,
			const PixelBuilderEffect &effect,
			bool first,
			Vector2 dimension
		) {
			const auto sample = [&](double x, double y) {
				if (x < 0 || y < 0 || x > 1 || y > 1) return Rgba{};
				return SampleNearest(previous, x, y);
			};
			const Rgba current = sample(uv.X, uv.Y);
			Rgba result = first ? Rgba{} : current;
			const bool shape = current[3] > 0;
			const Vector2 patternUv=effect.MapBounds ? Vector2{
				(uv.X-bounds[0]/dimension.X)/((bounds[2]-bounds[0])/dimension.X),
				(uv.Y-bounds[1]/dimension.Y)/((bounds[3]-bounds[1])/dimension.Y)} : uv;
			const Rgba colour = Pattern(effect, patternUv, dimension, effect.Color);
			const auto paint = [&](const Rgba &paintColour) {
				if (effect.Subtract)
					return Rgba{current[0], current[1], current[2], current[3] - effect.Intensity};
				return Mix(current, paintColour, paintColour[3] * effect.Intensity);
			};
			if (effect.Type == 0) return shape ? paint(colour) : result;
			if (effect.Type == 1) {
				double distance = 99999;
				const double alpha = shape ? 0 : 1;
				for (double i = -effect.Thickness; i <= effect.Thickness; ++i)
					for (double j = -effect.Thickness; j <= effect.Thickness; ++j) {
						if (sample(uv.X + i / dimension.X, uv.Y + j / dimension.Y)[3] == alpha)
							distance = std::min(
								distance, effect.StrokeCorner == 0 ? std::hypot(i, j) : std::min(i, j)
							);
					}
				const bool stroke = effect.StrokePosition == 0	 ? distance <= effect.Thickness / 2
									: effect.StrokePosition == 1 ? shape && distance <= effect.Thickness
																 : !shape && distance <= effect.Thickness;
				return stroke ? paint(colour) : result;
			}
			if (effect.Type == 2) {
				if (!shape) return result;
				double fill = 0, size = 0;
				for (double i = -effect.Radius; i <= effect.Radius; ++i)
					for (double j = -effect.Radius; j <= effect.Radius; ++j) {
						++size;
						fill += sample(uv.X + i / dimension.X, uv.Y + j / dimension.Y)[3];
					}
				return fill / size < 0.5 ? paint(colour) : result;
			}
			if (effect.Type == 3) {
				if (!shape) return result;
				const std::array<double, 4> widths{
					effect.HighlightWidths.Z,
					effect.HighlightWidths.X,
					effect.HighlightWidths.Y,
					effect.HighlightWidths.W
				};
				const std::array<Vector2, 4> directions{
					Vector2{-1, 0}, Vector2{1, 0}, Vector2{0, -1}, Vector2{0, 1}
				};
				int nearest = -1;
				double distance = 9999;
				for (size_t side = 0; side < 4; ++side)
					for (double i = 1; i <= widths[side]; ++i) {
						if (sample(
								uv.X + directions[side].X * i / dimension.X,
								uv.Y + directions[side].Y * i / dimension.Y
							)[3] != 0)
							continue;
						if (i < distance) {
							distance = i;
							nearest = int(side);
						}
						break;
					}
				return nearest >= 0
						   ? paint(Pattern(
								 effect, patternUv, dimension, effect.HighlightColors[size_t(nearest)]
							 ))
						   : result;
			}
			if (effect.Type == 4) {
				if (shape) return result;
				const double direction = effect.Direction * std::acos(-1.0) / 180;
				for (double i = 1; i <= 16 && i <= effect.Thickness; ++i)
					if (sample(
							uv.X - std::cos(direction) * i / dimension.X,
							uv.Y + std::sin(direction) * i / dimension.Y
						)[3] != 0)
						return paint(colour);
				return result;
			}
			if (effect.Type == 5) {
				if (!shape) return result;
				Vector2 pixel{std::floor(uv.X * dimension.X), std::floor(uv.Y * dimension.Y)};
				if (effect.ShinesAxis == 1) std::swap(pixel.X, pixel.Y);
				const double width = effect.ShinesAxis == 0 ? dimension.X : dimension.Y;
				const double shineWidth = std::accumulate(effect.Shines.begin(), effect.Shines.end(), 0.0);
				double start = effect.Slope == 0
								   ? -shineWidth + (width + 2 * shineWidth) * effect.Progress
								   : -shineWidth + (2 * width + 2 * shineWidth) * effect.Progress -
										 pixel.Y / effect.Slope;
				bool fill = true;
				for (double shine : effect.Shines) {
					const double end = start + shine;
					if (fill && pixel.X > start && pixel.X <= end) return paint(colour);
					fill = !fill;
					start = end;
				}
			}
			return result;
		}
	}
	bool ReadPixelBuilderEffects(
		NodeContext &context, Vector2 dimension, std::vector<PixelBuilderEffect> &effects
	) {
		if (context.Authored.DynamicInputs.size() > Limits::MaximumPixelBuilderDynamicInputsPerNode)
			return context.Fail(Status::LimitExceeded, "PB authored effect IDs exceed bounded source groups");
		size_t count = 0;
		std::array<bool, 64> present{};
		for (const auto &input : context.Authored.DynamicInputs) {
			size_t group = 0;
			if (!FindDynamicTemplate(context.Entry, input.Id, group))
				return context.Fail(Status::InvalidValue, "Unknown PB effect control", input.Id);
			if (group >= present.size() || context.Entry.DynamicGroupLimit <= 0 ||
				group >= size_t(context.Entry.DynamicGroupLimit))
				return context.Fail(Status::LimitExceeded, "PB effects exceed source group limit", input.Id);
			present[group] = true;
			count = std::max(count, group + 1);
		}
		for (size_t index = 0; index < count; ++index)
			if (!present[index])
				return context.Fail(Status::InvalidValue, "PB effect groups must be contiguous");
		effects.reserve(count);
		const auto rgba = [&](const std::string &port) {
			const Colour color = context.Get<Colour>(port, Colour{255, 255, 255, 255});
			return Rgba{color.Red / 255.0, color.Green / 255.0, color.Blue / 255.0, color.Alpha / 255.0};
		};
		for (size_t group = 0; group < count; ++group) {
			const auto id = [&](std::string_view name) {
				return std::string(name) + "_" + std::to_string(group);
			};
			PixelBuilderEffect effect;
			effect.Type = context.Integer(id("effect_type"));
			effect.Color = rgba(id("color"));
			effect.Intensity = context.Scalar(id("intensity"), 1);
			effect.Pattern = context.Integer(id("pattern"));
			effect.PatternColor = rgba(id("pattern_color"));
			effect.PatternIntensity = context.Scalar(id("pattern_intensity"), 1);
			effect.PatternScale = context.Vec2(id("pattern_scale"), {1, 1});
			effect.PatternPosition = context.Vec2(id("pattern_position"));
			for (const auto name : {"pattern_scale", "pattern_position"}) {
				const auto port = id(name);
				const int64_t unit = context.Integer(id(std::string(name) + "_unit"), 1);
				if (unit != 0 && unit != 1)
					return context.Fail(Status::InvalidValue, "Invalid PB pattern unit", port);
				// Source Vec2 unit.apply multiplies linked and authored inputs alike.
				if (unit == 1) {
					auto &value = std::string_view(name) == "pattern_scale" ? effect.PatternScale
																			: effect.PatternPosition;
					value.X *= dimension.X;
					value.Y *= dimension.Y;
				}
			}
			effect.MapBounds = context.Boolean(id("pattern_map"));
			effect.Thickness = context.Integer(id("thickness"), 1);
			effect.StrokePosition = context.Integer(id("stroke_position"), 1);
			effect.StrokeCorner = context.Integer(id("stroke_corner"));
			effect.Radius = context.Integer(id("corner_radius"), 1);
			effect.HighlightWidths = context.Get<Vector4>(id("highlight_widths"));
			size_t highlight = 0;
			for (const auto name : {"highlight_left", "highlight_right", "highlight_top", "highlight_bottom"})
				effect.HighlightColors[highlight++] = rgba(id(name));
			effect.Modify = context.Scalar(id("modify"), 4);
			effect.Subtract = context.Boolean(id("subtract"));
			effect.Direction = context.Scalar(id("direction"), -90);
			if (const Value *value = context.Find(id("shines"))) {
				const auto *array = std::get_if<ArrayValue>(value);
				if (!array || !array->Nested.empty() || !array->Items.empty() || array->Elements.size() > 64)
					return context.Fail(
						Status::InvalidValue, "PB shines require a bounded numeric array", id("shines")
					);
				effect.Shines.clear();
				effect.Shines.reserve(array->Elements.size());
				for (const auto &item : array->Elements) {
					if (const auto *number = std::get_if<double>(&item))
						effect.Shines.push_back(*number);
					else if (const auto *number = std::get_if<int64_t>(&item))
						effect.Shines.push_back(double(*number));
					else
						return context.Fail(
							Status::InvalidValue, "PB shines require numeric segments", id("shines")
						);
				}
			}
			effect.Progress = context.Scalar(id("progress"), .5);
			effect.Slope = context.Scalar(id("slope"), 1);
			effect.ShinesAxis = context.Integer(id("axis"));
			if (effect.Pattern == 42 && !context.Find(id("seed")))
				return context.Fail(
					Status::UnsupportedExecution, "PB noise pattern requires recorded source seed", id("seed")
				);
			effect.Seed = context.Scalar(id("seed"));
			effects.push_back(std::move(effect));
		}
		return context.FailureCode == Status::Ok;
	}
	bool ApplyPixelBuilderEffects(
		NodeContext &context,
		const Image &shape,
		const std::array<double, 4> &bounds,
		std::span<const PixelBuilderEffect> effects,
		Image &output,
		Vector2 canvasDimension
	) {
		if (effects.empty())
			return context.Fail(
				Status::UnsupportedExecution, "PB Draw without effects retains previous source output"
			);
		uint64_t samples = 0;
		for (const auto &effect : effects) {
			if (effect.Shines.size() > 64)
				return context.Fail(Status::LimitExceeded, "PB shines exceed source shader slots");
			double work = 1;
			if (effect.Type == 1 || effect.Type == 2) {
				const double radius = effect.Type == 1 ? effect.Thickness : effect.Radius;
				if (!std::isfinite(radius) || radius > Limits::MaximumDimension)
					return context.Fail(Status::LimitExceeded, "PB effect radius exceeds bounded canvas");
				work = radius < 0 ? 1 : std::pow(std::floor(radius * 2) + 1, 2);
			} else if (effect.Type == 3) {
				for (double width :
					 {effect.HighlightWidths.X,
					  effect.HighlightWidths.Y,
					  effect.HighlightWidths.Z,
					  effect.HighlightWidths.W}) {
					if (!std::isfinite(width) || width > Limits::MaximumDimension)
						return context.Fail(
							Status::LimitExceeded, "PB highlight width exceeds bounded canvas"
						);
					work += std::max(0.0, std::floor(width));
				}
			} else if (effect.Type == 4)
				work = 16;
			else if (effect.Type == 5)
				work = effect.Shines.size() + 1;
			if (!std::isfinite(work) || work > 64000000 || samples > 64000000 - uint64_t(work))
				return context.Fail(Status::LimitExceeded, "PB effects exceed sampling budget");
			samples += uint64_t(work);
		}
		if (uint64_t(shape.Width) * shape.Height > 64000000 / std::max(uint64_t{1}, samples))
			return context.Fail(Status::LimitExceeded, "PB effects exceed sampling budget");
		auto charge = context.ReserveWorkspace(shape.Pixels.size() * 2);
		if (!charge) return false;
		Image previous = shape;
		Image next{shape.Width, shape.Height, std::vector<uint8_t>(shape.Pixels.size()), 0};
		for (size_t index = 0; index < effects.size(); ++index) {
			for (uint32_t y = 0; y < shape.Height; ++y)
				for (uint32_t x = 0; x < shape.Width; ++x)
					if (!WritePixel(
							next,
							x,
							y,
							EffectPixel(
								previous,
								{(x + 0.5) / shape.Width, (y + 0.5) / shape.Height},
								bounds,
								effects[index],
								index == 0,
								canvasDimension
							)
						))
						return context.Fail(Status::InvalidValue, "PB effect sample is nonfinite");
			std::swap(previous, next);
		}
		for (uint32_t y = 0; y < shape.Height; ++y)
			for (uint32_t x = 0; x < shape.Width; ++x)
				if (!WritePixel(output, x, y, ReadPixel(previous, x, y)))
					return context.Fail(Status::InvalidValue, "PB draw final sample is nonfinite");
		return context.FailureCode == Status::Ok;
	}
}
