#include "SourceDisplace.hpp"

#include "../AtlasPayload.hpp"
#include "../SourceMappedInputs.hpp"
#include "Families.hpp"
#include "Sampler.hpp"
#include "SourceNormalShaderCurve.hpp"

namespace engine::imagegraph::detail {
	namespace {
		constexpr uint64_t DISPLACE_WORK_LIMIT = 64000000;
		constexpr float DISPLACE_PI = 3.14159265358979323846f;
		struct DisplaceMapped {
			const Image *Map = nullptr;
			float Low = 0, High = 0;
		};
		struct DisplaceInputs {
			const Image *Source = nullptr, *Map = nullptr, *Map2 = nullptr, *UV = nullptr, *Mask = nullptr;
			const Curve *Falloff = nullptr;
			DisplaceMapped Strength, Middle;
			Vector2 Position, Center;
			SamplerSettings Sampler;
			float UVBlend = 1, Angle = 0, Ratio = .5f;
			double Feather = 0;
			int64_t Mode = 0, Blend = 0, Steps = 0, Repeats = 1;
			bool Separate = false, Iterate = false, Reposition = false, Fade = false, Stop = false;
			bool Inactive = false, Copy = false;
		};
		bool DisplaceFloat(NodeContext &context, std::string_view port, double value, float &out) {
			out = float(value);
			return std::isfinite(out) ||
				   context.Fail(Status::InvalidValue, "Displace shader control is nonfinite", port);
		}
		bool DisplaceSurface(NodeContext &context, std::string_view port, const Image *image, bool raw) {
			if (!image) return true;
			if (!ValidSurfaceLayout(*image, Limits::MaximumDimension, Limits::MaximumEvaluationBytes))
				return context.Fail(Status::InvalidValue, "Displace surface layout is invalid", port);
			if (const auto *value = context.Find(port)) {
				if (const auto *atlas = std::get_if<AtlasValue>(value)) {
					if (!ValidAtlasPayload(*atlas))
						return context.Fail(Status::InvalidValue, "Displace Atlas is malformed", port);
					if (raw)
						return context.Fail(
							Status::UnsupportedExecution, "Displace raw map binding rejects Atlas", port
						);
				}
			}
			return true;
		}
		bool DisplaceMappedInput(NodeContext &context, std::string_view port, DisplaceMapped &mapped) {
			if (!SourceRangeMapped(context, port)) {
				if (!DisplaceFloat(context, port, context.Scalar(port), mapped.Low)) return false;
				mapped.High = mapped.Low;
				return context.FailureCode == Status::Ok;
			}
			Vector2 range;
			if (!ReadSourceMappedRange(context, port, range) ||
				!DisplaceFloat(context, port, range.X, mapped.Low) ||
				!DisplaceFloat(context, port, range.Y, mapped.High))
				return false;
			mapped.Map = context.Input(std::string(port) + "_map");
			return DisplaceSurface(context, std::string(port) + "_map", mapped.Map, true);
		}
		bool DisplaceUnit(
			NodeContext &context, const DisplaceInputs &inputs, std::string_view port, Vector2 &out
		) {
			out = context.Vec2(port);
			const auto unit = context.Integer(std::string(port) + "_unit", 1);
			if (unit < 0 || unit > 1)
				return context.Fail(
					Status::UnsupportedExecution, "Displace reference unit is undefined", port
				);
			if (unit == 1 && !context.IsLinked(port)) {
				const auto reference = context.DisplaceReferenceDimension.value_or(
					Vector2{double(inputs.Source->Width), double(inputs.Source->Height)}
				);
				out.X *= reference.X;
				out.Y *= reference.Y;
			}
			float x, y;
			if (!DisplaceFloat(context, port, out.X, x) || !DisplaceFloat(context, port, out.Y, y))
				return false;
			out = {x, y};
			return true;
		}
		bool PrepareDisplace(NodeContext &context, DisplaceInputs &inputs) {
			inputs.Source = context.Input("surface_in");
			if (!inputs.Source)
				return context.Fail(Status::InvalidValue, "Displace requires Surface In", "surface_in");
			if (!DisplaceSurface(context, "surface_in", inputs.Source, false)) return false;
			inputs.Inactive = !context.Boolean("active", true);
			if (inputs.Inactive) return context.FailureCode == Status::Ok;
			inputs.Map = context.Input("displace_map");
			inputs.Mode = context.Integer("mode");
			inputs.Separate = context.Boolean("separate_axis");
			inputs.Map2 = context.Input("displace_map_2");
			inputs.Copy =
				!inputs.Map || ((inputs.Mode == 1 || inputs.Mode == 2) && inputs.Separate && !inputs.Map2);
			if (!ResolveProcessorSurfaceFormat(context, inputs.Source)) return false;
			if (inputs.Copy) return context.FailureCode == Status::Ok;
			if (inputs.Mode < 0 || inputs.Mode > 6 || inputs.Mode == 4)
				return context.Fail(
					Status::UnsupportedExecution, "Displace mode is a source separator or undefined", "mode"
				);
			if (!DisplaceSurface(context, "displace_map", inputs.Map, true)) return false;
			if (inputs.Separate && (inputs.Mode == 1 || inputs.Mode == 2) &&
				!DisplaceSurface(context, "displace_map_2", inputs.Map2, true))
				return false;
			inputs.UV = context.Input("uv_map");
			inputs.Mask = context.Input("mask");
			if (!DisplaceSurface(context, "uv_map", inputs.UV, true) ||
				!DisplaceSurface(context, "mask", inputs.Mask, true) ||
				!DisplaceMappedInput(context, "strength", inputs.Strength) ||
				!DisplaceMappedInput(context, "mid_value", inputs.Middle) ||
				!DisplaceUnit(context, inputs, "position", inputs.Position) ||
				!DisplaceUnit(context, inputs, "mid_point", inputs.Center) ||
				!DisplaceFloat(context, "uv_mix", context.Scalar("uv_mix", 1), inputs.UVBlend) ||
				!DisplaceFloat(context, "angle_offset", context.Scalar("angle_offset"), inputs.Angle) ||
				!DisplaceFloat(context, "mix_ratio", context.Scalar("mix_ratio", .5), inputs.Ratio))
				return false;
			inputs.Sampler = ReadSampler(context);
			inputs.Blend = context.Integer("blend_mode");
			if (inputs.Blend < 0 || inputs.Blend > 3)
				return context.Fail(
					Status::UnsupportedExecution, "Displace blend mode is undefined", "blend_mode"
				);
			inputs.Iterate = context.Boolean("iterate");
			inputs.Reposition = context.Boolean("reposition");
			inputs.Fade = context.Boolean("fade_distance");
			inputs.Stop = context.Boolean("stop_empty");
			inputs.Repeats = std::max<int64_t>(1, context.Integer("repeat", 1));
			const float iteration = float(context.Integer("iteration", 16));
			// grug source float loop can stop advancing past 2^24; quote rejects that before looping.
			if (inputs.Iterate && iteration > 0) {
				if (iteration >= float(DISPLACE_WORK_LIMIT))
					return context.Fail(
						Status::LimitExceeded, "Displace iteration exceeds work limit", "iteration"
					);
				inputs.Steps = int64_t(iteration);
			}
			inputs.Feather = context.Scalar("mask_feather");
			if (!std::isfinite(inputs.Feather))
				return context.Fail(Status::InvalidValue, "Displace feather is nonfinite", "mask_feather");
			if (context.Boolean("strength_curved")) {
				const auto *value = context.Find("strength_curve");
				inputs.Falloff = value ? std::get_if<Curve>(value) : nullptr;
				if (!inputs.Falloff || inputs.Falloff->Anchors.size() < 2 ||
					inputs.Falloff->Anchors.size() > 9 || float(inputs.Falloff->Header[1]) == 0)
					return context.Fail(
						Status::UnsupportedExecution, "Displace curve exceeds GLSL layout", "strength_curve"
					);
				for (double number : inputs.Falloff->Header)
					if (!std::isfinite(float(number)))
						return context.Fail(
							Status::InvalidValue, "Displace curve header is nonfinite", "strength_curve"
						);
				for (const auto &anchor : inputs.Falloff->Anchors)
					for (double number : anchor)
						if (!std::isfinite(float(number)))
							return context.Fail(
								Status::InvalidValue, "Displace curve anchor is nonfinite", "strength_curve"
							);
			}
			// grug source Oversample Mode input is unused; Oversample attribute drives the sampler.
			return context.FailureCode == Status::Ok;
		}
		Rgba DisplaceRaw(const DisplaceInputs &inputs, const Image &image, float u, float v) {
			auto settings = inputs.Sampler;
			settings.ForceNearest = true;
			return TextureInterpolated(
				image, u, v, settings, {double(inputs.Source->Width), double(inputs.Source->Height)}
			);
		}
		float DisplaceMean(const Rgba &colour) {
			return (float(colour[0]) + float(colour[1]) + float(colour[2])) / 3.f;
		}
		float DisplaceBright(const Rgba &colour) {
			return (float(colour[0]) * .2126f + float(colour[1]) * .7152f + float(colour[2]) * .0722f) *
				   float(colour[3]);
		}
		float DisplaceMappedValue(
			const DisplaceInputs &inputs, const DisplaceMapped &mapped, float u, float v, bool plain
		) {
			if (!mapped.Map) return mapped.Low;
			const auto colour =
				plain ? SampleNearest(*mapped.Map, u, v) : DisplaceRaw(inputs, *mapped.Map, u, v);
			return mapped.Low + (mapped.High - mapped.Low) * DisplaceMean(colour);
		}
		bool DisplaceShift(
			NodeContext &context,
			const DisplaceInputs &inputs,
			Vector2 point,
			float strength,
			float middle,
			Vector2 &out
		) {
			float u = float(point.X), v = float(point.Y);
			if (inputs.UV) {
				const auto map = SampleNearest(*inputs.UV, u, v);
				u += (float(map[0]) - u) * inputs.UVBlend;
				v += (1.f - float(map[1]) - v) * inputs.UVBlend;
			}
			if (!std::isfinite(u) || !std::isfinite(v))
				return context.Fail(Status::InvalidValue, "Displace UV coordinate is nonfinite", "uv_map");
			const auto map = DisplaceRaw(inputs, *inputs.Map, u, v);
			const float tx = 1.f / inputs.Source->Width, ty = 1.f / inputs.Source->Height;
			const auto curved = [&](float amount) {
				return inputs.Falloff ? NormalEvalShaderCurve(*inputs.Falloff, amount) : amount;
			};
			const float amount = curved(DisplaceBright(map) - middle);
			float x = u, y = v;
			switch (inputs.Mode) {
			case 0:
				x += amount * strength * (float(inputs.Position.X) * tx);
				y += amount * strength * (float(inputs.Position.Y) * ty);
				break;
			case 1:
				if (inputs.Separate) {
					x += amount * strength;
					y += curved(DisplaceBright(DisplaceRaw(inputs, *inputs.Map2, u, v)) - middle) * strength;
				} else {
					x += (float(map[0]) - middle) * strength;
					y += (float(map[1]) - middle) * strength;
				}
				break;
			case 2: {
				const float angle = (inputs.Separate ? DisplaceBright(map) : float(map[0])) * DISPLACE_PI * 2;
				const float distance =
					curved(
						(inputs.Separate ? DisplaceBright(DisplaceRaw(inputs, *inputs.Map2, u, v))
										 : float(map[1])) -
						middle
					) *
					strength;
				x += distance * std::cos(angle);
				y += distance * std::sin(angle);
				break;
			}
			case 3: {
				const float gx = DisplaceMean(DisplaceRaw(inputs, *inputs.Map, u + tx, v)) -
								 DisplaceMean(DisplaceRaw(inputs, *inputs.Map, u - tx, v));
				const float gy = DisplaceMean(DisplaceRaw(inputs, *inputs.Map, u, v + ty)) -
								 DisplaceMean(DisplaceRaw(inputs, *inputs.Map, u, v - ty));
				const float angle = inputs.Angle * (DISPLACE_PI / 180.f), c = std::cos(angle),
							s = std::sin(angle);
				x += (gx * c - gy * s - middle) * strength;
				y += (gx * s + gy * c - middle) * strength;
				break;
			}
			case 5: {
				const float angle = DISPLACE_PI * amount * strength, c = std::cos(angle), s = std::sin(angle);
				const float cx = float(inputs.Center.X) / inputs.Source->Width,
							cy = float(inputs.Center.Y) / inputs.Source->Height;
				x = (u - cx) * c - (v - cy) * s + cx;
				y = (u - cx) * s + (v - cy) * c + cy;
				break;
			}
			case 6: {
				const float scale = amount * strength;
				const float cx = float(inputs.Center.X) / inputs.Source->Width,
							cy = float(inputs.Center.Y) / inputs.Source->Height;
				x = (u - cx) * scale + cx;
				y = (v - cy) * scale + cy;
				break;
			}
			}
			out = {x, y};
			return (std::isfinite(x) && std::isfinite(y) && std::isfinite(strength) &&
					std::isfinite(middle)) ||
				   context.Fail(Status::InvalidValue, "Displace derived coordinate is nonfinite", "strength");
		}
		Rgba DisplaceBlend(const DisplaceInputs &inputs, const Rgba &from, const Rgba &to) {
			if (inputs.Blend == 1) return DisplaceBright(from) < DisplaceBright(to) ? from : to;
			if (inputs.Blend == 2) return DisplaceBright(from) > DisplaceBright(to) ? from : to;
			if (inputs.Blend != 3) return to;
			Rgba result;
			for (size_t channel = 0; channel < 4; ++channel)
				result[channel] =
					float(from[channel]) + (float(to[channel]) - float(from[channel])) * inputs.Ratio;
			return result;
		}
		bool QuoteDisplace(NodeContext &context, const DisplaceInputs &inputs, uint64_t &work) {
			const uint64_t pixels = uint64_t(inputs.Source->Width) * inputs.Source->Height;
			const uint64_t taps = inputs.Sampler.Interpolation == 4	  ? 16
								  : inputs.Sampler.Interpolation == 3 ? 8
								  : inputs.Sampler.Interpolation == 1 || inputs.Sampler.Interpolation == 6
									  ? 1
									  : 4;
			long double rowWork = pixels;
			if (!inputs.Inactive && !inputs.Copy) {
				rowWork +=
					static_cast<long double>(pixels) * inputs.Repeats *
					((128 * taps + (inputs.Falloff ? 2048 : 0)) * (inputs.Iterate ? inputs.Steps + 1 : 2));
				if (inputs.Mask && inputs.Feather > 0) {
					const double radius = std::max(1., std::round(inputs.Feather));
					if (radius > std::numeric_limits<int>::max())
						return context.Fail(
							Status::LimitExceeded, "Displace mask radius exceeds limits", "mask_feather"
						);
					rowWork += static_cast<long double>(inputs.Mask->Width) * inputs.Mask->Height *
								   (2 * (2 * radius - 1) * 16 + 2) +
							   radius * 16;
				}
			}
			if (work > DISPLACE_WORK_LIMIT || rowWork > DISPLACE_WORK_LIMIT - work)
				return context.Fail(
					Status::LimitExceeded, "Displace complete batch exceeds work limit", "surface_out"
				);
			work += uint64_t(std::ceil(rowWork));
			if (inputs.Inactive || inputs.Copy) return true;
			// grug check the full coordinate path before any output, independent of later empty-pixel stops.
			for (uint32_t y = 0; y < inputs.Source->Height; ++y)
				for (uint32_t x = 0; x < inputs.Source->Width; ++x) {
					const Vector2 start{
						(float(x) + .5f) / inputs.Source->Width, (float(y) + .5f) / inputs.Source->Height
					};
					const float strength =
						DisplaceMappedValue(inputs, inputs.Strength, float(start.X), float(start.Y), false);
					const float middle =
						DisplaceMappedValue(inputs, inputs.Middle, float(start.X), float(start.Y), true);
					if (!std::isfinite(strength) || !std::isfinite(middle))
						return context.Fail(
							Status::InvalidValue,
							"Displace mapped value is nonfinite",
							!std::isfinite(strength) ? "strength" : "mid_value"
						);
					Vector2 position = start;
					const float inverse = inputs.Steps ? 1.f / float(inputs.Steps) : 0;
					for (int64_t i = 0, count = inputs.Iterate ? inputs.Steps : 1; i < count; ++i)
						if (!DisplaceShift(
								context,
								inputs,
								inputs.Iterate && inputs.Reposition ? position : start,
								inputs.Iterate ? strength * (float(i) + 1) * inverse : strength,
								middle,
								position
							))
							return false;
				}
			return true;
		}
		bool DisplaceShade(
			NodeContext &context,
			const DisplaceInputs &inputs,
			const Image &source,
			uint32_t x,
			uint32_t y,
			Rgba &result
		) {
			const Vector2 start{(float(x) + .5f) / source.Width, (float(y) + .5f) / source.Height};
			const auto original = SampleTexture(source, start.X, start.Y, inputs.Sampler);
			auto colour = original;
			if (inputs.Iterate && inputs.Stop && original[3] == 0) {
				result = {};
				return true;
			}
			const float strength =
				DisplaceMappedValue(inputs, inputs.Strength, float(start.X), float(start.Y), false);
			const float middle =
				DisplaceMappedValue(inputs, inputs.Middle, float(start.X), float(start.Y), true);
			Vector2 position = start;
			const float inverse = inputs.Steps ? 1.f / float(inputs.Steps) : 0;
			for (int64_t i = 0, count = inputs.Iterate ? inputs.Steps : 1; i < count; ++i) {
				if (!DisplaceShift(
						context,
						inputs,
						inputs.Iterate && inputs.Reposition ? position : start,
						inputs.Iterate ? strength * (float(i) + 1) * inverse : strength,
						middle,
						position
					))
					return false;
				auto sampled = SampleTexture(source, position.X, position.Y, inputs.Sampler);
				if (inputs.Iterate && inputs.Stop && sampled[3] == 0) break;
				if (inputs.Iterate && inputs.Fade)
					for (size_t channel = 0; channel < 3; ++channel)
						sampled[channel] = float(sampled[channel]) * (1.f - float(i) * inverse);
				colour = inputs.Iterate ? DisplaceBlend(inputs, colour, sampled) : sampled;
			}
			result = DisplaceBlend(inputs, original, colour);
			return true;
		}
		bool DrawSourceDisplace(NodeContext &context) {
			ENGINE_PROFILE("imagegraph.source.displace");
			DisplaceInputs inputs;
			uint64_t work = 0;
			if (!PrepareDisplace(context, inputs) || !QuoteDisplace(context, inputs, work)) return false;
			bool failed = false;
			if (CopyWhenInactive(context, failed)) return !failed;
			const auto format = ResolveProcessorSurfaceFormat(context, inputs.Source);
			if (!format) return false;
			const auto width = inputs.Source->Width, height = inputs.Source->Height;
			// grug scratch surfaces are RGBA8 regardless of requested output depth, as surface_verify
			// defaults.
			auto scratchCharge =
				context.ReserveWorkspace(inputs.Copy ? 0 : uint64_t(width) * height * 8, "surface_out");
			if (!scratchCharge) return false;
			Image first, second;
			if (!inputs.Copy) {
				first = {width, height, std::vector<uint8_t>(size_t(width) * height * 4), 0};
				second = {width, height, std::vector<uint8_t>(size_t(width) * height * 4), 0};
				for (uint32_t y = 0; y < height; ++y)
					for (uint32_t x = 0; x < width; ++x)
						if (!WritePixel(first, x, y, ReadPixel(*inputs.Source, x, y)))
							return context.Fail(
								Status::InvalidValue,
								"Displace initial scratch conversion failed",
								"surface_in"
							);
				for (int64_t pass = 0; pass < inputs.Repeats; ++pass) {
					for (uint32_t y = 0; y < height; ++y)
						for (uint32_t x = 0; x < width; ++x) {
							Rgba colour;
							if (!DisplaceShade(context, inputs, first, x, y, colour)) return false;
							for (double channel : colour)
								if (!std::isfinite(channel))
									return context.Fail(
										Status::InvalidValue,
										"Displace shader sample is nonfinite",
										"surface_out"
									);
							if (!WritePixel(second, x, y, colour))
								return context.Fail(
									Status::InvalidValue, "Displace scratch sample is invalid", "surface_out"
								);
						}
					std::swap(first, second);
				}
			}
			auto *output = context.NewImage("surface_out", width, height, *format);
			if (!output) return false;
			const auto &final = inputs.Copy ? *inputs.Source : first;
			for (uint32_t y = 0; y < height; ++y)
				for (uint32_t x = 0; x < width; ++x)
					if (!WritePixel(*output, x, y, ReadPixel(final, x, y)))
						return context.Fail(
							Status::InvalidValue, "Displace output conversion failed", "surface_out"
						);
			if (!inputs.Copy) FinishProcessor(context, *inputs.Source, *output);
			return context.FailureCode == Status::Ok;
		}
	}
	bool AdmitSourceDisplace(NodeContext &context, uint64_t &batchWork) {
		DisplaceInputs inputs;
		return PrepareDisplace(context, inputs) && QuoteDisplace(context, inputs, batchWork);
	}
	std::span<const ExecutorEntry> SourceDisplaceExecutors() {
		static constexpr ExecutorEntry entries[]{{"pc.displace", DrawSourceDisplace, true}};
		return entries;
	}
}
