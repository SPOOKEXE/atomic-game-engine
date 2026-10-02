#include "Sampler.hpp"

namespace engine::imagegraph::detail {
	bool MorphSurface(NodeContext &context) {
		const Image *from = context.Input("surface_from"), *to = context.Input("surface_to");
		if (!from || !to) return context.Fail(Status::InvalidValue, "Both morph surfaces are required");
		const auto format = ResolveProcessorSurfaceFormat(context, from);
		if (!format) return false;
		const SamplerSettings sampler = ReadSampler(context);
		if (!SupportedSampler(context, sampler)) return false;
		constexpr uint64_t MAXIMUM_SAMPLES = 64'000'000;
		if (uint64_t(from->Width + 1) * 130 > MAXIMUM_SAMPLES / from->Width / from->Height)
			return context.Fail(Status::LimitExceeded, "Surface morph exceeds sample work budget");
		Image *output = context.NewImage("surface_out", from->Width, from->Height, *format);
		if (!output) return false;
		const double amount = context.Scalar("morph_amount"), threshold = context.Scalar("threshold", 0.1);
		const auto outside = [](Vector2 point) {
			return point.X < 0 || point.Y < 0 || point.X > 1 || point.Y > 1;
		};
		for (uint32_t y = 0; y < from->Height; ++y)
			for (uint32_t x = 0; x < from->Width; ++x) {
				const Vector2 uv{(x + 0.5) / from->Width, (y + 0.5) / from->Height};
				Rgba result{};
				bool matched = false;
				for (uint32_t radius = 0; radius <= from->Width && !matched; ++radius) {
					double top = 0, base = 1;
					const double distance =
						double(radius) / from->Width / (amount > 0.5 ? amount : 1 - amount);
					for (int angleIndex = 0; angleIndex <= 64; ++angleIndex) {
						double angle = top / base * 2 * std::acos(-1.0);
						if (amount > 0.5) angle = 2 * std::acos(-1.0) - angle;
						top += 2;
						if (top >= base) {
							top = 1;
							base *= 2;
						}
						const Vector2 offset{
							std::cos(angle) * radius / from->Width, std::sin(angle) * radius / from->Height
						};
						const Vector2 first{uv.X + offset.X, uv.Y + offset.Y};
						if (outside(first)) continue;
						const double length = std::hypot(offset.X, offset.Y);
						const Vector2 direction =
							radius == 0 ? Vector2{} : Vector2{offset.X / length, offset.Y / length};
						const Vector2 second{
							first.X - direction.X * distance, first.Y - direction.Y * distance
						};
						if (outside(second)) continue;
						const Vector2 fromUv = amount > 0.5 ? first : second,
									  toUv = amount > 0.5 ? second : first;
						const Rgba firstColour = TextureInterpolated(*from, fromUv.X, fromUv.Y, sampler);
						if (firstColour[3] == 0) continue;
						const Rgba secondColour = TextureInterpolated(*to, toUv.X, toUv.Y, sampler);
						if (secondColour[3] == 0) continue;
						double difference = 0;
						for (size_t channel = 0; channel < 4; ++channel)
							difference += std::pow(firstColour[channel] - secondColour[channel], 2);
						if (std::sqrt(difference) > threshold * 2) continue;
						for (size_t channel = 0; channel < 4; ++channel)
							result[channel] = firstColour[channel] +
											  (secondColour[channel] - firstColour[channel]) * amount;
						matched = true;
						break;
					}
				}
				if (!WritePixel(*output, x, y, result))
					return context.Fail(
						Status::InvalidValue, "Morph sample exceeds surface range", "surface_out"
					);
			}
		return true;
	}
	bool HeightBlend(NodeContext &context) {
		const Image *background = context.Input("background"), *foreground = context.Input("foreground");
		if (!background || !foreground)
			return context.Fail(Status::InvalidValue, "Both height blend surfaces are required");
		Image *output =
			context.NewImage("surface_out", background->Width, background->Height, SurfaceFormat::RGBA8Unorm);
		if (!output) return false;
		const int64_t mode = context.Integer("mode"), type = context.Integer("type", 1);
		const double factor = context.Scalar("factor", 0.5);
		for (uint32_t y = 0; y < output->Height; ++y)
			for (uint32_t x = 0; x < output->Width; ++x) {
				const double u = (x + 0.5) / output->Width, v = (y + 0.5) / output->Height;
				const Rgba bg = SampleNearest(*background, u, v), fg = SampleNearest(*foreground, u, v);
				const double first = (bg[0] + bg[1] + bg[2]) / 3 * bg[3],
							 second = (fg[0] + fg[1] + fg[2]) / 3 * fg[3];
				const double nearest = mode == 0 ? std::min(first, second) : std::max(first, second);
				const double a = mode == 0 ? 1 - first : first, b = mode == 0 ? 1 - second : second;
				double height = 0;
				if (type == 0)
					height = -factor * std::log2(std::exp2(-a / factor) + std::exp2(-b / factor));
				else if (type == 1) {
					const double k = 2 * nearest * factor;
					height = 0.5 * (a + b - std::sqrt((b - a) * (b - a) + k * k));
				} else if (type == 2)
					height = a + (b - a) / (1 - std::exp2((b - a) / (nearest * factor * std::log(2.0))));
				else {
					const double k = factor * (type == 3 ? 4 : type == 4 ? 6 : 1 / (1 - std::sqrt(0.5)));
					const double h = std::max(k - std::abs(a - b), 0.0) / k;
					if (type == 3)
						height = std::min(a, b) - h * h * k / 4;
					else if (type == 4)
						height = std::min(a, b) - h * h * h * k / 6;
					else
						height = std::min(a, b) - k * 0.5 * (1 + h - std::sqrt(1 - h * (h - 2)));
				}
				const double level = mode == 0 ? 1 - height : height;
				if (!WritePixel(*output, x, y, {level, level, level, std::max(bg[3], fg[3])}))
					return context.Fail(
						Status::InvalidValue, "Height blend sample exceeds surface range", "surface_out"
					);
			}
		return true;
	}

}
