#include "../SourceSafeDraw.hpp"
#include "Processor.hpp"
#include "Sampler.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>

namespace engine::imagegraph::detail {
	bool SourceEdgeDetect(NodeContext &context) {
		bool inactiveFailed = false;
		if (CopyWhenInactive(context, inactiveFailed)) return !inactiveFailed;
		const Image *source = context.Input("surface_in");
		if (!source)
			return context.Fail(Status::InvalidValue, "Edge Detect requires its input surface", "surface_in");
		const bool single = source->Format == SurfaceFormat::R8Unorm ||
							source->Format == SurfaceFormat::R16Float ||
							source->Format == SurfaceFormat::R32Float;
		if (single)
			return RunPixelProcessor(
				context, [](const Image &surface, uint32_t x, uint32_t y, double, double) {
					return SourceSafeDrawPixel(surface, x, y);
				}
			);
		const int64_t algorithm = context.Integer("algorithm"), mode = context.Integer("color");
		if (algorithm < 0 || algorithm > 3)
			return context.Fail(Status::InvalidValue, "Edge Detect algorithm is invalid", "algorithm");
		if (mode < 0 || mode > 2)
			return context.Fail(Status::InvalidValue, "Edge Detect color mode is invalid", "color");
		const Vector2 levelIn = context.Vec2("level_in", {0, 1}),
					  levelOut = context.Vec2("level_out", {0, 1});
		if (levelIn.X == levelIn.Y)
			return context.Fail(
				Status::UnsupportedExecution, "Edge Detect source level division is undefined", "level_in"
			);
		std::array<int32_t, 9> sides{};
		if (algorithm == 3) {
			const Value *value = context.Find("attribute_filter");
			const ArrayValue *array = value ? std::get_if<ArrayValue>(value) : nullptr;
			if (!array || !array->Nested.empty() || (!array->Elements.empty() && !array->Items.empty()) ||
				(array->Items.empty() ? array->Elements.size() : array->Items.size()) != sides.size() ||
				(array->Items.empty() && array->ElementType != ValueType::Scalar &&
				 array->ElementType != ValueType::Integer && array->ElementType != ValueType::Boolean))
				return context.Fail(
					Status::InvalidValue,
					"Edge Detect requires nine flat numeric or boolean neighbor switches",
					"attribute_filter"
				);
			for (size_t i = 0; i < sides.size(); ++i) {
				const ElementValue *leaf = array->Items.empty()
											   ? &array->Elements[i]
											   : std::get_if<ElementValue>(&array->Items[i].Data);
				if (!leaf)
					return context.Fail(
						Status::InvalidValue,
						"Edge Detect neighbor switches cannot contain images or nested arrays",
						"attribute_filter"
					);
				double number = 0;
				if (const auto *real = std::get_if<double>(leaf))
					number = *real;
				else if (const auto *integer = std::get_if<int64_t>(leaf)) {
					if (*integer < std::numeric_limits<int32_t>::min() ||
						*integer > std::numeric_limits<int32_t>::max())
						return context.Fail(
							Status::InvalidValue,
							"Edge Detect neighbor switch exceeds source integer range",
							"attribute_filter"
						);
					number = double(*integer);
				} else if (const auto *boolean = std::get_if<bool>(leaf))
					number = *boolean ? 1 : 0;
				else
					return context.Fail(
						Status::InvalidValue,
						"Edge Detect neighbor switches must be numeric or boolean",
						"attribute_filter"
					);
				if (!std::isfinite(number) || number < std::numeric_limits<int32_t>::min() ||
					number > std::numeric_limits<int32_t>::max())
					return context.Fail(
						Status::InvalidValue,
						"Edge Detect neighbor switch exceeds source integer range",
						"attribute_filter"
					);
				if (std::trunc(number) != number)
					return context.Fail(
						Status::UnsupportedExecution,
						"Edge Detect fractional integer-uniform upload needs a source runtime observation",
						"attribute_filter"
					);
				sides[i] = static_cast<int32_t>(number);
			}
		}
		const uint64_t rows = std::max(uint64_t{1}, uint64_t(context.ProcessorCount));
		if (rows > 64000000 / 10 || uint64_t(source->Width) * source->Height > 64000000 / (10 * rows))
			return context.Fail(
				Status::LimitExceeded, "Edge Detect neighborhood exceeds work budget", "surface_in"
			);
		const bool filtered = Filtered(ReadSampler(context));
		constexpr std::array<double, 9> SOBEL{-1, -2, -1, 0, 0, 0, 1, 2, 1};
		constexpr std::array<double, 9> PREWITT{-1, -1, -1, 0, 0, 0, 1, 1, 1};
		constexpr std::array<double, 9> LAPLACIAN{1, 1, 1, 1, -8, 1, 1, 1, 1};
		return RunPixelProcessor(context, [&](const Image &surface, uint32_t, uint32_t, double u, double v) {
			const Rgba point = Texture(surface, u, v, filtered);
			Rgba h{}, vertical{};
			std::array<Rgba, 9> neighbors{};
			for (int i = -1; i <= 1; ++i)
				for (int j = -1; j <= 1; ++j) {
					// The pinned shader clamps every neighbor before sampler_simple; oversample is therefore
					// inert.
					const Rgba sample = Texture(
						surface,
						std::clamp(u + double(i) / surface.Width, 0., 1.),
						std::clamp(v + double(j) / surface.Height, 0., 1.),
						filtered
					);
					const size_t index = size_t(j + 1) * 3 + size_t(i + 1),
								 transposed = size_t(i + 1) * 3 + size_t(j + 1);
					neighbors[index] = sample;
					if (algorithm == 3) continue;
					const double hw = algorithm == 0   ? SOBEL[index]
									  : algorithm == 1 ? PREWITT[index]
													   : LAPLACIAN[index];
					const double vw = algorithm == 0   ? SOBEL[transposed]
									  : algorithm == 1 ? PREWITT[transposed]
													   : 0.;
					for (size_t c = 0; c < 4; ++c) {
						h[c] += sample[c] * hw;
						vertical[c] += sample[c] * vw;
					}
				}
			Rgba result = point;
			if (algorithm <= 1) {
				const double divisor = algorithm == 0 ? 4. : 3.;
				double square = 0;
				// GLSL distance operates on the complete RGBA vectors, rather than the usual two gradient
				// norms.
				for (size_t c = 0; c < 4; ++c) {
					const double difference = (h[c] - vertical[c]) / divisor;
					square += difference * difference;
				}
				result[0] = result[1] = result[2] = std::sqrt(square);
			} else if (algorithm == 2) {
				for (size_t c = 0; c < 3; ++c)
					result[c] = h[c] / 2.;
			} else {
				for (size_t c = 0; c < 3; ++c) {
					result[c] = 0;
					for (size_t n = 0; n < sides.size(); ++n)
						if (n != 4 && sides[n] == 1)
							result[c] = std::max(result[c], std::abs(neighbors[n][c] - neighbors[4][c]));
				}
			}
			for (size_t c = 0; c < 3; ++c)
				result[c] = levelOut.X +
							(levelOut.Y - levelOut.X) * (result[c] - levelIn.X) / (levelIn.Y - levelIn.X);
			const double brightness = (result[0] * .2126 + result[1] * .7152 + result[2] * .0722) * result[3];
			if (mode == 1) result[0] = result[1] = result[2] = brightness;
			if (mode == 2) result[0] = result[1] = result[2] = brightness >= .5 ? 1. : 0.;
			return result;
		});
	}
}
