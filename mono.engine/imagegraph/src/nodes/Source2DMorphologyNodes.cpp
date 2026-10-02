#include "Sampler.hpp"

namespace engine::imagegraph::detail {
	namespace {
		struct NeighbourComparison {
			Rgba Center;
			double Tolerance;
			double Distance(const Rgba &first, const Rgba &second) const {
				double sum = 0;
				for (size_t channel = 0; channel < 3; ++channel)
					sum += std::pow(first[channel] * first[3] - second[channel] * second[3], 2);
				return std::sqrt(sum / 3);
			}
			bool S(const Rgba &colour) const {
				return Distance(Center, colour) <= Tolerance;
			}
			bool S(const Rgba &first, const Rgba &second) const {
				return Distance(first, second) <= Tolerance;
			}
			bool S(bool ignore, const Rgba &colour) const {
				return ignore || S(colour);
			}
			bool S(bool ignore, const Rgba &first, const Rgba &second) const {
				return ignore || S(first, second);
			}
			bool NS(const Rgba &colour) const {
				return Distance(Center, colour) > Tolerance;
			}
			bool NS(bool ignore, const Rgba &colour) const {
				return ignore || NS(colour);
			}
		};

		// Preserve source predicate order because the first matched corner wins.
		Rgba DeCornerPixel(NodeContext &context, const Image &image, double u, double v, int64_t oversample) {
			const auto sample = [&](int x, int y) {
				return SampleTextureSimple(
					image, u + double(x) / image.Width, v + double(y) / image.Height, oversample, false
				);
			};
			const Rgba a4 = SampleNearest(image, u, v);
			Rgba result = a4;
			if (a4[3] == 0) return result;
			const Rgba a0 = sample(-1, -1), a1 = sample(0, -1), a2 = sample(1, -1), a3 = sample(-1, 0),
					   a5 = sample(1, 0), a6 = sample(-1, 1), a7 = sample(0, 1), a8 = sample(1, 1);
			const NeighbourComparison comparison{a4, MappedScalar(context, "tolerance", u, v)};
			const auto sel2 = [&](const Rgba &first, const Rgba &second) {
				return comparison.Distance(a4, first) <= comparison.Distance(a4, second) ? first : second;
			};
			const auto sel3 = [&](const Rgba &first, const Rgba &second, const Rgba &third) {
				return sel2(sel2(first, second), third);
			};
			const int64_t strict = context.Integer("type"), include = context.Integer("include", 3);
			const int inner = (include & 1) != 0, side = (include & 2) != 0;
			bool ignoreInner = inner == 0;
			bool ignoreSides = side == 0;

			if (strict == 0) {

				if (comparison.S(ignoreInner, a0) && comparison.S(a1) && comparison.S(a3) &&
					comparison.NS(ignoreSides, a2) && comparison.NS(ignoreSides, a5) &&
					comparison.NS(ignoreSides, a6) && comparison.NS(ignoreSides, a7) && comparison.NS(a8)) {

					result = ignoreInner ? sel3(a5, a7, a8) : sel3(sel2(a2, a6), sel2(a5, a7), a8);
					return result;
				}

				if (comparison.S(a1) && comparison.S(ignoreInner, a2) && comparison.S(a5) &&
					comparison.NS(ignoreSides, a0) && comparison.NS(ignoreSides, a3) && comparison.NS(a6) &&
					comparison.NS(ignoreSides, a7) && comparison.NS(ignoreSides, a8)) {

					result = ignoreInner ? sel3(a3, a6, a7) : sel3(sel2(a0, a8), sel2(a3, a7), a6);
					return result;
				}

				if (comparison.S(a3) && comparison.S(ignoreInner, a6) && comparison.S(a7) &&
					comparison.NS(ignoreSides, a0) && comparison.NS(ignoreSides, a1) && comparison.NS(a2) &&
					comparison.NS(ignoreSides, a5) && comparison.NS(ignoreSides, a8)) {

					result = ignoreInner ? sel3(a1, a2, a5) : sel3(sel2(a0, a8), sel2(a1, a5), a2);
					return result;
				}

				if (comparison.S(a5) && comparison.S(a7) && comparison.S(ignoreInner, a8) &&
					comparison.NS(a0) && comparison.NS(ignoreSides, a1) && comparison.NS(ignoreSides, a2) &&
					comparison.NS(ignoreSides, a3) && comparison.NS(ignoreSides, a6)) {

					result = ignoreInner ? sel3(a0, a1, a3) : sel3(sel2(a2, a6), sel2(a1, a3), a0);
					return result;
				}

			} else if (strict == 1) {
				if (comparison.S(a5, a7) && comparison.S(a1) && comparison.S(a3) &&
					comparison.S(ignoreInner, a0) && comparison.NS(ignoreSides, a2) &&
					comparison.NS(ignoreSides, a5) && comparison.NS(ignoreSides, a6) &&
					comparison.NS(ignoreSides, a7)) {

					result = ignoreInner ? sel3(a5, a7, a8) : sel3(sel2(a2, a6), sel2(a5, a7), a8);
					return result;
				}

				if (comparison.S(a3, a7) && comparison.S(a1) && comparison.S(ignoreInner, a2) &&
					comparison.S(a5) && comparison.NS(ignoreSides, a0) && comparison.NS(ignoreSides, a3) &&
					comparison.NS(ignoreSides, a7) && comparison.NS(ignoreSides, a8)) {

					result = ignoreInner ? sel3(a3, a6, a7) : sel3(sel2(a0, a8), sel2(a3, a7), a6);
					return result;
				}

				if (comparison.S(a5, a1) && comparison.S(a3) && comparison.S(ignoreInner, a6) &&
					comparison.S(a7) && comparison.NS(ignoreSides, a0) && comparison.NS(ignoreSides, a1) &&
					comparison.NS(ignoreSides, a5) && comparison.NS(ignoreSides, a8)) {

					result = ignoreInner ? sel3(a1, a2, a5) : sel3(sel2(a0, a8), sel2(a1, a5), a2);
					return result;
				}

				if (comparison.S(a3, a1) && comparison.S(a5) && comparison.S(ignoreInner, a8) &&
					comparison.S(a7) && comparison.NS(ignoreSides, a2) && comparison.NS(ignoreSides, a1) &&
					comparison.NS(ignoreSides, a3) && comparison.NS(ignoreSides, a6)) {

					result = ignoreInner ? sel3(a0, a1, a3) : sel3(sel2(a2, a6), sel2(a1, a3), a0);
					return result;
				}
			}

			return result;
		}
	}

	bool DeCorner(NodeContext &context) {
		bool failed = false;
		if (CopyWhenInactive(context, failed)) return !failed;
		const Image *source = context.Input("surface_in");
		if (!source) return context.Fail(Status::InvalidValue, "Surface In is required", "surface_in");
		const int64_t iterations = context.Integer("iteration", 2);
		if (iterations < 0)
			return context.Fail(Status::InvalidValue, "Iteration must be nonnegative", "iteration");
		if (uint64_t(iterations) > 64'000'000 / 9 / source->Width / source->Height)
			return context.Fail(Status::LimitExceeded, "De-corner exceeds sample work budget", "iteration");
		const auto format = ResolveProcessorSurfaceFormat(context, source);
		if (!format) return false;
		const uint64_t pixels = uint64_t(source->Width) * source->Height;
		auto charge = context.ReserveWorkspace(pixels * 8, "surface_in");
		if (!charge) return false;
		Image previous{source->Width, source->Height, std::vector<uint8_t>(pixels * 4), 0},
			next{source->Width, source->Height, std::vector<uint8_t>(pixels * 4), 0};
		for (uint32_t y = 0; y < source->Height; ++y)
			for (uint32_t x = 0; x < source->Width; ++x)
				if (!WritePixel(previous, x, y, ReadPixel(*source, x, y)))
					return context.Fail(
						Status::InvalidValue, "Scratch sample exceeds surface range", "surface_in"
					);
		const int64_t oversample = ReadSampler(context).Oversample;
		for (int64_t iteration = 0; iteration < iterations; ++iteration) {
			for (uint32_t y = 0; y < source->Height; ++y)
				for (uint32_t x = 0; x < source->Width; ++x)
					if (!WritePixel(
							next,
							x,
							y,
							DeCornerPixel(
								context,
								previous,
								(x + .5) / source->Width,
								(y + .5) / source->Height,
								oversample
							)
						))
						return context.Fail(
							Status::InvalidValue, "De-corner sample exceeds surface range", "surface_out"
						);
			previous.Pixels.swap(next.Pixels);
		}
		Image *output = context.NewImage("surface_out", source->Width, source->Height, *format);
		if (!output) return false;
		for (uint32_t y = 0; y < source->Height; ++y)
			for (uint32_t x = 0; x < source->Width; ++x)
				if (!WritePixel(*output, x, y, ReadPixel(previous, x, y)))
					return context.Fail(
						Status::InvalidValue, "De-corner output exceeds surface range", "surface_out"
					);
		FinishProcessor(context, *source, *output);
		return context.FailureCode == Status::Ok;
	}
}
