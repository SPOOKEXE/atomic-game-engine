#include "SourceTileRandom.hpp"

// The MIT License
// Copyright © 2015 Inigo Quilez
// Permission is hereby granted, free of charge, to any person obtaining a copy of this software and
// associated documentation files (the "Software"), to deal in the Software without restriction,
// including without limitation the rights to use, copy, modify, merge, publish, distribute, sublicense,
// and/or sell copies of the Software, and to permit persons to whom the Software is furnished to do so,
// subject to the following conditions: The above copyright notice and this permission notice shall be
// included in all copies or substantial portions of the Software. THE SOFTWARE IS PROVIDED "AS IS",
// WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
// MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS
// OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF
// CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
// DEALINGS IN THE SOFTWARE.
// https://www.youtube.com/c/InigoQuilez
// https://iquilezles.org
//
// CPU translation of pinned sh_tile_random, with shader_set_surface's nearest stage sampler.

#include "../AtlasPayload.hpp"
#include "Sampler.hpp"
#include "Source2DGenerator.hpp"

#include <array>
#include <cmath>
#include <limits>

namespace engine::imagegraph::detail {
	namespace {
		constexpr uint64_t TILE_RANDOM_WORK_LIMIT = 64000000;
		// Nine hash, exponential and texture taps bound both validation and drawing.
		constexpr uint64_t TILE_RANDOM_PIXEL_WORK = 9 * 64;
		struct TileRandomInputs {
			const Image *Source = nullptr;
			float RawWidth = 0, RawHeight = 0, ScaleX = 0, ScaleY = 0, Blend = 0;
			uint32_t Width = 0, Height = 0;
		};

		bool TileRandomFloat(NodeContext &context, double value, float &out, std::string_view port) {
			if (!std::isfinite(value) || std::abs(value) > std::numeric_limits<float>::max())
				return context.Fail(
					Status::InvalidValue, "Tile Random control exceeds finite shader range", port
				);
			out = float(value);
			return std::isfinite(out) ||
				   context.Fail(
					   Status::InvalidValue, "Tile Random control exceeds finite shader range", port
				   );
		}

		bool PrepareTileRandom(NodeContext &context, TileRandomInputs &inputs) {
			if (const auto *value = context.Find("surface_in");
				value && std::holds_alternative<AtlasValue>(*value))
				return context.Fail(
					Status::UnsupportedExecution,
					"Tile Random raw texture binding requires a physical surface",
					"surface_in"
				);
			inputs.Source = context.Input("surface_in");
			if (!inputs.Source ||
				!ValidSurfaceLayout(*inputs.Source, Limits::MaximumDimension, Limits::MaximumEvaluationBytes))
				return context.Fail(
					Status::InvalidValue, "Tile Random requires a valid source surface", "surface_in"
				);
			const auto *dimensionValue = context.Find("dimension");
			if (dimensionValue && !std::holds_alternative<Vector2>(*dimensionValue) &&
				!std::holds_alternative<double>(*dimensionValue) &&
				!std::holds_alternative<int64_t>(*dimensionValue))
				return context.Fail(
					Status::UnsupportedExecution,
					"Tile Random dimension requires a numeric pair, scalar or surface dimension",
					"dimension"
				);
			Vector2 raw = context.Vec2("dimension", {1, 1});
			if (!context.IsLinked("dimension")) {
				const double unit = context.SourceChoice("dimension_unit", 1);
				if (context.FailureCode != Status::Ok) return false;
				if (unit != std::trunc(unit) || unit < 0 || unit > 2)
					return context.Fail(
						Status::UnsupportedExecution,
						"Tile Random dimension unit is undefined",
						"dimension_unit"
					);
				if (unit == 2)
					return context.Fail(
						Status::UnsupportedExecution,
						"Tile Random source Dimension has no Mask getter",
						"dimension_unit"
					);
				if (unit == 1) {
					raw.X *= context.Project.SurfaceWidth;
					raw.Y *= context.Project.SurfaceHeight;
				}
			}
			if (!std::isfinite(raw.X) || !std::isfinite(raw.Y))
				return context.Fail(
					Status::InvalidValue, "Tile Random dimensions must be finite", "dimension"
				);
			const double width = std::max(1., source2d::GeneratorRoundHalfEven(raw.X)),
						 height = std::max(1., source2d::GeneratorRoundHalfEven(raw.Y));
			if (width > Limits::MaximumDimension || height > Limits::MaximumDimension)
				return context.Fail(
					Status::LimitExceeded, "Tile Random dimensions exceed native limits", "dimension"
				);
			if (context.Request.MaximumImageDimension == 0 ||
				context.Request.MaximumImageDimension > Limits::MaximumDimension)
				return context.Fail(
					Status::InvalidValue, "Tile Random request dimension limit is invalid", "surface_out"
				);
			if (width > context.Request.MaximumImageDimension ||
				height > context.Request.MaximumImageDimension)
				return context.Fail(
					Status::LimitExceeded, "Tile Random dimensions exceed request limits", "dimension"
				);
			inputs.Width = uint32_t(width);
			inputs.Height = uint32_t(height);
			if (!TileRandomFloat(context, raw.X, inputs.RawWidth, "dimension") ||
				!TileRandomFloat(context, raw.Y, inputs.RawHeight, "dimension") ||
				!TileRandomFloat(context, raw.X / inputs.Source->Width, inputs.ScaleX, "dimension") ||
				!TileRandomFloat(context, raw.Y / inputs.Source->Height, inputs.ScaleY, "dimension") ||
				!TileRandomFloat(context, context.Scalar("randomness", .5), inputs.Blend, "randomness"))
				return false;
			return context.FailureCode == Status::Ok;
		}

		float TileRandomFract(float value) {
			return value - std::floor(value);
		}

		std::array<float, 4> TileRandomHash(float x, float y) {
			const std::array<float, 4> phase{
				1.f + (x * 37.f + y * 17.f),
				2.f + (x * 11.f + y * 47.f),
				3.f + (x * 41.f + y * 29.f),
				4.f + (x * 23.f + y * 31.f)
			};
			std::array<float, 4> result{};
			for (size_t component = 0; component < result.size(); ++component)
				result[component] = TileRandomFract(std::sin(phase[component]) * 103.f);
			return result;
		}

		bool TileRandomPixel(
			NodeContext &context, const TileRandomInputs &inputs, uint32_t x, uint32_t y, Rgba &pixel
		) {
			pixel = {};
			const float centerX = float(x) + .5f, centerY = float(y) + .5f;
			if (centerX >= inputs.RawWidth || centerY >= inputs.RawHeight) return true;
			// Sprite UVs use the raw draw extent; surface allocation uses its rounded extent.
			const float u = (centerX / inputs.RawWidth) * inputs.ScaleX,
						v = (centerY / inputs.RawHeight) * inputs.ScaleY;
			if (!std::isfinite(u) || !std::isfinite(v))
				return context.Fail(
					Status::InvalidValue, "Tile Random shader coordinate is nonfinite", "dimension"
				);
			const float px = std::floor(u), py = std::floor(v), fx = TileRandomFract(u),
						fy = TileRandomFract(v);
			std::array<float, 4> accumulated{};
			float sum = 0, squaredSum = 0;
			for (int j = -1; j <= 1; ++j)
				for (int i = -1; i <= 1; ++i) {
					const auto offset = TileRandomHash(px + float(i), py + float(j));
					const float rx = float(i) - fx + offset[0], ry = float(j) - fy + offset[1];
					const float weight = std::exp(-5.f * (rx * rx + ry * ry));
					const float shiftedU = u + inputs.Blend * offset[2],
								shiftedV = v + inputs.Blend * offset[3];
					if (!std::isfinite(shiftedU) || !std::isfinite(shiftedV) || !std::isfinite(weight))
						return context.Fail(
							Status::InvalidValue,
							"Tile Random shifted coordinate or weight is nonfinite",
							"randomness"
						);
					const auto sample =
						SampleNearest(*inputs.Source, TileRandomFract(shiftedU), TileRandomFract(shiftedV));
					for (size_t component = 0; component < accumulated.size(); ++component) {
						const float value = float(sample[component]);
						if (!std::isfinite(value))
							return context.Fail(
								Status::InvalidValue, "Tile Random source sample is nonfinite", "surface_in"
							);
						accumulated[component] += weight * value;
					}
					sum += weight;
					squaredSum += weight * weight;
				}
			const float normalization = std::sqrt(squaredSum);
			if (!std::isfinite(sum) || !std::isfinite(normalization) || sum <= 0 || normalization <= 0)
				return context.Fail(
					Status::InvalidValue, "Tile Random weight denominator is nonfinite or zero", "randomness"
				);
			for (size_t component = 0; component < accumulated.size(); ++component) {
				const float average = accumulated[component] / sum,
							corrected = .3f + (accumulated[component] - sum * .3f) / normalization;
				const float value = average * (1.f - inputs.Blend) + corrected * inputs.Blend;
				if (!std::isfinite(average) || !std::isfinite(corrected) || !std::isfinite(value))
					return context.Fail(
						Status::InvalidValue, "Tile Random shader sample is nonfinite", "randomness"
					);
				pixel[component] = value;
			}
			return true;
		}

		bool QuoteTileRandom(NodeContext &context, const TileRandomInputs &inputs, uint64_t &work) {
			const uint64_t pixels = uint64_t(inputs.Width) * inputs.Height;
			if (work > TILE_RANDOM_WORK_LIMIT ||
				pixels > (TILE_RANDOM_WORK_LIMIT - work) / TILE_RANDOM_PIXEL_WORK)
				return context.Fail(
					Status::LimitExceeded, "Tile Random complete batch exceeds work limits", "surface_out"
				);
			work += pixels * TILE_RANDOM_PIXEL_WORK;
			const uint64_t outputBytes = (work / TILE_RANDOM_PIXEL_WORK) * 4;
			if (outputBytes > context.AvailableBytes())
				return context.Fail(
					Status::LimitExceeded,
					"Tile Random complete batch exceeds live output byte limits",
					"surface_out"
				);
			for (uint32_t y = 0; y < inputs.Height; ++y)
				for (uint32_t x = 0; x < inputs.Width; ++x) {
					Rgba pixel;
					if (!TileRandomPixel(context, inputs, x, y, pixel)) return false;
				}
			return true;
		}
	}

	bool AdmitSourceTileRandom(NodeContext &context, uint64_t &batchWork) {
		ENGINE_PROFILE("imagegraph.source.tile_random_admission");
		TileRandomInputs inputs;
		return PrepareTileRandom(context, inputs) && QuoteTileRandom(context, inputs, batchWork);
	}

	bool DrawSourceTileRandom(NodeContext &context) {
		ENGINE_PROFILE("imagegraph.source.tile_random");
		TileRandomInputs inputs;
		uint64_t work = 0;
		if (!PrepareTileRandom(context, inputs) || !QuoteTileRandom(context, inputs, work)) return false;
		auto *out = context.NewImage("surface_out", inputs.Width, inputs.Height, SurfaceFormat::RGBA8Unorm);
		if (!out) return false;
		for (uint32_t y = 0; y < inputs.Height; ++y)
			for (uint32_t x = 0; x < inputs.Width; ++x) {
				Rgba pixel;
				if (!TileRandomPixel(context, inputs, x, y, pixel)) return false;
				if (!WritePixel(*out, x, y, pixel))
					return context.Fail(
						Status::InvalidValue, "Tile Random output sample is nonfinite", "surface_out"
					);
			}
		return true;
	}
}
