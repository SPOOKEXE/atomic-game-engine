#include "Families.hpp"
#include "Sampler.hpp"

#include <numbers>

namespace engine::imagegraph::detail {
	namespace {
		// Node_Sprite_Stack and its draw_surface_blend helpers preserve each intermediate
		// surface conversion. The final ordinary draw multiplies all four channels by alpha.
		Rgba SpriteStackTint(const NodeContext &context, std::string_view id) {
			const Colour colour = context.Get<Colour>(id, {255, 255, 255, 255});
			return {colour.Red / 255.0, colour.Green / 255.0, colour.Blue / 255.0, colour.Alpha / 255.0};
		}
		Vector2 SpriteStackOrigin(const Image &image, double rotation) {
			const double x = image.Width * .5, y = image.Height * .5;
			if (rotation == 0) return {};
			if (rotation == 180) return {image.Width * 1.0, image.Height * 1.0};
			const double angle = -rotation * std::numbers::pi / 180.0;
			return {
				x - x * std::cos(angle) + y * std::sin(angle), y - x * std::sin(angle) - y * std::cos(angle)
			};
		}
		bool
		SpriteStackDimensions(NodeContext &context, const Image &source, uint32_t &width, uint32_t &height) {
			// OUTPUT_SCALING has no Fit content case in this source node.
			const Value *modeValue = context.Find("output_dimension_type");
			const auto number = modeValue ? SourceChoiceNumber(*modeValue) : std::optional<double>{1};
			if (!number || !std::isfinite(*number) || std::trunc(*number) != *number || *number < 0 ||
				*number > 3)
				return context.Fail(
					Status::InvalidValue, "sprite stack dimension mode is invalid", "output_dimension_type"
				);
			const int64_t mode = int64_t(*number);
			if (mode == 1) return ResolveDimension(context, "dimension", width, height);
			double x = source.Width, y = source.Height;
			if (mode == 2) {
				const Vector2 relative = context.Vec2("relative_dimension", {1, 1});
				x *= relative.X;
				y *= relative.Y;
			}
			if (!std::isfinite(x) || !std::isfinite(y))
				return context.Fail(
					Status::InvalidValue, "sprite stack dimensions must be finite", "relative_dimension"
				);
			const auto rounded = [](double value) {
				const double lower = std::floor(value), remainder = value - lower;
				return std::max(
					1.0, lower + (remainder > .5 || (remainder == .5 && std::fmod(lower, 2.0) != 0))
				);
			};
			x = rounded(x);
			y = rounded(y);
			if (x > Limits::MaximumDimension || y > Limits::MaximumDimension)
				return context.Fail(
					Status::LimitExceeded,
					"sprite stack dimensions exceed native limits",
					"relative_dimension"
				);
			width = uint32_t(x);
			height = uint32_t(y);
			return true;
		}
		bool SpriteStackHighlight(
			NodeContext &context,
			const Image &source,
			double u,
			double v,
			Vector2 shift,
			double rotation,
			int64_t highlight,
			const Rgba &tint,
			Rgba &pixel,
			uint64_t &work
		) {
			pixel = SampleNearest(source, u, v);
			Rgba alternate = tint;
			if (highlight == 2) {
				const float angle = float(rotation * std::numbers::pi / 180.0);
				const float cosine = std::cos(angle), sine = std::sin(angle);
				const float sx = float(shift.X / source.Width), sy = float(shift.Y / source.Height);
				// GLSL mat2 constructor is column-major; this is the pinned shader's rotation.
				const float dx = cosine * sx + sine * sy, dy = -sine * sx + cosine * sy;
				float x = float(u) - dx, y = float(v) - dy;
				Rgba nearby = SampleNearest(source, x, y);
				while (nearby == pixel) {
					if (++work > 64'000'000)
						return context.Fail(
							Status::LimitExceeded,
							"sprite stack inner highlight exceeds bounded sampling",
							"highlight"
						);
					const float nextX = x - dx, nextY = y - dy;
					if (nextX == x && nextY == y)
						return context.Fail(
							Status::UnsupportedExecution,
							"source inner highlight does not advance its sample",
							"stack_shift"
						);
					x = nextX;
					y = nextY;
					nearby = SampleNearest(source, x, y);
					if (nearby[3] == 0 || x < 0 || y < 0 || x > 1 || y > 1) break;
				}
				if (pixel[3] != 1 || nearby[3] <= 0)
					return context.Fail(
						Status::UnsupportedExecution,
						"source inner highlight reads an uninitialized colour",
						"highlight"
					);
				for (size_t channel = 0; channel < 4; channel++)
					alternate[channel] = nearby[channel] * tint[channel];
			}
			for (size_t channel = 0; channel < 3; channel++)
				pixel[channel] += (alternate[channel] - pixel[channel]) * alternate[3];
			return true;
		}
		bool SpriteStackLayer(
			NodeContext &context,
			const Image &source,
			Image &foreground,
			Image &previous,
			Image &next,
			Vector2 position,
			Vector2 shift,
			double rotation,
			const Rgba &tint,
			double opacity,
			int64_t highlight,
			const SamplerSettings &sampler,
			uint64_t &work
		) {
			const double angle = rotation * std::numbers::pi / 180.0;
			const double cosine = std::cos(angle), sine = std::sin(angle);
			const Vector2 origin = SpriteStackOrigin(source, rotation);
			const double px = origin.X + position.X, py = origin.Y + position.Y;
			if (!std::isfinite(px) || !std::isfinite(py) || !std::isfinite(opacity))
				return context.Fail(
					Status::InvalidValue, "sprite stack transforms must remain finite", "position"
				);
			// The sampled foreground is an override draw onto a cleared scratch surface.
			for (uint32_t y = 0; y < foreground.Height; y++)
				for (uint32_t x = 0; x < foreground.Width; x++) {
					const double dx = (x + .5) - px, dy = (y + .5) - py;
					const double u = (cosine * dx - sine * dy) / source.Width;
					const double v = (sine * dx + cosine * dy) / source.Height;
					Rgba pixel{};
					if (u >= 0 && v >= 0 && u < 1 && v < 1) {
						if (highlight) {
							if (!SpriteStackHighlight(
									context, source, u, v, shift, rotation, highlight, tint, pixel, work
								))
								return false;
						} else {
							pixel = SampleTexture(source, u, v, sampler);
							// draw_surface_blend_ext always draws alpha=1, then supplies opacity separately.
							for (size_t channel = 0; channel < 3; channel++)
								pixel[channel] *= tint[channel];
						}
					}
					if (!WritePixel(foreground, x, y, pixel))
						return context.Fail(
							Status::InvalidValue,
							"sprite stack foreground exceeds the numeric surface range",
							"surface_out"
						);
				}
			// sh_blend_normal computes unassociated RGB, with a separate combined alpha.
			for (uint32_t y = 0; y < next.Height; y++)
				for (uint32_t x = 0; x < next.Width; x++) {
					const Rgba background = ReadPixel(previous, x, y);
					Rgba front = ReadPixel(foreground, x, y), result{};
					front[3] *= opacity;
					const double alpha = front[3] + background[3] * (1 - front[3]);
					if (alpha != 0) {
						for (size_t channel = 0; channel < 3; channel++)
							result[channel] = (front[channel] * front[3] +
											   background[channel] * background[3] * (1 - front[3])) /
											  alpha;
						result[3] = alpha;
					}
					if (!WritePixel(next, x, y, result))
						return context.Fail(
							Status::InvalidValue,
							"sprite stack blend exceeds the numeric surface range",
							"surface_out"
						);
				}
			std::swap(previous, next);
			return true;
		}
		bool SpriteStack(NodeContext &context) {
			const Image *source = context.Input("base_shape");
			const ImageArray *sources = nullptr;
			// Processor rows retain the original array view alongside the selected image.
			if (context.Integer("array_process", 1))
				for (const auto &[port, array] : context.ImageArrays)
					if (port == "base_shape") sources = array;
			if (sources) {
				if (sources->Items.empty())
					return context.Fail(
						Status::UnsupportedExecution,
						"source sprite stack needs a first array image for dimensions",
						"base_shape"
					);
				const auto *index = std::get_if<size_t>(&sources->Items.front().Data);
				if (!index || *index >= sources->Images.size())
					return context.Fail(
						Status::UnsupportedExecution,
						"source sprite stack first array entry is not a surface",
						"base_shape"
					);
				source = &sources->Images[*index];
			}
			if (!source)
				return context.Fail(
					Status::TypeMismatch,
					"sprite stack requires a surface or combined surface array",
					"base_shape"
				);
			const auto format = ResolveProcessorSurfaceFormat(context, source);
			uint32_t width = 0, height = 0;
			if (!format || !SpriteStackDimensions(context, *source, width, height)) return false;
			const auto layout = CheckedSurfaceLayout(width, height, *format, Limits::MaximumOutputBytes);
			if (!layout)
				return context.Fail(
					Status::LimitExceeded, "sprite stack scratch dimensions exceed native limits", "dimension"
				);
			const int64_t amount = sources && context.Integer("array_process", 1)
									   ? int64_t(sources->Items.size())
									   : context.Integer("stack_amount", 4);
			const uint64_t layers = uint64_t(std::max<int64_t>(amount, 0)) + !sources;
			const SamplerSettings sampler = ReadSampler(context);
			if (!SupportedSampler(context, sampler)) return false;
			const uint64_t samples = uint64_t(width) * height;
			const uint64_t cost = sampler.Interpolation == 4 ? 11 : 3;
			if (layers > 64'000'000 / cost || (layers && samples > 64'000'000 / (layers * cost)))
				return context.Fail(
					Status::LimitExceeded, "sprite stack exceeds bounded layer sampling", "stack_amount"
				);
			Image *output = context.NewImage("surface_out", width, height, *format);
			if (!output) return false;
			auto charge = context.ReserveWorkspace(layout->Bytes * 4, "surface_out");
			if (!charge) return false;
			std::array<Image, 4> scratch;
			for (auto &image : scratch) {
				image.Width = width;
				image.Height = height;
				image.Format = *format;
				image.Pixels.resize(size_t(layout->Bytes));
			}
			const Vector2 shift = context.Vec2("stack_shift", {0, 1});
			// setUnitSimple references node.getDimension(), whose dimension_index is 1,
			// even when Same as input or Relative chooses another output size.
			if (!context.IsLinked("dimension") && context.Integer("dimension_unit", 1) == 2 &&
				context.Integer("position_unit", 1) == 1)
				return context.Fail(
					Status::UnsupportedExecution,
					"source sprite stack Dimension has no mask reference",
					"dimension_unit"
				);
			Vector2 reference = context.Vec2("dimension", {1, 1});
			if (!context.IsLinked("dimension") && context.Integer("dimension_unit", 1) == 1) {
				reference.X *= context.Project.SurfaceWidth;
				reference.Y *= context.Project.SurfaceHeight;
			}
			Vector2 position = UnitVector(context, "position", reference.X, reference.Y);
			const double rotation = context.Scalar("rotation"),
						 alphaEnd = sources ? 1 : context.Scalar("alpha_end", 1);
			const Rgba tint = SpriteStackTint(context, "stack_blend"),
					   highlightTint = SpriteStackTint(context, "highlight_color");
			const int64_t highlight = sources ? 0 : context.Integer("highlight");
			if (!std::isfinite(position.X) || !std::isfinite(position.Y) || !std::isfinite(shift.X) ||
				!std::isfinite(shift.Y) || !std::isfinite(rotation) || !std::isfinite(alphaEnd))
				return context.Fail(Status::InvalidValue, "sprite stack controls must be finite");
			if (context.Boolean("move_base")) {
				position.X -= shift.X * amount;
				position.Y -= shift.Y * amount;
			}
			uint64_t work = samples * layers * cost;
			if (sources) {
				for (int64_t i = 0; i < amount; i++) {
					const size_t entry = std::min<size_t>(size_t(i), sources->Items.size() - 1);
					const auto *index = std::get_if<size_t>(&sources->Items[entry].Data);
					if (!index || *index >= sources->Images.size()) continue;
					if (!SpriteStackLayer(
							context,
							sources->Images[*index],
							scratch[2],
							scratch[0],
							scratch[1],
							position,
							shift,
							rotation,
							tint,
							tint[3],
							0,
							sampler,
							work
						))
						return false;
					position.X += shift.X;
					position.Y += shift.Y;
				}
			} else {
				double alpha = alphaEnd;
				const double delta = amount == 0 ? 0 : (1 - alpha) / amount;
				position.X += shift.X * amount;
				position.Y += shift.Y * amount;
				for (int64_t i = 0; i < amount; i++) {
					const int64_t selectedHighlight = i == amount - 1 ? highlight : 0;
					if (!SpriteStackLayer(
							context,
							*source,
							scratch[2],
							scratch[0],
							scratch[1],
							position,
							shift,
							rotation,
							selectedHighlight ? highlightTint : tint,
							selectedHighlight ? 1 : tint[3] * alpha,
							selectedHighlight,
							sampler,
							work
						))
						return false;
					position.X -= shift.X;
					position.Y -= shift.Y;
					alpha += delta;
				}
				if (!SpriteStackLayer(
						context,
						*source,
						scratch[2],
						scratch[0],
						scratch[1],
						position,
						shift,
						rotation,
						{1, 1, 1, 1},
						alpha,
						0,
						sampler,
						work
					))
					return false;
			}
			for (uint32_t y = 0; y < height; y++)
				for (uint32_t x = 0; x < width; x++) {
					Rgba pixel = ReadPixel(scratch[0], x, y);
					const double alpha = pixel[3];
					for (double &channel : pixel)
						channel *= alpha;
					if (!WritePixel(*output, x, y, pixel))
						return context.Fail(
							Status::InvalidValue,
							"sprite stack final draw exceeds the numeric surface range",
							"surface_out"
						);
				}
			return context.FailureCode == Status::Ok;
		}
	}
	std::span<const ExecutorEntry> SourceSpriteStackExecutors() {
		static constexpr ExecutorEntry entries[]{{"pc.sprite_stack", SpriteStack, true}};
		return entries;
	}
}
