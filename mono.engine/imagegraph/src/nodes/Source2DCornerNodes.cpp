#include "Sampler.hpp"

namespace engine::imagegraph::detail {
	double SourceRound(double value);
	bool RoundCorner(NodeContext &context) {
		bool failed = false;
		if (CopyWhenInactive(context, failed)) return !failed;
		const Image *source = context.Input("surface_in");
		if (!source)
			return context.Fail(Status::InvalidValue, "Round Corner requires a surface", "surface_in");
		const uint32_t iterations = uint32_t(SourceRound(std::max(source->Width, source->Height) / 4.0));
		const uint64_t pixels = uint64_t(source->Width) * source->Height;
		if (pixels > 64000000 / (1089 + uint64_t(iterations) * 33))
			return context.Fail(Status::LimitExceeded, "Round Corner exceeds its sample work budget");
		const auto format = ResolveProcessorSurfaceFormat(context, source);
		if (!format) return false;
		auto charge = context.ReserveWorkspace(pixels * 8);
		if (!charge) return false;
		Image previous{source->Width, source->Height, std::vector<uint8_t>(pixels * 4), 0}, next = previous;
		for (uint32_t y = 0; y < source->Height; ++y)
			for (uint32_t x = 0; x < source->Width; ++x) {
				const Rgba original = ReadPixel(*source, x, y);
				const bool occupied = (original[0] + original[1] + original[2]) * original[3] / 3 > 0;
				if (!WritePixel(
						previous,
						x,
						y,
						occupied ? Rgba{(x + .5) / source->Width, (y + .5) / source->Height, 0, 1} : Rgba{}
					))
					return context.Fail(Status::InvalidValue, "Round Corner coordinates are nonfinite");
			}
		for (uint32_t iteration = 0; iteration < iterations; ++iteration) {
			for (uint32_t y = 0; y < source->Height; ++y)
				for (uint32_t x = 0; x < source->Width; ++x) {
					Rgba current = ReadPixel(previous, x, y);
					if (current[3] != 0) {
						for (const Vector2 direction :
							 {Vector2{1, 0}, Vector2{-1, 0}, Vector2{0, 1}, Vector2{0, -1}})
							for (uint32_t step = 1; step <= 8; ++step) {
								const Rgba sample = SampleNearest(
									previous,
									(x + .5 + direction.X * step) / source->Width,
									(y + .5 + direction.Y * step) / source->Height
								);
								if (sample[3] == 0) break;
								current[0] = std::min(current[0], sample[0]);
								current[1] = std::min(current[1], sample[1]);
							}
					} else
						current = {};
					if (!WritePixel(next, x, y, current))
						return context.Fail(Status::InvalidValue, "Round Corner propagation is nonfinite");
				}
			previous.Pixels.swap(next.Pixels);
		}
		Image *output = context.NewImage("surface_out", source->Width, source->Height, *format);
		if (!output) return false;
		const int64_t oversample = ReadSampler(context).Oversample;
		for (uint32_t y = 0; y < source->Height; ++y)
			for (uint32_t x = 0; x < source->Width; ++x) {
				const double u = (x + .5) / source->Width, v = (y + .5) / source->Height;
				const double radius = MappedScalar(context, "radius", u, v),
							 threshold = MappedScalar(context, "threshold", u, v);
				Rgba result{0, 0, 0, context.Boolean("transparent") ? 0.0 : 1.0};
				const Rgba center = ReadPixel(previous, x, y);
				if (center[3] != 0) {
					double fill = 0, size = 0;
					for (int dx = -16; dx <= 16; ++dx)
						for (int dy = -16; dy <= 16; ++dy) {
							if (std::abs(dx) > radius || std::abs(dy) > radius) continue;
							const Rgba sample = SampleTextureSimple(
								previous,
								u + double(dx) / source->Width,
								v + double(dy) / source->Height,
								oversample,
								false
							);
							++size;
							if (sample[0] == center[0] && sample[1] == center[1]) fill += sample[3];
						}
					if (size == 0)
						return context.Fail(
							Status::UnsupportedExecution,
							"Round Corner source division is undefined for an empty kernel",
							"radius"
						);
					if (!(fill / size < threshold)) result = ReadPixel(*source, x, y);
				}
				if (!WritePixel(*output, x, y, result))
					return context.Fail(Status::InvalidValue, "Round Corner output is nonfinite");
			}
		FinishProcessor(context, *source, *output);
		return context.FailureCode == Status::Ok;
	}
} // namespace engine::imagegraph::detail
