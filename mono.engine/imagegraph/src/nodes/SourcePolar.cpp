#include "SourcePolar.hpp"

#include "../AtlasPayload.hpp"
#include "../SourceMappedInputs.hpp"
#include "Families.hpp"
#include "SourceRefractClean.hpp"

namespace engine::imagegraph::detail {
	namespace {
		constexpr uint64_t POLAR_WARP_WORK_LIMIT = 64000000;
		constexpr float POLAR_WARP_PI = 3.14159265358979323846f;
		struct PolarWarpMapped {
			const Image *Map = nullptr;
			float Value = 0, Low = 0, High = 0;
		};
		struct PolarWarpInputs {
			const Image *Source = nullptr, *Mask = nullptr;
			PolarWarpMapped Angle, Blend, Twist;
			Vector2 Center, Tile, Range;
			SamplerSettings Sampler;
			double Feather = 0;
			int64_t RadiusMode = 0;
			bool Invert = false, Swap = false, Inactive = false;
		};
		bool PolarWarpFloat(NodeContext &context, std::string_view port, double number, float &result) {
			result = float(number);
			return std::isfinite(result) ||
				   context.Fail(Status::InvalidValue, "Polar control exceeds finite shader range", port);
		}
		bool PolarWarpVector(NodeContext &context, std::string_view port, Vector2 &result) {
			const auto value = context.Vec2(port);
			float x, y;
			if (!PolarWarpFloat(context, port, value.X, x) || !PolarWarpFloat(context, port, value.Y, y))
				return false;
			result = {x, y};
			return true;
		}
		bool PolarWarpSurface(NodeContext &context, std::string_view port, const Image *image) {
			if (!image) return true;
			if (!ValidSurfaceLayout(*image, Limits::MaximumDimension, Limits::MaximumEvaluationBytes))
				return context.Fail(Status::InvalidValue, "Polar surface layout is invalid", port);
			if (const auto *value = context.Find(port); value && std::holds_alternative<AtlasValue>(*value))
				return context.Fail(
					Status::UnsupportedExecution, "Polar raw map binding rejects Atlas", port
				);
			return true;
		}
		bool PolarWarpPrepareMapped(NodeContext &context, std::string_view port, PolarWarpMapped &mapped) {
			const std::string name(port);
			if (!SourceRangeMapped(context, port))
				return PolarWarpFloat(context, port, context.Scalar(port), mapped.Value) &&
					   context.FailureCode == Status::Ok;
			Vector2 range;
			if (!ReadSourceMappedRange(context, port, range) ||
				!PolarWarpFloat(context, port, range.X, mapped.Low) ||
				!PolarWarpFloat(context, port, range.Y, mapped.High))
				return false;
			mapped.Value = mapped.Low;
			mapped.Map = context.Input(name + "_map");
			return PolarWarpSurface(context, name + "_map", mapped.Map) && context.FailureCode == Status::Ok;
		}
		bool PreparePolarWarp(NodeContext &context, PolarWarpInputs &inputs) {
			inputs.Source = context.Input("surface_in");
			if (!inputs.Source ||
				!ValidSurfaceLayout(*inputs.Source, Limits::MaximumDimension, Limits::MaximumEvaluationBytes))
				return context.Fail(Status::InvalidValue, "Polar requires valid Surface In", "surface_in");
			if (const auto *value = context.Find("surface_in"))
				if (const auto *atlas = std::get_if<AtlasValue>(value); atlas && !ValidAtlasPayload(*atlas))
					return context.Fail(Status::InvalidValue, "Polar Atlas is malformed", "surface_in");
			inputs.Inactive = !context.Boolean("active", true);
			if (inputs.Inactive) return context.FailureCode == Status::Ok;
			inputs.Sampler = ReadSampler(context);
			inputs.Invert = context.Boolean("invert");
			inputs.Swap = context.Boolean("swap_axis");
			inputs.RadiusMode = context.Integer("radius_mode");
			if (inputs.RadiusMode < 0 || inputs.RadiusMode > 2)
				return context.Fail(
					Status::UnsupportedExecution, "Polar radius mode is undefined", "radius_mode"
				);
			if (!PolarWarpVector(context, "tile", inputs.Tile) ||
				!PolarWarpVector(context, "range", inputs.Range) ||
				!PolarWarpPrepareMapped(context, "angle", inputs.Angle) ||
				!PolarWarpPrepareMapped(context, "blend", inputs.Blend) ||
				!PolarWarpPrepareMapped(context, "twist", inputs.Twist))
				return false;
			const auto unit = context.Integer("center_unit", 1);
			if (unit < 0 || unit > 1)
				return context.Fail(
					Status::UnsupportedExecution, "Polar center unit is undefined", "center_unit"
				);
			const auto center = context.Vec2("center");
			double centerX = center.X, centerY = center.Y;
			if (!context.IsLinked("center") && unit == 1) {
				centerX *= inputs.Source->Width;
				centerY *= inputs.Source->Height;
			}
			float uploadedX, uploadedY;
			if (!PolarWarpFloat(context, "center", centerX, uploadedX) ||
				!PolarWarpFloat(context, "center", centerY, uploadedY))
				return false;
			inputs.Center = {uploadedX / inputs.Source->Width, uploadedY / inputs.Source->Height};
			if (!std::isfinite(inputs.Center.X) || !std::isfinite(inputs.Center.Y))
				return context.Fail(Status::InvalidValue, "Polar center is nonfinite", "center");
			if (float(inputs.Range.Y - inputs.Range.X) == 0)
				return context.Fail(Status::InvalidValue, "Polar angular range divides by zero", "range");
			inputs.Mask = context.Input("mask");
			if (!PolarWarpSurface(context, "mask", inputs.Mask)) return false;
			inputs.Feather = context.Scalar("mask_feather");
			if (!std::isfinite(inputs.Feather))
				return context.Fail(Status::InvalidValue, "Polar mask feather is nonfinite", "mask_feather");
			return context.FailureCode == Status::Ok;
		}
		Rgba PolarWarpTexture(
			const Image &image, float u, float v, const SamplerSettings &settings, Vector2 dimension
		) {
			// grug source calls texture2Dintp directly, so Oversample does not affect these reads.
			return settings.Interpolation == 6 ? source_refract_clean::Texture(image, u, v, dimension)
											   : TextureInterpolated(image, u, v, settings, dimension);
		}
		bool PolarWarpMappedValue(
			NodeContext &context,
			std::string_view port,
			const PolarWarpMapped &mapped,
			const PolarWarpInputs &inputs,
			float u,
			float v,
			float &value
		) {
			value = mapped.Value;
			if (mapped.Map) {
				auto settings = inputs.Sampler;
				// grug shader_set_f_map binds each auxiliary texture with its own nearest stage filter.
				settings.ForceNearest = true;
				const auto sample = PolarWarpTexture(
					*mapped.Map, u, v, settings, {double(inputs.Source->Width), double(inputs.Source->Height)}
				);
				const float mean = (float(sample[0]) + float(sample[1]) + float(sample[2])) / 3.f;
				value = mapped.Low + (mapped.High - mapped.Low) * mean;
			}
			return std::isfinite(value) ||
				   context.Fail(Status::InvalidValue, "Polar mapped value is nonfinite", port);
		}
		bool PolarWarpCoordinate(
			NodeContext &context,
			const PolarWarpInputs &inputs,
			uint32_t x,
			uint32_t y,
			Vector2 &coordinate,
			bool &clipped
		) {
			const float u = (float(x) + .5f) / inputs.Source->Width,
						v = (float(y) + .5f) / inputs.Source->Height;
			float angleOffset, blend, twist;
			if (!PolarWarpMappedValue(context, "angle", inputs.Angle, inputs, u, v, angleOffset) ||
				!PolarWarpMappedValue(context, "blend", inputs.Blend, inputs, u, v, blend) ||
				!PolarWarpMappedValue(context, "twist", inputs.Twist, inputs, u, v, twist))
				return false;
			const float centerX = float(inputs.Center.X), centerY = float(inputs.Center.Y);
			const float deltaX = u - centerX, deltaY = v - centerY;
			float distance = inputs.Invert
								 ? u * .5f
								 : std::sqrt(deltaX * deltaX + deltaY * deltaY) / (std::sqrt(2.f) * .5f);
			if (inputs.RadiusMode == 1)
				distance = std::sqrt(distance);
			else if (inputs.RadiusMode == 2)
				distance = std::log(distance);
			if (!std::isfinite(distance))
				return context.Fail(
					Status::InvalidValue, "Polar radial distance is undefined", "radius_mode"
				);
			float angle = inputs.Invert ? v * POLAR_WARP_PI * 2
										: std::atan2(v - centerY, -(u - centerX)) + POLAR_WARP_PI;
			const float start = float(inputs.Range.X) * (POLAR_WARP_PI / 180.f);
			const float end = float(inputs.Range.Y) * (POLAR_WARP_PI / 180.f);
			const float rangeScale = (float(inputs.Range.Y) - float(inputs.Range.X)) / 360.f;
			angle = (angle - start) / rangeScale;
			if (!std::isfinite(angle))
				return context.Fail(Status::InvalidValue, "Polar angular range is undefined", "range");
			// grug keep source comparison after range scaling; conventional angular clipping differs.
			clipped = angle < start || angle > end;
			if (clipped) return true;
			angle -= angleOffset * (POLAR_WARP_PI / 180.f);
			angle += twist * distance;
			const float tileX = float(inputs.Swap ? inputs.Tile.Y : inputs.Tile.X),
						tileY = float(inputs.Swap ? inputs.Tile.X : inputs.Tile.Y);
			float mappedU, mappedV;
			if (inputs.Invert) {
				mappedU = centerX + std::cos(angle) * distance * tileX;
				mappedV = centerY + std::sin(angle) * distance * tileY;
			} else {
				mappedU = distance * tileX;
				mappedV = angle / (POLAR_WARP_PI * 2) * tileY;
			}
			if (!std::isfinite(mappedU) || !std::isfinite(mappedV))
				return context.Fail(
					Status::InvalidValue, "Polar mapped coordinate is undefined", "surface_out"
				);
			mappedU -= std::floor(mappedU);
			mappedV -= std::floor(mappedV);
			if (inputs.Swap) std::swap(mappedU, mappedV);
			const float sampleU = u + (mappedU - u) * blend, sampleV = v + (mappedV - v) * blend;
			if (!std::isfinite(sampleU) || !std::isfinite(sampleV))
				return context.Fail(Status::InvalidValue, "Polar sampling coordinate is undefined", "blend");
			coordinate = {sampleU, sampleV};
			return true;
		}
		bool QuotePolarWarp(NodeContext &context, const PolarWarpInputs &inputs, uint64_t &work) {
			const uint64_t pixels = uint64_t(inputs.Source->Width) * inputs.Source->Height;
			const uint64_t taps = inputs.Sampler.Interpolation == 6	  ? 32
								  : inputs.Sampler.Interpolation == 4 ? 16
								  : inputs.Sampler.Interpolation == 3 ? 8
								  : inputs.Sampler.Interpolation == 1 ? 1
																	  : 4;
			const uint64_t maps = bool(inputs.Angle.Map) + bool(inputs.Blend.Map) + bool(inputs.Twist.Map);
			long double rowWork = pixels * (inputs.Inactive ? 1 : 128 * (maps + 1) * taps);
			if (!inputs.Inactive && inputs.Mask && inputs.Feather > 0) {
				const double radius = std::max(1., std::round(inputs.Feather));
				if (radius > std::numeric_limits<int>::max())
					return context.Fail(
						Status::LimitExceeded, "Polar mask feather exceeds supported radius", "mask_feather"
					);
				rowWork += static_cast<long double>(inputs.Mask->Width) * inputs.Mask->Height *
							   (2 * (2 * radius - 1) * 16 + 2) +
						   radius * 16;
			}
			if (rowWork > POLAR_WARP_WORK_LIMIT - work)
				return context.Fail(
					Status::LimitExceeded, "Polar complete batch exceeds work limit", "surface_out"
				);
			work += uint64_t(rowWork);
			if (inputs.Inactive) return true;
			for (uint32_t y = 0; y < inputs.Source->Height; ++y)
				for (uint32_t x = 0; x < inputs.Source->Width; ++x) {
					Vector2 coordinate;
					bool clipped;
					if (!PolarWarpCoordinate(context, inputs, x, y, coordinate, clipped)) return false;
				}
			return true;
		}
		bool DrawSourcePolar(NodeContext &context) {
			ENGINE_PROFILE("imagegraph.source.polar");
			PolarWarpInputs inputs;
			uint64_t work = 0;
			if (!PreparePolarWarp(context, inputs) || !QuotePolarWarp(context, inputs, work)) return false;
			bool failed = false;
			if (CopyWhenInactive(context, failed)) return !failed;
			const auto format = ResolveProcessorSurfaceFormat(context, inputs.Source);
			if (!format) return false;
			auto *output =
				context.NewImage("surface_out", inputs.Source->Width, inputs.Source->Height, *format);
			if (!output) return false;
			const Vector2 dimension{double(inputs.Source->Width), double(inputs.Source->Height)};
			for (uint32_t y = 0; y < output->Height; ++y)
				for (uint32_t x = 0; x < output->Width; ++x) {
					Vector2 coordinate;
					bool clipped;
					if (!PolarWarpCoordinate(context, inputs, x, y, coordinate, clipped)) return false;
					const auto colour = clipped ? Rgba{}
												: PolarWarpTexture(
													  *inputs.Source,
													  float(coordinate.X),
													  float(coordinate.Y),
													  inputs.Sampler,
													  dimension
												  );
					for (double channel : colour)
						if (!std::isfinite(channel))
							return context.Fail(
								Status::InvalidValue, "Polar sample is nonfinite", "surface_out"
							);
					if (!WritePixel(*output, x, y, colour))
						return context.Fail(
							Status::InvalidValue, "Polar output exceeds numeric storage range", "surface_out"
						);
				}
			FinishProcessor(context, *inputs.Source, *output);
			return context.FailureCode == Status::Ok;
		}
	}
	bool AdmitSourcePolar(NodeContext &context, uint64_t &batchWork) {
		PolarWarpInputs inputs;
		return PreparePolarWarp(context, inputs) && QuotePolarWarp(context, inputs, batchWork);
	}
	std::span<const ExecutorEntry> SourcePolarExecutors() {
		static constexpr ExecutorEntry entries[]{{"pc.polar", DrawSourcePolar, true}};
		return entries;
	}
}
