#include "SourceErodeNodes.hpp"

#include "../SourceMappedInputs.hpp"
#include "Sampler.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>
#include <type_traits>

namespace engine::imagegraph::detail {
	namespace {
		constexpr uint64_t MAXIMUM_ERODE_SAMPLE_WORK = 64'000'000;
		const Value *WholeValue(const NodeContext &context, std::string_view port) {
			for (auto it = context.ProcessorOriginalValues.rbegin();
				 it != context.ProcessorOriginalValues.rend();
				 ++it)
				if (it->first == port) return it->second;
			for (const auto &[id, value] : context.Values)
				if (id == port) return &value;
			return context.Find(port);
		}
		template <class T> bool NumberBound(const T &value, double &maximum) {
			if constexpr (std::is_same_v<T, double> || std::is_same_v<T, int64_t>) {
				if (!std::isfinite(float(value))) return false;
				maximum = std::max(maximum, std::abs(double(float(value))));
				return true;
			} else if constexpr (std::is_same_v<T, Vector2>) {
				return NumberBound(value.X, maximum) && NumberBound(value.Y, maximum);
			} else if constexpr (std::is_same_v<T, ArrayValue>) {
				if (!value.Items.empty()) return false;
				for (const auto &item : value.Elements)
					if (!std::visit([&](const auto &leaf) { return NumberBound(leaf, maximum); }, item))
						return false;
				for (const auto &row : value.Nested)
					for (const auto &item : row)
						if (!std::visit([&](const auto &leaf) { return NumberBound(leaf, maximum); }, item))
							return false;
				return true;
			} else
				return false;
		}
		bool AnyActive(const Value *value) {
			if (!value) return true;
			if (const auto *flag = std::get_if<bool>(value)) return *flag;
			if (const auto *array = std::get_if<ArrayValue>(value)) {
				for (const auto &item : array->Elements)
					if (const auto *flag = std::get_if<bool>(&item); !flag || *flag) return true;
				return !array->Nested.empty() || !array->Items.empty();
			}
			return true;
		}
		// Admit the worst selected row across the complete source batch before creating its first image.
		bool AdmitBatch(NodeContext &context) {
			if (context.ProcessorRow != 0) return true;
			uint64_t pixels = 0;
			uint64_t pixelBytes = 1;
			for (const auto &[port, image] : context.Images)
				if (port == "surface_in") {
					pixels = std::max(pixels, uint64_t(image->Width) * image->Height);
					if (const auto format = DescribeSurfaceFormat(image->Format))
						pixelBytes = std::max(pixelBytes, uint64_t(format->BytesPerPixel));
				}
			for (const auto &[port, array] : context.ImageArrays)
				if (port == "surface_in")
					for (const auto &image : array->Images) {
						pixels = std::max(pixels, uint64_t(image.Width) * image.Height);
						if (const auto format = DescribeSurfaceFormat(image.Format))
							pixelBytes = std::max(pixelBytes, uint64_t(format->BytesPerPixel));
					}
			if (!pixels)
				return context.Fail(Status::InvalidValue, "erode requires a source surface", "surface_in");
			const uint64_t rows = std::max<size_t>(1, context.ProcessorCount);
			double maximum = 0;
			if (AnyActive(WholeValue(context, "active"))) {
				const auto *width = WholeValue(context, "width");
				const auto *range = WholeValue(context, "width_map_range");
				if (!width && !range) maximum = 1;
				if ((width && !std::visit([&](const auto &v) { return NumberBound(v, maximum); }, *width)) ||
					(range && !std::visit([&](const auto &v) { return NumberBound(v, maximum); }, *range)))
					return context.Fail(
						Status::UnsupportedExecution,
						"erode width exceeds finite source shader range",
						"width"
					);
			}
			// Radial takes 65 directions per ring. Box and Diamond visit their complete bounding square.
			if (maximum > MAXIMUM_ERODE_SAMPLE_WORK)
				return context.Fail(
					Status::LimitExceeded, "erode exceeds bounded aggregate sample work", "width"
				);
			const uint64_t rings = uint64_t(std::floor(maximum));
			const uint64_t side = uint64_t(std::ceil(2 * maximum)) + 1;
			uint64_t offsets = std::max(rings * 65, side * side);
			if (const auto *pattern = WholeValue(context, "pattern")) {
				const auto mode = SourceChoiceNumber(*pattern);
				if (mode && *mode == 0)
					offsets = rings * 65;
				else if (mode && *mode == 3)
					offsets = rings * 4;
				else if (mode && (*mode == 1 || *mode == 2))
					offsets = side * side;
			}
			bool hasMap = context.Input("width_map") != nullptr;
			for (const auto &[port, array] : context.ImageArrays)
				if (port == "width_map" && !array->Images.empty()) hasMap = true;
			const Value *mapping = WholeValue(context, "width_mapped");
			const bool mapRead = hasMap && mapping && AnyActive(mapping);
			// sampler_simple with filtered=false always selects one nearest tap. Oversampling
			// changes addressing or returns a constant; it never enables a multi-tap filter.
			// Each fragment also reads its base texel, optionally reads the map, and writes once.
			const uint64_t samples = offsets + 2 + uint64_t(mapRead);
			if (rows > MAXIMUM_ERODE_SAMPLE_WORK / samples ||
				pixels > MAXIMUM_ERODE_SAMPLE_WORK / (rows * samples))
				return context.Fail(
					Status::LimitExceeded, "erode exceeds bounded aggregate sample work", "width"
				);
			const int64_t depth = context.Integer("attribute_color_depth", 0);
			if (depth != 0 && AnyActive(WholeValue(context, "active"))) {
				const auto format = ResolveProcessorSurfaceFormat(context, context.Input("surface_in"));
				if (!format) return false;
				pixelBytes = std::max(pixelBytes, uint64_t(DescribeSurfaceFormat(*format)->BytesPerPixel));
			}
			// Reserve all output payloads together at their resolved format and maximum row dimensions;
			// actual row publication keeps its normal ledger charge and shares no extra persistent storage.
			if (pixels > Limits::MaximumEvaluationBytes / rows / pixelBytes)
				return context.Fail(
					Status::LimitExceeded, "erode batch output exceeds byte budget", "surface_out"
				);
			auto charge = context.ReserveWorkspace(pixels * rows * pixelBytes, "surface_out");
			return bool(charge);
		}
	}
	bool SourceErode(NodeContext &context) {
		ENGINE_PROFILE("imagegraph.erode");
		if (!AdmitBatch(context)) return false;
		bool failed = false;
		if (CopyWhenInactive(context, failed)) return !failed;
		if (context.Request.RequireSourceGpuRasterCoverage)
			return context.Fail(
				Status::UnsupportedExecution,
				"erode requires captured GameMaker GPU sampling coverage",
				"surface_out"
			);
		const Image *source = context.Input("surface_in");
		if (!source)
			return context.Fail(Status::InvalidValue, "erode requires a source surface", "surface_in");
		Vector2 range;
		const bool mapped = context.Boolean("width_mapped");
		if (mapped) {
			if (!ReadSourceMappedRange(context, "width", range)) return false;
		} else {
			const double width = context.Scalar("width", 1);
			range = {width, width};
		}
		const float low = float(range.X), high = float(range.Y);
		if (!std::isfinite(low) || !std::isfinite(high) || !std::isfinite(high - low))
			return context.Fail(
				Status::UnsupportedExecution, "erode width exceeds finite source shader range", "width"
			);
		const Image *map = mapped ? context.Input("width_map") : nullptr;
		const float maximum = map ? std::max(std::abs(low), std::abs(high)) : std::abs(low);
		const int64_t mode = context.Integer("pattern");
		// The shader has no fallback branch: an out-of-range source Pattern leaves the base sample.
		const int64_t oversample = ReadSampler(context).Oversample;
		return RunPixelProcessor(context, [&](const Image &image, uint32_t, uint32_t, double du, double dv) {
			const float u = float(du), v = float(dv);
			float size = low;
			if (map) {
				const auto sample = Texture(*map, u, v, false);
				const float amount = (float(sample[0]) + float(sample[1]) + float(sample[2])) / 3.f;
				size = low * (1.f - amount) + high * amount;
			}
			if (!std::isfinite(size)) return Rgba{std::numeric_limits<double>::quiet_NaN(), 0, 0, 0};
			const bool erode = size > 0;
			size = std::abs(size);
			Rgba colour = Texture(image, u, v, false);
			float fillAlpha = erode ? float(colour[3]) : 0.f;
			const auto check = [&](float x, float y) {
				const auto sample = SampleTextureSimple(
					image, u + x / float(image.Width), v + y / float(image.Height), oversample, false
				);
				const float alpha = float(sample[3]);
				if (erode && alpha < fillAlpha) {
					fillAlpha = alpha;
					colour[3] = alpha;
				} else if (!erode && alpha > fillAlpha) {
					fillAlpha = alpha;
					colour = {float(sample[0]), float(sample[1]), float(sample[2]), alpha};
				}
			};
			if (mode == 0) {
				for (float i = 1; i <= maximum; ++i) {
					if (i > size) break;
					float base = 1, top = 0;
					for (int j = 0; j <= 64; ++j) {
						const float angle = top / base * float(2 * std::numbers::pi);
						top += 2;
						if (top >= base) {
							top = 1;
							base *= 2;
						}
						check(std::cos(angle) * i, std::sin(angle) * i);
					}
				}
			} else if (mode == 1 || mode == 2) {
				for (float i = -maximum; i <= maximum; ++i) {
					if (i < -size) continue;
					if (i > size) break;
					for (float j = -maximum; j <= maximum; ++j) {
						if (j < -size) continue;
						if (j > size) break;
						if (mode == 2 && std::abs(i) + std::abs(j) > size) continue;
						check(i, j);
					}
				}
			} else if (mode == 3) {
				for (float i = 1; i <= maximum; ++i) {
					if (i > size) break;
					check(i, 0);
				}
				for (float i = 1; i <= maximum; ++i) {
					if (i > size) break;
					check(-i, 0);
				}
				for (float i = 1; i <= maximum; ++i) {
					if (i > size) break;
					check(0, i);
				}
				for (float i = 1; i <= maximum; ++i) {
					if (i > size) break;
					check(0, -i);
				}
			}
			return colour;
		});
	}
	std::span<const ExecutorEntry> SourceErodeExecutors() {
		static const ExecutorEntry entries[] = {{"pc.erode", SourceErode, true}};
		return entries;
	}
}
