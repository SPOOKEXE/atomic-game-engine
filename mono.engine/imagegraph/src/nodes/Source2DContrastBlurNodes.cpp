#include "../SourceSafeDraw.hpp"
#include "Processor.hpp"
#include "Sampler.hpp"

namespace engine::imagegraph::detail {
	bool SourceContrastBlur(NodeContext &context) {
		bool inactiveFailed = false;
		if (CopyWhenInactive(context, inactiveFailed)) return !inactiveFailed;
		const Image *source = context.Input("surface_in");
		if (!source)
			return context.Fail(
				Status::InvalidValue, "Contrast Blur requires its input surface", "surface_in"
			);
		const bool single = source->Format == SurfaceFormat::R8Unorm ||
							source->Format == SurfaceFormat::R16Float ||
							source->Format == SurfaceFormat::R32Float;
		if (single)
			return RunPixelProcessor(
				context, [](const Image &surface, uint32_t x, uint32_t y, double, double) {
					return SourceSafeDrawPixel(surface, x, y);
				}
			);
		const auto hardware = ReadSampler(context);
		const auto uv = ReadUvMap(context);
		const bool gamma = context.Boolean("gamma_correction");
		uint64_t work = 0;
		SamplerSettings sampler = hardware;
		// sampler_simple uses texture2D; custom interpolation kernels are absent from
		// this shader.
		sampler.Interpolation = Filtered(hardware) ? 2 : 1;
		return RunPixelProcessor(
			context, [&](const Image &surface, uint32_t x, uint32_t y, double u, double v) {
				const double size =
					MappedScalar(context, "size", u, v, Filtered(hardware)) *
					(!context.IsLinked("size") ? UnitScale(context, "size", surface.Width) : 1);
				const double threshold = MappedScalar(context, "threshold", u, v, Filtered(hardware));
				const Rgba center = ReadPixel(surface, x, y);
				Rgba colour = center;
				double divisor = 1;
				const double square = size * size;
				const int radius = int(std::ceil(std::min(64., std::abs(size))));
				const uint64_t taps = uint64_t(radius * 2 + 1) * (radius * 2 + 1);
				if (taps > 64000000 - work) {
					context.Fail(
						Status::LimitExceeded, "Contrast Blur neighborhood exceeds work budget", "size"
					);
					return Rgba{};
				}
				work += taps;
				for (int i = -radius; i <= radius; ++i)
					for (int j = -radius; j <= radius; ++j) {
						const double dist = i * i + j * j;
						if (dist >= square) continue;
						const double amplitude = 1 - dist / square;
						Rgba sample = SampleTextureUv(
							surface,
							u + double(i) / surface.Width,
							v + double(j) / surface.Height,
							1 - amplitude,
							uv,
							sampler
						);
						double difference = 0;
						for (size_t channel = 0; channel < 3; ++channel)
							difference +=
								(center[channel] - sample[channel]) * (center[channel] - sample[channel]);
						if (center[3] != sample[3] || std::sqrt(difference) / 1.7320508076 > threshold)
							continue;
						for (size_t channel = 0; channel < 4; ++channel) {
							if (gamma && channel < 3) {
								if (sample[channel] < 0) {
									context.Fail(
										Status::UnsupportedExecution,
										"Contrast Blur source gamma power is undefined for "
										"negative samples",
										"surface_in"
									);
									return Rgba{};
								}
								sample[channel] = std::pow(sample[channel], 2.2);
							}
							colour[channel] += sample[channel] * amplitude;
						}
						divisor += amplitude;
					}
				for (size_t channel = 0; channel < 4; ++channel) {
					colour[channel] /= divisor;
					if (gamma && channel < 3) {
						if (colour[channel] < 0) {
							context.Fail(
								Status::UnsupportedExecution,
								"Contrast Blur source inverse gamma power is undefined",
								"surface_in"
							);
							return Rgba{};
						}
						colour[channel] = std::pow(colour[channel], 1 / 2.2);
					}
				}
				return colour;
			}
		);
	}
} // namespace engine::imagegraph::detail
