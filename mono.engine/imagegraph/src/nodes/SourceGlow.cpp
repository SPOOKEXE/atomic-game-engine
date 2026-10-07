#include "SourceGlow.hpp"

#include "../AtlasPayload.hpp"
#include "Families.hpp"
#include "Sampler.hpp"
#include "SourceNormalShaderCurve.hpp"

namespace engine::imagegraph::detail {
	namespace {
		constexpr uint64_t GLOW_WORK_LIMIT = 64000000;
		struct GlowMapped {
			const Image *Map = nullptr;
			float Value = 0, Low = 0, High = 0;
		};
		struct GlowInputs {
			const Image *Source = nullptr, *Texture = nullptr, *Mask = nullptr;
			const Curve *Falloff = nullptr;
			GlowMapped Size, Strength;
			Rgba Colour{};
			int64_t Mode = 0, Side = 0, Blend = 3;
			float BlendColour = 0;
			double Feather = 0;
			bool Original = true, PixelDistance = true, Inactive = false;
		};
		bool GlowSurface(NodeContext &context, std::string_view port, const Image *surface) {
			if (!surface) return true;
			if (!ValidSurfaceLayout(*surface, Limits::MaximumDimension, Limits::MaximumEvaluationBytes))
				return context.Fail(Status::InvalidValue, "Glow surface layout is invalid", port);
			const auto *value = context.Find(port);
			if (value && std::holds_alternative<AtlasValue>(*value))
				return context.Fail(
					Status::UnsupportedExecution, "Glow raw texture binding rejects Atlas", port
				);
			return true;
		}
		bool GlowNumber(NodeContext &context, std::string_view port, double value, float &converted) {
			converted = float(value);
			return std::isfinite(converted) ||
				   context.Fail(Status::InvalidValue, "Glow control exceeds finite shader range", port);
		}
		bool PrepareGlowMapped(NodeContext &context, std::string_view port, GlowMapped &mapped) {
			const std::string name(port);
			if (!GlowNumber(context, port, context.Scalar(port), mapped.Value)) return false;
			if (!context.Boolean(name + "_mapped")) return context.FailureCode == Status::Ok;
			mapped.Map = context.Input(name + "_map");
			if (!mapped.Map) return context.FailureCode == Status::Ok;
			if (!GlowSurface(context, name + "_map", mapped.Map)) return false;
			const auto range = context.Vec2(name + "_map_range");
			return GlowNumber(context, name + "_map_range", range.X, mapped.Low) &&
				   GlowNumber(context, name + "_map_range", range.Y, mapped.High);
		}
		bool PrepareGlow(NodeContext &context, GlowInputs &inputs) {
			inputs.Source = context.Input("surface_in");
			if (!inputs.Source)
				return context.Fail(Status::InvalidValue, "Glow requires Surface In", "surface_in");
			inputs.Inactive = !context.Boolean("active", true);
			if (inputs.Inactive) return context.FailureCode == Status::Ok;
			if (!ValidSurfaceLayout(*inputs.Source, Limits::MaximumDimension, Limits::MaximumEvaluationBytes))
				return context.Fail(Status::InvalidValue, "Glow surface layout is invalid", "surface_in");
			if (const auto *value = context.Find("surface_in"))
				if (const auto *atlas = std::get_if<AtlasValue>(value); atlas && !ValidAtlasPayload(*atlas))
					return context.Fail(Status::InvalidValue, "Glow Atlas is malformed", "surface_in");
			inputs.Mode = context.Integer("mode");
			inputs.Side = context.Integer("side");
			inputs.Blend = context.Integer("blend_mode", 3);
			if (inputs.Mode < 0 || inputs.Mode > 1)
				return context.Fail(Status::UnsupportedExecution, "Glow mode is undefined", "mode");
			if (inputs.Side < 0 || inputs.Side > 1)
				return context.Fail(Status::UnsupportedExecution, "Glow side is undefined", "side");
			if (inputs.Blend != 0 && inputs.Blend != 1 && inputs.Blend != 3 && inputs.Blend != 4 &&
				inputs.Blend != 6 && inputs.Blend != 7)
				return context.Fail(
					Status::UnsupportedExecution,
					"Glow blend mode is a source separator or undefined",
					"blend_mode"
				);
			inputs.Original = context.Boolean("draw_original", true);
			inputs.PixelDistance = context.Boolean("pixel_distance", true);
			if (!GlowNumber(context, "blend_color", context.Scalar("blend_color"), inputs.BlendColour) ||
				!PrepareGlowMapped(context, "size", inputs.Size) ||
				!PrepareGlowMapped(context, "strength", inputs.Strength))
				return false;
			const auto colour = context.Get<Colour>("color", {255, 255, 255, 255});
			inputs.Colour = {
				colour.Red / 255.f, colour.Green / 255.f, colour.Blue / 255.f, colour.Alpha / 255.f
			};
			inputs.Texture = context.Input("texture");
			inputs.Mask = context.Input("mask");
			if (!GlowSurface(context, "texture", inputs.Texture) ||
				!GlowSurface(context, "mask", inputs.Mask))
				return false;
			inputs.Feather = context.Scalar("mask_feather");
			if (!std::isfinite(inputs.Feather))
				return context.Fail(Status::InvalidValue, "Glow mask feather is nonfinite", "mask_feather");
			if (context.Boolean("strength_curved")) {
				const auto *value = context.Find("strength_curve");
				inputs.Falloff = value ? std::get_if<Curve>(value) : nullptr;
				if (!inputs.Falloff || inputs.Falloff->Anchors.size() < 2 ||
					inputs.Falloff->Anchors.size() > 9 || float(inputs.Falloff->Header[1]) == 0)
					return context.Fail(
						Status::UnsupportedExecution,
						"Glow falloff curve exceeds the GLSL curve layout",
						"strength_curve"
					);
				for (double number : inputs.Falloff->Header)
					if (!std::isfinite(float(number)))
						return context.Fail(
							Status::InvalidValue,
							"Glow curve header exceeds finite shader range",
							"strength_curve"
						);
				for (const auto &anchor : inputs.Falloff->Anchors)
					for (double number : anchor)
						if (!std::isfinite(float(number)))
							return context.Fail(
								Status::InvalidValue,
								"Glow curve anchor exceeds finite shader range",
								"strength_curve"
							);
			}
			// grug source uploads Border but the pinned shader never reads it.
			return context.FailureCode == Status::Ok;
		}
		float GlowMappedValue(const GlowMapped &mapped, float u, float v) {
			if (!mapped.Map) return mapped.Value;
			const auto texel = SampleNearest(*mapped.Map, u, v);
			const float amount = (float(texel[0]) + float(texel[1]) + float(texel[2])) / 3.f;
			return mapped.Low + (mapped.High - mapped.Low) * amount;
		}
		bool QuoteGlow(NodeContext &context, const GlowInputs &inputs, uint64_t &work) {
			const uint64_t pixels = uint64_t(inputs.Source->Width) * inputs.Source->Height;
			const auto spend = [&](long double count, std::string_view port) {
				if (!std::isfinite(count) || count > GLOW_WORK_LIMIT - work)
					return context.Fail(
						Status::LimitExceeded, "Glow complete batch exceeds work limit", port
					);
				work += uint64_t(std::ceil(count));
				return true;
			};
			if (!spend(pixels * (inputs.Inactive ? 1 : 256), "surface_out")) return false;
			if (inputs.Inactive) return true;
			for (uint32_t y = 0; y < inputs.Source->Height; ++y)
				for (uint32_t x = 0; x < inputs.Source->Width; ++x) {
					const float u = (float(x) + .5f) / inputs.Source->Width,
								v = (float(y) + .5f) / inputs.Source->Height;
					const float size = GlowMappedValue(inputs.Size, u, v),
								strength = GlowMappedValue(inputs.Strength, u, v);
					if (!std::isfinite(size))
						return context.Fail(Status::InvalidValue, "Glow mapped size is nonfinite", "size");
					if (!std::isfinite(strength))
						return context.Fail(
							Status::InvalidValue, "Glow mapped strength is nonfinite", "strength"
						);
					const long double rings = std::max(0.L, std::ceil(static_cast<long double>(size)) - 1);
					const long double angles =
						std::floor(std::max(64.L, static_cast<long double>(size) * 4)) + 1;
					if (!spend(
							rings * angles * 16 + (inputs.Falloff ? inputs.Falloff->Anchors.size() * 128 : 0),
							"size"
						))
						return false;
				}
			if (inputs.Mask && inputs.Feather > 0) {
				const double radius = std::max(1., std::round(inputs.Feather));
				if (radius > std::numeric_limits<int>::max())
					return context.Fail(
						Status::LimitExceeded, "Glow mask feather exceeds supported radius", "mask_feather"
					);
				return spend(
					static_cast<long double>(inputs.Mask->Width) * inputs.Mask->Height *
							(2 * (2 * radius - 1) * 16 + 2) +
						radius * 16,
					"mask_feather"
				);
			}
			return true;
		}
		float GlowBrightness(const Rgba &colour) {
			return (float(colour[0]) * .2126f + float(colour[1]) * .7152f + float(colour[2]) * .0722f) *
				   float(colour[3]);
		}
		bool ShadeGlow(NodeContext &context, const GlowInputs &inputs, uint32_t x, uint32_t y, Rgba &output) {
			const auto base = ReadPixel(*inputs.Source, x, y);
			for (double channel : base)
				if (!std::isfinite(float(channel)))
					return context.Fail(
						Status::InvalidValue, "Glow source exceeds finite shader range", "surface_in"
					);
			output = inputs.Original ? base : inputs.Mode == 0 ? Rgba{0, 0, 0, 1} : Rgba{};
			if ((inputs.Side == 0 &&
				 (inputs.Mode == 0 ? base[0] == 1 && base[1] == 1 && base[2] == 1 : base[3] == 1)) ||
				(inputs.Side == 1 &&
				 (inputs.Mode == 0 ? base[0] == 0 && base[1] == 0 && base[2] == 0 : base[3] == 0)))
				return true;
			const float u = (float(x) + .5f) / inputs.Source->Width,
						v = (float(y) + .5f) / inputs.Source->Height;
			const float size = GlowMappedValue(inputs.Size, u, v);
			const float angles = std::max(64.f, size * 4);
			const float texelX = 1.f / inputs.Source->Width, texelY = 1.f / inputs.Source->Height;
			float distance = 0, hitU = u, hitV = v;
			for (float radius = 1; radius < size && distance == 0; radius += 1)
				for (float step = 0; step <= angles; step += 1) {
					const float angle = step / angles * 6.283185307179586f;
					const float sampleU = u + std::cos(angle) * radius * texelX;
					const float sampleV = v + std::sin(angle) * radius * texelY;
					const auto sample = SampleNearest(*inputs.Source, sampleU, sampleV);
					const float sampled = inputs.Mode == 0 ? GlowBrightness(sample) : float(sample[3]);
					const float original = inputs.Mode == 0 ? GlowBrightness(base) : float(base[3]);
					if ((inputs.Side == 0 && sampled > original) ||
						(inputs.Side == 1 && sampled < original)) {
						distance = inputs.PixelDistance
									   ? radius
									   : std::hypot(
											 float(x) - std::floor(sampleU * inputs.Source->Width),
											 float(y) - std::floor(sampleV * inputs.Source->Height)
										 );
						hitU = sampleU;
						hitV = sampleV;
						break;
					}
				}
			if (distance <= 0) return true;
			Rgba colour = inputs.Colour;
			if (inputs.Texture) {
				const auto texture = SampleNearest(*inputs.Texture, u, v);
				for (size_t c = 0; c < 4; ++c)
					colour[c] = float(colour[c]) * float(texture[c]);
			}
			if (inputs.BlendColour > 0) {
				const auto sample = SampleNearest(*inputs.Source, hitU, hitV);
				for (size_t c = 0; c < 4; ++c)
					colour[c] = float(colour[c]) +
								(float(colour[c]) * float(sample[c]) - float(colour[c])) * inputs.BlendColour;
			}
			float strength = 1 - distance / size;
			if (inputs.Falloff) strength = NormalEvalShaderCurve(*inputs.Falloff, strength);
			strength *= GlowMappedValue(inputs.Strength, u, v);
			Rgba from = inputs.Original ? base : Rgba{}, target = base;
			for (size_t c = 0; c < 4; ++c) {
				const float b = float(base[c]), col = float(colour[c]);
				switch (inputs.Blend) {
				case 0:
				case 1:
					target[c] = col;
					break;
				case 3:
					target[c] = b + col;
					break;
				case 4:
					target[c] = 1 - (1 - b) * (1 - col);
					break;
				case 6:
					target[c] = b - col * strength;
					break;
				case 7:
					target[c] = b * col;
					break;
				}
			}
			if (inputs.Blend == 1) strength = std::clamp(strength, 0.f, 1.f);
			if (inputs.Mode == 0)
				from[3] = target[3] = base[3];
			else if (inputs.Side == 0) {
				from = {colour[0], colour[1], colour[2], 0};
				target = {colour[0], colour[1], colour[2], 1};
			}
			for (size_t c = 0; c < 4; ++c) {
				const float result = float(from[c]) + (float(target[c]) - float(from[c])) * strength;
				if (!std::isfinite(result))
					return context.Fail(
						Status::InvalidValue, "Glow result exceeds finite shader range", "surface_out"
					);
				output[c] = result;
			}
			return true;
		}
		bool DrawGlow(NodeContext &context) {
			ENGINE_PROFILE("imagegraph.source.glow");
			GlowInputs inputs;
			uint64_t work = 0;
			if (!PrepareGlow(context, inputs) || !QuoteGlow(context, inputs, work)) return false;
			bool failed = false;
			if (CopyWhenInactive(context, failed)) return !failed;
			const auto format = ResolveProcessorSurfaceFormat(context, inputs.Source);
			if (!format) return false;
			auto *output =
				context.NewImage("surface_out", inputs.Source->Width, inputs.Source->Height, *format);
			if (!output) return false;
			for (uint32_t y = 0; y < output->Height; ++y)
				for (uint32_t x = 0; x < output->Width; ++x) {
					Rgba colour;
					if (!ShadeGlow(context, inputs, x, y, colour)) return false;
					if (!WritePixel(*output, x, y, colour))
						return context.Fail(
							Status::InvalidValue, "Glow output exceeds numeric storage range", "surface_out"
						);
				}
			FinishProcessor(context, *inputs.Source, *output);
			return context.FailureCode == Status::Ok;
		}
	}
	bool AdmitSourceGlow(NodeContext &context, uint64_t &batchWork) {
		GlowInputs inputs;
		return PrepareGlow(context, inputs) && QuoteGlow(context, inputs, batchWork);
	}
	std::span<const ExecutorEntry> SourceGlowExecutors() {
		static constexpr ExecutorEntry entries[]{{"pc.glow", DrawGlow, true}};
		return entries;
	}
}
