#include "SourceRepeatTexture.hpp"

#include "Sampler.hpp"
#include "Source2DReferenceUnits.hpp"

#include <array>
#include <cmath>
#include <limits>

// Tiling algorithms
// Copyright © 2015 Inigo Quilez
// Native nearest CPU profile of pinned node_repeat_texture and sh_texture_repeat.
// Default normal blend entry state is assumed;
// this translation does not claim matched GPU arithmetic or licensed executable parity.

namespace engine::imagegraph::detail {
	namespace {
		constexpr uint64_t REPEAT_TEXTURE_WORK_LIMIT = 64000000;
		struct RepeatTextureInputs {
			const Image *Source = nullptr;
			uint32_t Width = 0, Height = 0;
			float ScaleX = 0, ScaleY = 0, Seed = 0, Randomness = 1;
			int64_t Type = 1;
			SurfaceFormat Format = SurfaceFormat::RGBA8Unorm;
		};

		bool RepeatFloat(NodeContext &c, double value, float &out, std::string_view port) {
			if (!std::isfinite(value) || std::abs(value) > std::numeric_limits<float>::max())
				return c.Fail(Status::InvalidValue, "Repeat Texture value exceeds finite shader range", port);
			out = float(value);
			return true;
		}

		bool PrepareRepeatTexture(NodeContext &c, RepeatTextureInputs &in) {
			if (const auto *value = c.Find("surface_in"); value && std::holds_alternative<AtlasValue>(*value))
				return c.Fail(
					Status::UnsupportedExecution,
					"Repeat Texture requires a physical source surface",
					"surface_in"
				);
			in.Source = c.Input("surface_in");
			if (!in.Source ||
				!ValidSurfaceLayout(*in.Source, Limits::MaximumDimension, Limits::MaximumEvaluationBytes))
				return c.Fail(Status::InvalidValue, "Repeat Texture source surface is invalid", "surface_in");
			const auto *dimension = c.Find("target_dimension");
			if (dimension && !std::holds_alternative<Vector2>(*dimension) &&
				!std::holds_alternative<double>(*dimension) && !std::holds_alternative<int64_t>(*dimension))
				return c.Fail(
					Status::UnsupportedExecution,
					"Repeat Texture dimension requires a numeric pair or scalar",
					"target_dimension"
				);
			Vector2 raw = c.Vec2("target_dimension", {1, 1});
			if (!c.IsLinked("target_dimension")) {
				const double unit = c.SourceChoice("target_dimension_unit", 1);
				if (c.FailureCode != Status::Ok) return false;
				if (unit != std::trunc(unit) || unit < 0 || unit > 2)
					return c.Fail(
						Status::UnsupportedExecution,
						"Repeat Texture dimension unit is undefined",
						"target_dimension_unit"
					);
				if (unit == 2)
					return c.Fail(
						Status::UnsupportedExecution,
						"Repeat Texture Dimension has no Mask getter",
						"target_dimension_unit"
					);
				if (unit == 1) {
					raw.X *= c.Project.SurfaceWidth;
					raw.Y *= c.Project.SurfaceHeight;
				}
			}
			float rawWidth, rawHeight;
			if (!RepeatFloat(c, raw.X, rawWidth, "target_dimension") ||
				!RepeatFloat(c, raw.Y, rawHeight, "target_dimension"))
				return false;
			const double width = std::max(1., source2d::GeneratorRoundHalfEven(raw.X)),
						 height = std::max(1., source2d::GeneratorRoundHalfEven(raw.Y));
			if (width > Limits::MaximumDimension || height > Limits::MaximumDimension)
				return c.Fail(
					Status::LimitExceeded,
					"Repeat Texture dimensions exceed native limits",
					"target_dimension"
				);
			if (c.Request.MaximumImageDimension == 0 ||
				c.Request.MaximumImageDimension > Limits::MaximumDimension)
				return c.Fail(
					Status::InvalidValue, "Repeat Texture request dimension limit is invalid", "surface_out"
				);
			if (width > c.Request.MaximumImageDimension || height > c.Request.MaximumImageDimension)
				return c.Fail(
					Status::LimitExceeded,
					"Repeat Texture dimensions exceed request limits",
					"target_dimension"
				);
			in.Width = uint32_t(width);
			in.Height = uint32_t(height);
			in.ScaleX = rawWidth / float(in.Source->Width);
			in.ScaleY = rawHeight / float(in.Source->Height);
			if (!RepeatFloat(c, c.Scalar("seed"), in.Seed, "seed") ||
				!RepeatFloat(c, c.Scalar("randomness", 1), in.Randomness, "randomness"))
				return false;
			const double type = c.SourceChoice("type", 1);
			if (c.FailureCode != Status::Ok) return false;
			if (!std::isfinite(type) || type != std::trunc(type) || type < 0 || type > 2)
				return c.Fail(Status::UnsupportedExecution, "Repeat Texture type is undefined", "type");
			in.Type = static_cast<int64_t>(type);
			if (in.Type != 0 && !c.Find("seed"))
				return c.Fail(
					Status::UnsupportedExecution,
					"Repeat Texture random modes require the observed source seed",
					"seed"
				);
			// attrDepth queries the original source's first array leaf, before processor selection.
			const Image *depthSource = in.Source;
			for (const auto &[port, images] : c.ImageArrays)
				if (port == "surface_in" && images) {
					depthSource = source2d::FirstReferenceImage(*images);
					if (!depthSource)
						return c.Fail(
							Status::InvalidValue,
							"Repeat Texture source has no first depth leaf",
							"surface_in"
						);
					break;
				}
			const auto format = ResolveProcessorSurfaceFormat(c, depthSource);
			if (!format || c.FailureCode != Status::Ok) return false;
			in.Format = *format;
			return true;
		}

		float RepeatFract(float v) {
			return v - std::floor(v);
		}
		float RepeatSign(float v) {
			return v > 0 ? 1.f : v < 0 ? -1.f : 0.f;
		}
		float RepeatMix(float a, float b, float t) {
			return a * (1.f - t) + b * t;
		}
		float RepeatSmooth(float v) {
			const float t = std::clamp((v - .25f) / .5f, 0.f, 1.f);
			return t * t * (3.f - 2.f * t);
		}
		using RepeatPixel = std::array<float, 4>;

		bool RepeatHash(NodeContext &c, const RepeatTextureInputs &in, float x, float y, RepeatPixel &out) {
			const RepeatPixel phase{
				1.f + in.Seed + (x * 37.f + y * 17.f),
				2.f + in.Seed + (x * 11.f + y * 47.f),
				3.f + in.Seed + (x * 41.f + y * 29.f),
				4.f + in.Seed + (x * 23.f + y * 31.f)
			};
			for (size_t channel = 0; channel < 4; ++channel) {
				if (!std::isfinite(phase[channel]))
					return c.Fail(Status::InvalidValue, "Repeat Texture hash phase is nonfinite", "seed");
				out[channel] = RepeatFract(std::sin(phase[channel]) * 103.f) * in.Randomness;
				if (!std::isfinite(out[channel]))
					return c.Fail(
						Status::InvalidValue, "Repeat Texture random offset is nonfinite", "randomness"
					);
			}
			return true;
		}

		bool RepeatSample(NodeContext &c, const RepeatTextureInputs &in, float u, float v, RepeatPixel &out) {
			if (!std::isfinite(u) || !std::isfinite(v))
				return c.Fail(
					Status::InvalidValue, "Repeat Texture sample coordinate is nonfinite", "randomness"
				);
			const auto sample = SampleNearest(*in.Source, RepeatFract(u), RepeatFract(v));
			for (size_t channel = 0; channel < 4; ++channel)
				if (!RepeatFloat(c, sample[channel], out[channel], "surface_in")) return false;
			return true;
		}

		bool
		RepeatTexturePixel(NodeContext &c, const RepeatTextureInputs &in, uint32_t x, uint32_t y, Rgba &out) {
			// draw_empty stretches to the allocated target, not to its unrounded dimension control.
			const float u = ((float(x) + .5f) / float(in.Width)) * in.ScaleX,
						v = ((float(y) + .5f) / float(in.Height)) * in.ScaleY;
			if (!std::isfinite(u) || !std::isfinite(v))
				return c.Fail(
					Status::InvalidValue, "Repeat Texture shader coordinate is nonfinite", "target_dimension"
				);
			RepeatPixel pixel{};
			if (in.Type == 0) {
				if (!RepeatSample(c, in, u, v, pixel)) return false;
			} else if (in.Type == 1) {
				std::array<RepeatPixel, 4> taps{};
				for (size_t tap = 0; tap < taps.size(); ++tap) {
					RepeatPixel offset;
					if (!RepeatHash(
							c, in, std::floor(u) + float(tap % 2), std::floor(v) + float(tap / 2), offset
						) ||
						!RepeatSample(
							c,
							in,
							u * RepeatSign(offset[2] - .5f) + offset[0],
							v * RepeatSign(offset[3] - .5f) + offset[1],
							taps[tap]
						))
						return false;
				}
				const float bx = RepeatSmooth(RepeatFract(u)), by = RepeatSmooth(RepeatFract(v));
				for (size_t channel = 0; channel < 4; ++channel)
					pixel[channel] = RepeatMix(
						RepeatMix(taps[0][channel], taps[1][channel], bx),
						RepeatMix(taps[2][channel], taps[3][channel], bx),
						by
					);
			} else {
				float weightSum = 0;
				for (int j = -1; j <= 1; ++j)
					for (int i = -1; i <= 1; ++i) {
						RepeatPixel offset, sample;
						if (!RepeatHash(c, in, std::floor(u) + float(i), std::floor(v) + float(j), offset))
							return false;
						const float rx = float(i) - RepeatFract(u) + offset[0],
									ry = float(j) - RepeatFract(v) + offset[1];
						const float distance = rx * rx + ry * ry;
						if (!std::isfinite(distance))
							return c.Fail(
								Status::InvalidValue,
								"Repeat Texture cell distance is nonfinite",
								"randomness"
							);
						const float weight = std::exp(-5.f * distance);
						if (!std::isfinite(-5.f * distance) || !std::isfinite(weight) ||
							!RepeatSample(c, in, u + 4.f * offset[2], v + 4.f * offset[3], sample))
							return c.FailureCode != Status::Ok
									   ? false
									   : c.Fail(
											 Status::InvalidValue,
											 "Repeat Texture cell weight is nonfinite",
											 "randomness"
										 );
						for (size_t channel = 0; channel < 3; ++channel)
							pixel[channel] += weight * sample[channel];
						weightSum += weight;
					}
				if (!std::isfinite(weightSum) || weightSum <= 0)
					return c.Fail(
						Status::InvalidValue,
						"Repeat Texture cell denominator is zero or nonfinite",
						"randomness"
					);
				for (size_t channel = 0; channel < 3; ++channel)
					pixel[channel] /= weightSum;
				pixel[3] = 1.f;
			}
			const auto format = *DescribeSurfaceFormat(in.Format);
			for (size_t channel = 0; channel < 4; ++channel) {
				if (!std::isfinite(pixel[channel]) ||
					(channel < format.Channels && format.FloatingPoint && format.BitsPerChannel == 16 &&
					 std::abs(pixel[channel]) > 65504.f))
					return c.Fail(
						Status::InvalidValue,
						"Repeat Texture output exceeds finite storage range",
						"surface_out"
					);
				out[channel] = pixel[channel];
			}
			// Clear + source BLEND_ALPHA has RGB factor ONE, so no extra alpha multiplication.
			return true;
		}

		bool
		QuoteRepeatTexture(NodeContext &c, const RepeatTextureInputs &in, uint64_t &work, uint64_t &bytes) {
			const uint64_t pixels = uint64_t(in.Width) * in.Height;
			const uint64_t pixelWork = in.Type == 0 ? 64 : in.Type == 1 ? 256 : 576;
			if (work > REPEAT_TEXTURE_WORK_LIMIT || pixels > (REPEAT_TEXTURE_WORK_LIMIT - work) / pixelWork)
				return c.Fail(
					Status::LimitExceeded, "Repeat Texture complete batch exceeds work limits", "surface_out"
				);
			const auto layout = CheckedSurfaceLayout(in.Width, in.Height, in.Format, c.AvailableBytes());
			if (!layout || bytes > c.AvailableBytes() || layout->Bytes > c.AvailableBytes() - bytes)
				return c.Fail(
					Status::LimitExceeded,
					"Repeat Texture complete batch exceeds live output byte limits",
					"surface_out"
				);
			for (uint32_t y = 0; y < in.Height; ++y)
				for (uint32_t x = 0; x < in.Width; ++x) {
					Rgba pixel;
					if (!RepeatTexturePixel(c, in, x, y, pixel)) return false;
				}
			work += pixels * pixelWork;
			bytes += layout->Bytes;
			return true;
		}
	}

	bool AdmitSourceRepeatTexture(NodeContext &context, uint64_t &batchWork, uint64_t &batchBytes) {
		ENGINE_PROFILE("imagegraph.source.repeat_texture_admission");
		RepeatTextureInputs inputs;
		return PrepareRepeatTexture(context, inputs) &&
			   QuoteRepeatTexture(context, inputs, batchWork, batchBytes);
	}

	bool DrawSourceRepeatTexture(NodeContext &context) {
		ENGINE_PROFILE("imagegraph.source.repeat_texture");
		RepeatTextureInputs inputs;
		uint64_t work = 0, bytes = 0;
		if (!PrepareRepeatTexture(context, inputs) || !QuoteRepeatTexture(context, inputs, work, bytes))
			return false;
		auto *out = context.NewImage("surface_out", inputs.Width, inputs.Height, inputs.Format);
		if (!out) return false;
		for (uint32_t y = 0; y < inputs.Height; ++y)
			for (uint32_t x = 0; x < inputs.Width; ++x) {
				Rgba pixel;
				if (!RepeatTexturePixel(context, inputs, x, y, pixel) || !WritePixel(*out, x, y, pixel))
					return context.FailureCode != Status::Ok ? false
															 : context.Fail(
																   Status::InvalidValue,
																   "Repeat Texture output sample is invalid",
																   "surface_out"
															   );
			}
		return true;
	}
}
