#include "Sampler.hpp"
#include "SourceSafeDraw.hpp"

namespace engine::imagegraph::detail {
	namespace {
		double StrayDistance(const Rgba &a, const Rgba &b) {
			double sum = 0;
			for (size_t c = 0; c < 3; ++c)
				sum += std::pow(a[c] * a[3] - b[c] * b[3], 2);
			return std::sqrt(sum) / 1.7320508076;
		}
		Rgba StrayPixel(NodeContext &context, const Image &image, uint32_t x, uint32_t y) {
			const double u = (x + .5) / image.Width, v = (y + .5) / image.Height;
			std::array<Rgba, 9> neighbours;
			for (int dy = -1; dy <= 1; ++dy)
				for (int dx = -1; dx <= 1; ++dx)
					neighbours[(dy + 1) * 3 + dx + 1] =
						SampleNearest(image, u + double(dx) / image.Width, v + double(dy) / image.Height);
			const Rgba &center = neighbours[4];
			// The source binds the Active junction, whose mapped flag is false, to
			// toleranceUseSurf.
			const double tolerance = context.Boolean("tolerance_mapped")
										 ? context.Vec2("tolerance_map_range").X
										 : context.Scalar("tolerance");
			const auto same = [&](size_t a, size_t b) {
				return StrayDistance(neighbours[a], neighbours[b]) <= tolerance;
			};
			const auto select = [&](std::initializer_list<size_t> candidates) {
				size_t best = *candidates.begin();
				for (size_t index : candidates)
					if (StrayDistance(center, neighbours[index]) < StrayDistance(center, neighbours[best]))
						best = index;
				return neighbours[best];
			};
			if (center[3] == 0) return context.Boolean("fill_empty") ? select({1, 3, 5, 7}) : center;
			const int64_t strict = context.Integer("strictness");
			Rgba result = center;
			if (strict == 0) {
				if (!same(4, 1) && same(1, 3) && same(1, 5)) result = select({1, 3, 5});
				if (!same(4, 3) && same(3, 1) && same(3, 7)) result = select({3, 1, 7});
				if (!same(4, 5) && same(5, 1) && same(5, 7)) result = select({5, 1, 7});
				if (!same(4, 7) && same(7, 3) && same(7, 5)) result = select({7, 3, 5});
			} else if ((strict == 1 || strict == 2) && !same(4, 1) && same(1, 3) && same(1, 5) &&
					   same(1, 7) && (strict == 1 || (same(1, 0) && same(1, 2) && same(1, 6) && same(1, 8))))
				result = select({1, 3, 5, 7});
			return result;
		}
	} // namespace
	bool DeStray(NodeContext &context) {
		bool failed = false;
		if (CopyWhenInactive(context, failed)) return !failed;
		const Image *source = context.Input("surface_in");
		if (!source) return context.Fail(Status::InvalidValue, "De-Stray requires a surface", "surface_in");
		const int64_t iterations = std::max<int64_t>(0, context.Integer("iteration", 2));
		if (uint64_t(iterations) > 64000000 / 9 / source->Width / source->Height)
			return context.Fail(
				Status::LimitExceeded, "De-Stray exceeds the sample work budget", "iteration"
			);
		const auto format = ResolveProcessorSurfaceFormat(context, source);
		if (!format) return false;
		const uint64_t bytes = uint64_t(source->Width) * source->Height * 4;
		auto charge = context.ReserveWorkspace(bytes * 2);
		if (!charge) return false;
		Image previous{source->Width, source->Height, std::vector<uint8_t>(bytes), 0}, next = previous;
		for (uint32_t y = 0; y < source->Height; ++y)
			for (uint32_t x = 0; x < source->Width; ++x)
				if (!WritePixel(previous, x, y, SourceSafeDrawPixel(*source, x, y)))
					return context.Fail(Status::InvalidValue, "De-Stray input is nonfinite");
		for (int64_t iteration = 0; iteration < iterations; ++iteration) {
			for (uint32_t y = 0; y < source->Height; ++y)
				for (uint32_t x = 0; x < source->Width; ++x)
					if (!WritePixel(next, x, y, StrayPixel(context, previous, x, y)))
						return context.Fail(Status::InvalidValue, "De-Stray sample is nonfinite");
			previous.Pixels.swap(next.Pixels);
		}
		Image *output = context.NewImage("surface_out", source->Width, source->Height, *format);
		if (!output) return false;
		for (uint32_t y = 0; y < source->Height; ++y)
			for (uint32_t x = 0; x < source->Width; ++x)
				if (!WritePixel(*output, x, y, ReadPixel(previous, x, y)))
					return context.Fail(Status::InvalidValue, "De-Stray result is nonfinite");
		FinishProcessor(context, *source, *output);
		return context.FailureCode == Status::Ok;
	}
} // namespace engine::imagegraph::detail
