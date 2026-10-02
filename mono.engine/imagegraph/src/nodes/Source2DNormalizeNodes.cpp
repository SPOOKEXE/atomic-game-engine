#include "../SourceSafeDraw.hpp"
#include "Processor.hpp"

namespace engine::imagegraph::detail {
	bool SourceNormalize(NodeContext &context) {
		bool inactiveFailed = false;
		if (CopyWhenInactive(context, inactiveFailed)) return !inactiveFailed;
		const Image *surface = context.Input("surface_in");
		if (!surface)
			return context.Fail(Status::InvalidValue, "Normalize requires an input surface", "surface_in");
		const int64_t mode = context.Integer("modes");
		const bool single = surface->Format == SurfaceFormat::R8Unorm ||
							surface->Format == SurfaceFormat::R16Float ||
							surface->Format == SurfaceFormat::R32Float;
		const uint64_t pixels = uint64_t(surface->Width) * surface->Height;
		double minimum = 999999. / 255, maximum = -999999. / 255;
		if (mode == 0) {
			const auto format = DescribeSurfaceFormat(surface->Format);
			if (!format)
				return context.Fail(Status::InvalidValue, "Normalize input format is invalid", "surface_in");
			if (format->BytesPerPixel < 4)
				return context.Fail(
					Status::UnsupportedExecution,
					"Normalize source C range helper reads four bytes per pixel beyond this format's buffer",
					"surface_in"
				);
			const auto byte = [&](uint64_t offset) { return surface->Pixels[size_t(offset)]; };
			// surface_get_range_c reinterprets raw storage as byte RGBA, including float-format payloads.
			for (uint64_t i = 0; i < pixels; ++i) {
				if (byte(i * 4 + 3) == 0) continue;
				const double value = (double(byte(i * 4)) + byte(i * 4 + 1) + byte(i * 4 + 2)) / (3 * 255);
				minimum = std::min(minimum, value);
				maximum = std::max(maximum, value);
			}
			if (!single && minimum == maximum)
				return context.Fail(
					Status::UnsupportedExecution,
					"Normalize source global range division is undefined",
					"surface_in"
				);
		}
		if (single)
			return RunPixelProcessor(context, [](const Image &image, uint32_t x, uint32_t y, double, double) {
				return SourceSafeDrawPixel(image, x, y);
			});
		const int64_t radius = context.Integer("radius", 4);
		if (mode == 1) {
			if (radius <= 0)
				return context.Fail(
					Status::UnsupportedExecution,
					"Normalize source local influence has an empty or undefined neighborhood",
					"radius"
				);
			if (radius > 4096)
				return context.Fail(
					Status::LimitExceeded, "Normalize source local radius exceeds work bounds", "radius"
				);
			const uint64_t side = radius < 0 ? 0 : uint64_t(radius) * 2 + 1;
			if (side && pixels > 64000000 / (side * side))
				return context.Fail(
					Status::LimitExceeded, "Normalize source local neighborhood exceeds work budget", "radius"
				);
		}
		return RunPixelProcessor(
			context, [&](const Image &image, uint32_t x, uint32_t y, double u, double v) {
				const Rgba source = ReadPixel(image, x, y);
				double low = minimum, high = maximum;
				if (mode == 1) {
					low = 1;
					high = 1;
					const double center = source[0] * .2126 + source[1] * .7152 + source[2] * .0722;
					for (int64_t i = -radius; i <= radius; ++i)
						for (int64_t j = -radius; j <= radius; ++j) {
							const double normalized = std::hypot(double(i) / radius, double(j) / radius);
							if (normalized > 1) continue;
							const double progress = std::clamp(1 - normalized, 0., 1.),
										 influence = progress * progress * (3 - 2 * progress);
							const Rgba neighbour = SampleNearest(
								image, u + double(i) / image.Width, v + double(j) / image.Height
							);
							const double luminance =
								neighbour[0] * .2126 + neighbour[1] * .7152 + neighbour[2] * .0722;
							const double value = center + (luminance - center) * influence;
							low = std::min(low, value);
							high = std::max(high, value);
						}
					if (low == high) {
						context.Fail(
							Status::UnsupportedExecution,
							"Normalize source local range division is undefined",
							"radius"
						);
						return Rgba{};
					}
				}
				return Rgba{
					(source[0] - low) / (high - low),
					(source[1] - low) / (high - low),
					(source[2] - low) / (high - low),
					source[3]
				};
			}
		);
	}
}
