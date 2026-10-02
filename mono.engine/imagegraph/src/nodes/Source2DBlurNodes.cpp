#include "Curve.hpp"
#include "Sampler.hpp"

namespace engine::imagegraph::detail {
	bool SlopeBlur(NodeContext &context) {
		bool failed = false;
		if (CopyWhenInactive(context, failed)) return !failed;
		const Image *source = context.Input("surface_in"), *slope = context.Input("slope_map");
		if (!source || !slope)
			return context.Fail(Status::InvalidValue, "Surface In and Slope Map are required");
		const SamplerSettings sampler = ReadSampler(context);
		if (!SupportedSampler(context, sampler)) return false;
		if (uint64_t(source->Width) * source->Height * 64 * 5 > 64'000'000)
			return context.Fail(Status::LimitExceeded, "Slope blur exceeds sample work budget");
		const UvMap uv = ReadUvMap(context);
		const auto *curve = std::get_if<Curve>(context.Find("strength_curve"));
		const bool curved = context.Boolean("strength_curved") && curve,
				   gamma = context.Boolean("gamma_correction");
		const double step = context.Scalar("step", 0.1);
		return RunPixelProcessor(context, [&](const Image &image, uint32_t, uint32_t, double u, double v) {
			const double strength = MappedScalar(context, "strength", u, v, Filtered(sampler));
			double weight = 0;
			Rgba total{};
			const auto brightness = [&](double x, double y) {
				const Rgba colour = SampleTexture(*slope, x, y, sampler);
				return (colour[0] + colour[1] + colour[2]) / 3;
			};
			for (int i = 0; i < 64 && i <= strength; ++i) {
				const double sampleWeight = 1 - i / strength;
				const double contribution =
					sampleWeight * (curved ? EvalShaderCurve(*curve, sampleWeight) : 1);
				Rgba colour = SampleTextureUv(image, u, v, i / strength, uv, sampler);
				if (gamma)
					for (size_t channel = 0; channel < 3; ++channel)
						colour[channel] = std::pow(colour[channel], 2.2);
				for (size_t channel = 0; channel < 4; ++channel)
					total[channel] += colour[channel] * contribution;
				weight += contribution;
				const double slopeX =
					brightness(u + 1.0 / slope->Width, v) - brightness(u - 1.0 / slope->Width, v);
				const double slopeY =
					brightness(u, v + 1.0 / slope->Height) - brightness(u, v - 1.0 / slope->Height);
				u += slopeX * step;
				v += slopeY * step;
			}
			for (double &channel : total)
				channel /= weight;
			if (gamma)
				for (size_t channel = 0; channel < 3; ++channel)
					total[channel] = std::pow(total[channel], 1 / 2.2);
			return total;
		});
	}
}

namespace engine::imagegraph::detail {
	bool Deblur(NodeContext &context) {
		bool failed = false;
		if (CopyWhenInactive(context, failed)) return !failed;
		const Image *source = context.Input("surface_in");
		if (!source) return context.Fail(Status::InvalidValue, "Surface In is required", "surface_in");
		Image *output =
			context.NewImage("surface_out", source->Width, source->Height, SurfaceFormat::RGBA8Unorm);
		if (!output) return false;
		const int64_t method = context.Integer("method");
		const double radius = context.Scalar("radius", 8), strength = context.Scalar("strength", 1),
					 denoise = context.Scalar("denoise", 1);
		const SamplerSettings sampler = ReadSampler(context);
		const auto smooth = [](double first, double second, double value) {
			const double progress = std::clamp((value - first) / (second - first), 0.0, 1.0);
			return progress * progress * (3 - 2 * progress);
		};
		for (uint32_t y = 0; y < source->Height; ++y)
			for (uint32_t x = 0; x < source->Width; ++x) {
				const double u = (x + 0.5) / source->Width, v = (y + 0.5) / source->Height;
				const auto sample = [&](double dx, double dy) {
					return SampleTextureSimple(
						*source,
						u + dx * radius / source->Width,
						v + dy * radius / source->Height,
						sampler.Oversample,
						true
					);
				};
				const Rgba original = sample(0, 0);
				Rgba result{};
				double suppression = 1;
				if (method == 0) {
					constexpr std::array<double, 5> WEIGHTS{0.06136, 0.24477, 0.38774, 0.24477, 0.06136};
					Rgba blurred{};
					// The source discards its horizontal pass; only vertical samples feed finalBlur.
					for (int i = 0; i < 5; ++i) {
						const Rgba neighbour = sample(0, i - 2);
						for (size_t channel = 0; channel < 4; ++channel)
							blurred[channel] += neighbour[channel] * WEIGHTS[size_t(i)];
					}
					double distance = 0;
					for (size_t channel = 0; channel < 4; ++channel) {
						result[channel] =
							original[channel] + (original[channel] - blurred[channel]) * strength;
						if (channel < 3) distance += std::pow(original[channel] - blurred[channel], 2);
					}
					suppression = smooth(0, denoise, std::sqrt(distance));
				} else if (method == 1) {
					for (size_t channel = 0; channel < 4; ++channel)
						result[channel] = original[channel] * (1 + 4 * strength);
					for (const Vector2 offset :
						 {Vector2{0, 1}, Vector2{0, -1}, Vector2{1, 0}, Vector2{-1, 0}}) {
						const Rgba neighbour = sample(offset.X, offset.Y);
						for (size_t channel = 0; channel < 4; ++channel)
							result[channel] -= neighbour[channel] * strength;
					}
				} else {
					for (int dx = -1; dx <= 1; ++dx)
						for (int dy = -1; dy <= 1; ++dy) {
							const double weight = dx == 0 && dy == 0 ? 1 + 0.8 * strength : -0.1 * strength;
							const Rgba neighbour = sample(dx, dy);
							for (size_t channel = 0; channel < 4; ++channel)
								result[channel] += neighbour[channel] * weight;
						}
					double distance = 0;
					for (size_t channel = 0; channel < 3; ++channel)
						distance += std::pow(original[channel] - result[channel], 2);
					suppression = smooth(denoise, 0, std::sqrt(distance));
				}
				for (size_t channel = 0; channel < 3; ++channel)
					result[channel] =
						original[channel] +
						(std::clamp(result[channel], 0.0, 1.0) - original[channel]) * suppression;
				result[3] = original[3];
				if (!WritePixel(*output, x, y, result))
					return context.Fail(
						Status::InvalidValue, "Deblur sample exceeds surface range", "surface_out"
					);
			}
		FinishProcessor(context, *source, *output);
		return context.FailureCode == Status::Ok;
	}
}

namespace engine::imagegraph::detail {
	bool ShapeBlur(NodeContext &context) {
		bool failed = false;
		if (CopyWhenInactive(context, failed)) return !failed;
		const Image *source = context.Input("surface_in"), *shape = context.Input("blur_shape"),
					*mask = context.Input("blur_mask");
		if (!source || !shape)
			return context.Fail(Status::InvalidValue, "Surface In and Blur Shape are required");
		const uint32_t columns = std::min<uint32_t>(shape->Width, 65),
					   rows = std::min<uint32_t>(shape->Height, 65);
		if (uint64_t(columns) * rows > 64'000'000 / source->Width / source->Height)
			return context.Fail(Status::LimitExceeded, "Shape blur exceeds sample work budget");
		const SamplerSettings sampler = ReadSampler(context);
		const UvMap uv = ReadUvMap(context);
		const int64_t mode = context.Integer("mode");
		// Node_Blur_Shape uploads input9 (Mask feather), rather than input10 (Gamma Correction).
		const bool gamma = std::trunc(context.Scalar("mask_feather")) == 1;
		return RunPixelProcessor(context, [&](const Image &image, uint32_t, uint32_t, double u, double v) {
			double strength = 1;
			if (mask) {
				const Rgba colour = SampleNearest(*mask, u, v);
				strength = (colour[0] + colour[1] + colour[2]) / 3 * colour[3];
			}
			if (strength == 0) return SampleTextureSimple(image, u, v, sampler.Oversample, false);
			Rgba total{};
			double weight = 0;
			for (uint32_t i = 0; i < columns; ++i)
				for (uint32_t j = 0; j < rows; ++j) {
					const Vector2 offset{
						(double(i) - shape->Width / 2.0) / strength,
						(double(j) - shape->Height / 2.0) / strength
					};
					const Vector2 relative{offset.X / shape->Width, offset.Y / shape->Height};
					if (std::abs(relative.X) >= 0.5 || std::abs(relative.Y) >= 0.5) continue;
					double sampleU = u + offset.X / image.Width, sampleV = v + offset.Y / image.Height;
					UvRemap(uv, sampleU, sampleV, std::hypot(relative.X * 2, relative.Y * 2), false);
					Rgba colour = SampleTextureSimple(image, sampleU, sampleV, sampler.Oversample, false);
					const Rgba shapeColour = BilinearClamp(*shape, 0.5 - relative.X, 0.5 - relative.Y);
					const double contribution =
						(shapeColour[0] + shapeColour[1] + shapeColour[2]) / 3 * shapeColour[3];
					if (gamma)
						for (size_t channel = 0; channel < 3; ++channel)
							colour[channel] = std::pow(colour[channel], 2.2);
					for (size_t channel = 0; channel < 4; ++channel)
						if (mode == 0)
							total[channel] += colour[channel] * contribution;
						else
							total[channel] = std::max(total[channel], colour[channel] * contribution);
					weight += contribution;
				}
			if (mode == 0)
				for (double &channel : total)
					channel /= weight;
			else
				total[3] = 1;
			if (gamma)
				for (size_t channel = 0; channel < 3; ++channel)
					total[channel] = std::pow(total[channel], 1 / 2.2);
			return total;
		});
	}
}
