#include "SourcePixelSort.hpp"

#include "../AtlasPayload.hpp"
#include "Families.hpp"
#include "Source2DGenerator.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <vector>
namespace engine::imagegraph::detail {
	namespace {
		constexpr uint64_t PIXEL_SORT_WORK_LIMIT = 64000000;
		struct SortInputs {
			const Image *Source = nullptr, *Mask = nullptr;
			int64_t Iterations = 2;
			int Direction = 0;
			float Threshold = .1f;
			double Feather = 0;
			bool Inactive = false;
		};
		bool SortInteger(NodeContext &context, std::string_view port, int64_t fallback, int64_t &result) {
			if (const auto *value = context.Find(port))
				if (const auto *integer = std::get_if<int64_t>(value)) {
					result = *integer;
					return true;
				}
			const double number = source2d::GeneratorRoundHalfEven(context.Scalar(port, double(fallback)));
			if (!std::isfinite(number) || number < -0x1p63 || number >= 0x1p63)
				return context.Fail(
					Status::InvalidValue, "Pixel Sort integer exceeds finite host range", port
				);
			result = int64_t(number);
			return context.FailureCode == Status::Ok;
		}
		bool SortSurface(NodeContext &context, std::string_view port, const Image *image, bool raw) {
			if (!image) return true;
			if (!ValidSurfaceLayout(*image, Limits::MaximumDimension, Limits::MaximumEvaluationBytes))
				return context.Fail(Status::InvalidValue, "Pixel Sort surface layout is invalid", port);
			if (const auto *value = context.Find(port))
				if (const auto *atlas = std::get_if<AtlasValue>(value)) {
					if (!ValidAtlasPayload(*atlas))
						return context.Fail(Status::InvalidValue, "Pixel Sort Atlas is malformed", port);
					if (raw)
						return context.Fail(
							Status::UnsupportedExecution, "Pixel Sort raw mask binding rejects Atlas", port
						);
				}
			return true;
		}
		bool PrepareSort(NodeContext &context, SortInputs &inputs) {
			inputs.Source = context.Input("surface_in");
			if (!inputs.Source)
				return context.Fail(Status::InvalidValue, "Pixel Sort requires Surface In", "surface_in");
			if (!SortSurface(context, "surface_in", inputs.Source, false)) return false;
			inputs.Inactive = !context.Boolean("active", true);
			if (inputs.Inactive) return context.FailureCode == Status::Ok;
			if (!ResolveProcessorSurfaceFormat(context, inputs.Source)) return false;
			if (!SortInteger(context, "iteration", 2, inputs.Iterations)) return false;
			if (inputs.Iterations <= 0) return true;
			int64_t direction;
			if (!SortInteger(context, "direction", 0, direction)) return false;
			// grug floor division must keep the negative quarter-turn before modulo normalization.
			int64_t quarter = direction / 90;
			if (direction % 90 < 0) --quarter;
			inputs.Direction = int((quarter % 4 + 4) % 4);
			inputs.Threshold = float(context.Scalar("threshold", .1));
			if (!std::isfinite(inputs.Threshold))
				return context.Fail(
					Status::InvalidValue, "Pixel Sort threshold exceeds finite shader range", "threshold"
				);
			inputs.Mask = context.Input("mask");
			if (!SortSurface(context, "mask", inputs.Mask, true)) return false;
			inputs.Feather = context.Scalar("mask_feather");
			if (!std::isfinite(inputs.Feather))
				return context.Fail(
					Status::InvalidValue, "Pixel Sort mask feather is nonfinite", "mask_feather"
				);
			return context.FailureCode == Status::Ok;
		}
		bool QuoteSort(NodeContext &context, const SortInputs &inputs, uint64_t &work) {
			const uint64_t pixels = uint64_t(inputs.Source->Width) * inputs.Source->Height;
			long double cost = pixels;
			if (!inputs.Inactive && inputs.Iterations > 0) {
				cost += pixels * (128.L + 32.L * inputs.Iterations);
				if (inputs.Mask && inputs.Feather > 0) {
					const double radius = std::max(1., std::round(inputs.Feather));
					if (radius > std::numeric_limits<int>::max())
						return context.Fail(
							Status::LimitExceeded,
							"Pixel Sort mask feather exceeds supported radius",
							"mask_feather"
						);
					cost += static_cast<long double>(inputs.Mask->Width) * inputs.Mask->Height *
								(2 * (2 * radius - 1) * 16 + 2) +
							radius * 16;
				}
			}
			if (!std::isfinite(cost) || work > PIXEL_SORT_WORK_LIMIT || cost > PIXEL_SORT_WORK_LIMIT - work)
				return context.Fail(
					Status::LimitExceeded, "Pixel Sort complete batch exceeds work limit", "surface_out"
				);
			work += uint64_t(std::ceil(cost));
			return true;
		}
		float Brightness(const Rgba &pixel) {
			return float(pixel[0]) * .2126f + float(pixel[1]) * .7152f + float(pixel[2]) * .0722f;
		}
		bool CopySort(NodeContext &context, const Image &source, Image &target) {
			for (uint32_t y = 0; y < target.Height; ++y)
				for (uint32_t x = 0; x < target.Width; ++x)
					if (!WritePixel(target, x, y, ReadPixel(source, x, y)))
						return context.Fail(
							Status::InvalidValue,
							"Pixel Sort copy exceeds numeric storage range",
							"surface_out"
						);
			return true;
		}
		bool DrawSort(NodeContext &context) {
			ENGINE_PROFILE("imagegraph.source.pixel_sort");
			SortInputs inputs;
			uint64_t work = 0;
			if (!PrepareSort(context, inputs) || !QuoteSort(context, inputs, work)) return false;
			bool failed = false;
			if (CopyWhenInactive(context, failed)) return !failed;
			const auto format = ResolveProcessorSurfaceFormat(context, inputs.Source);
			if (!format) return false;
			const uint32_t width = inputs.Source->Width, height = inputs.Source->Height;
			if (inputs.Iterations <= 0) {
				auto *output = context.NewImage("surface_out", width, height, *format);
				return output && CopySort(context, *inputs.Source, *output);
			}
			const uint64_t bytes = uint64_t(width) * height * 4;
			auto charge = context.ReserveWorkspace(bytes * 2, "surface_out");
			if (!charge) return false;
			// grug the pinned host uses default RGBA8 scratch even when output depth is floating point.
			std::array<Image, 2> scratch{
				{{width, height, std::vector<uint8_t>(size_t(bytes)), 0, SurfaceFormat::RGBA8Unorm},
				 {width, height, std::vector<uint8_t>(size_t(bytes)), 0, SurfaceFormat::RGBA8Unorm}}
			};
			if (!CopySort(context, *inputs.Source, scratch[1])) return false;
			for (int64_t iteration = 0; iteration < inputs.Iterations; ++iteration) {
				Image &target = scratch[size_t(iteration % 2)];
				const Image &source = scratch[size_t(1 - iteration % 2)];
				for (uint32_t y = 0; y < height; ++y)
					for (uint32_t x = 0; x < width; ++x) {
						const bool horizontal = inputs.Direction == 0 || inputs.Direction == 2;
						const int parity = int(iteration % 2) * 2 - 1,
								  vertex = int((horizontal ? x : y) % 2) * 2 - 1;
						const int dx = horizontal ? parity * vertex : 0,
								  dy = horizontal ? 0 : -parity * vertex;
						const int64_t nx = int64_t(x) + dx, ny = int64_t(y) + dy;
						const auto current = ReadPixel(source, x, y);
						auto result = current;
						if (nx >= 0 && ny >= 0 && nx < width && ny < height) {
							const auto compare = ReadPixel(source, uint32_t(nx), uint32_t(ny));
							const float curr = Brightness(current), comp = Brightness(compare);
							const bool shift = inputs.Direction == 0   ? dx < 0
											   : inputs.Direction == 1 ? dy < 0
											   : inputs.Direction == 2 ? dx > 0
																	   : dy > 0;
							// grug source ties copy full RGBA on one side, so equal RGB can duplicate alpha.
							if (shift ? curr > inputs.Threshold && comp > curr
									  : comp > inputs.Threshold && curr >= comp)
								result = compare;
						}
						if (!WritePixel(target, x, y, result))
							return context.Fail(
								Status::InvalidValue,
								"Pixel Sort pass exceeds numeric storage range",
								"surface_out"
							);
					}
			}
			auto *output = context.NewImage("surface_out", width, height, *format);
			if (!output) return false;
			if (!CopySort(context, scratch[size_t((inputs.Iterations - 1) % 2)], *output)) return false;
			FinishProcessor(context, *inputs.Source, *output);
			return context.FailureCode == Status::Ok;
		}
	}
	bool AdmitSourcePixelSort(NodeContext &context, uint64_t &batchWork) {
		SortInputs inputs;
		return PrepareSort(context, inputs) && QuoteSort(context, inputs, batchWork);
	}
	std::span<const ExecutorEntry> SourcePixelSortExecutors() {
		static constexpr ExecutorEntry entries[]{{"pc.pixel_sort", DrawSort, true}};
		return entries;
	}
}
