#include "SourceStrandNoise.hpp"

#include "Families.hpp"
#include "Source2DComplexGenerator.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <span>
#include <string_view>
#include <variant>
#include <vector>
namespace engine::imagegraph::detail {
	namespace {
		constexpr uint64_t STRAND_WORK_LIMIT = 64000000, STRAND_BASE_WORK = 512;
		constexpr float STRAND_TAU = 6.28318530718f;
		using Float2 = std::array<float, 2>;
		struct StrandInputs {
			source2d::ComplexCanvas Canvas;
			SurfaceFormat Format = SurfaceFormat::RGBA8Unorm;
			Float2 Dimension{}, Position{}, Curve{}, Opacity{0, 1}, LevelIn{0, 1}, LevelOut{0, 1};
			const Image *Uv = nullptr, *Mask = nullptr;
			float HashFactor = 1, Seed = 0, Slope = .5f, CurveScale = 1, CurveShift = 0, Thickness = 0,
				  UvMix = 1, Middle = 0;
			int32_t Count = 0, Axis = 0, Mode = 0;
			bool Covered = false;
		};
		bool StrandFinite(NodeContext &c, std::string_view port, float value) {
			return std::isfinite(value) ||
				   c.Fail(Status::InvalidValue, "Strand Noise shader arithmetic exceeds finite range", port);
		}
		bool StrandFloat(NodeContext &c, std::string_view port, double value, float &out) {
			if (!std::isfinite(value) || std::abs(value) > double(std::numeric_limits<float>::max()))
				return c.Fail(Status::InvalidValue, "Strand Noise value exceeds shader float range", port);
			out = float(value);
			return true;
		}
		bool StrandPair(NodeContext &c, std::string_view port, Vector2 value, Float2 &out) {
			return StrandFloat(c, port, value.X, out[0]) && StrandFloat(c, port, value.Y, out[1]);
		}
		bool StrandSurface(NodeContext &c, std::string_view port, const Image *image) {
			if (const auto *value = c.Find(port); value && std::holds_alternative<AtlasValue>(*value))
				return c.Fail(
					Status::UnsupportedExecution, "Strand Noise raw sampler binding rejects Atlas", port
				);
			return !image ||
				   ValidSurfaceLayout(*image, Limits::MaximumDimension, Limits::MaximumEvaluationBytes) ||
				   c.Fail(Status::InvalidValue, "Strand Noise sampler layout is invalid", port);
		}
		bool PrepareStrand(NodeContext &c, StrandInputs &in) {
			in.Uv = c.Input("uv_map");
			in.Mask = c.Input("mask");
			if (!StrandSurface(c, "uv_map", in.Uv) || !StrandSurface(c, "mask", in.Mask) ||
				!source2d::ResolveGeneratorDimensions(c, in.Mask, in.Canvas.Width, in.Canvas.Height))
				return false;
			if (in.Canvas.Width > c.Request.MaximumImageDimension ||
				in.Canvas.Height > c.Request.MaximumImageDimension)
				return c.Fail(
					Status::LimitExceeded, "Strand Noise output exceeds request dimensions", "dimension"
				);
			in.Canvas.Raw = c.Vec2("dimension", {1, 1});
			if (!c.IsLinked("dimension")) {
				const auto unit = c.Integer("dimension_unit", 1);
				if (unit == 1) {
					in.Canvas.Raw.X *= c.Project.SurfaceWidth;
					in.Canvas.Raw.Y *= c.Project.SurfaceHeight;
				} else if (unit == 2) {
					in.Canvas.Raw.X *= in.Mask->Width;
					in.Canvas.Raw.Y *= in.Mask->Height;
				}
			}
			if (!StrandPair(c, "dimension", in.Canvas.Raw, in.Dimension)) return false;
			const auto format = ResolveProcessorSurfaceFormat(c, nullptr);
			if (!format) return false;
			in.Format = *format;
			in.Covered = in.Dimension[0] > .5f && in.Dimension[1] > .5f;
			// grug clear target and absent fragments do not consume shader uniforms.
			if (!in.Covered) return c.FailureCode == Status::Ok;
			if (in.Uv && !StrandFloat(c, "uv_mix", c.Scalar("uv_mix", 1), in.UvMix)) return false;
			if (!StrandPair(c, "level_in", c.Vec2("level_in", {0, 1}), in.LevelIn) ||
				!StrandPair(c, "level_out", c.Vec2("level_out", {0, 1}), in.LevelOut))
				return false;
			if (in.LevelIn[0] == in.LevelIn[1])
				return c.Fail(
					Status::UnsupportedExecution, "Strand Noise equal input levels divide by zero", "level_in"
				);
			if (!StrandFinite(c, "level_in", in.LevelIn[1] - in.LevelIn[0])) return false;
			const auto axis = c.Integer("axis", 0), mode = c.Integer("mode", 0);
			if (axis < std::numeric_limits<int32_t>::min() || axis > std::numeric_limits<int32_t>::max())
				return c.Fail(Status::InvalidValue, "Strand Noise axis exceeds shader integer range", "axis");
			if (mode < std::numeric_limits<int32_t>::min() || mode > std::numeric_limits<int32_t>::max())
				return c.Fail(Status::InvalidValue, "Strand Noise mode exceeds shader integer range", "mode");
			in.Axis = int32_t(axis);
			in.Mode = int32_t(mode);
			float density = 0;
			if (!StrandFloat(c, "density", c.Scalar("density", .5), density)) return false;
			const float amount = density * in.Dimension[in.Axis == 0 ? 0 : 1];
			if (!StrandFinite(c, "density", amount)) return false;
			if (double(amount) < double(std::numeric_limits<int32_t>::min()) || double(amount) >= 2147483648.)
				return c.Fail(
					Status::UnsupportedExecution,
					"Strand Noise strand count exceeds shader integer range",
					"density"
				);
			in.Count = int32_t(amount);
			if (in.Count <= 0) return c.FailureCode == Status::Ok;
			if (!c.Find("seed"))
				return c.Fail(
					Status::UnsupportedExecution, "Strand Noise requires a resolved source seed", "seed"
				);
			Vector2 position;
			if (!source2d::ReferenceVector(c, "position", in.Canvas.Raw, position) ||
				!StrandPair(c, "position", position, in.Position) ||
				!StrandFloat(c, "seed", c.Scalar("seed"), in.Seed) ||
				!StrandFloat(c, "slope", c.Scalar("slope", .5), in.Slope) ||
				!StrandFloat(c, "curve_scale", c.Scalar("curve_scale", 1), in.CurveScale) ||
				!StrandFloat(c, "curve_shift", c.Scalar("curve_shift"), in.CurveShift) ||
				!StrandPair(c, "curve", c.Vec2("curve", {0, 0}), in.Curve) ||
				!StrandPair(c, "opacity", c.Vec2("opacity", {0, 1}), in.Opacity))
				return false;
			const float seedMod = in.Seed - 100000.f * std::floor(in.Seed / 100000.f);
			if (!StrandFinite(c, "seed", seedMod)) return false;
			in.HashFactor = 1.f + seedMod / 10.f;
			if (!StrandFinite(c, "seed", in.HashFactor)) return false;
			in.Middle = 1.f - std::min(1.f / in.Dimension[0], 1.f / in.Dimension[1]) / 2.f;
			if (in.Mode >= 0 && in.Mode <= 2) {
				if (!StrandFloat(c, "thickness", c.Scalar("thickness"), in.Thickness)) return false;
				if (in.Mode != 1 && !(in.Middle - in.Thickness < in.Middle + in.Thickness))
					return c.Fail(
						Status::UnsupportedExecution,
						"Strand Noise smoothstep edges are equal or reversed",
						"thickness"
					);
				if (!StrandFinite(c, "thickness", in.Middle - in.Thickness) ||
					!StrandFinite(c, "thickness", in.Middle + in.Thickness))
					return false;
				if (in.Mode != 1 &&
					!StrandFinite(c, "thickness", (in.Middle + in.Thickness) - (in.Middle - in.Thickness)))
					return false;
			}
			return c.FailureCode == Status::Ok;
		}
		float StrandFract(float value) {
			return value - std::floor(value);
		}
		float StrandMix(float low, float high, float weight) {
			return low * (1.f - weight) + high * weight;
		}
		float StrandRandom(const StrandInputs &in, float x, float y) {
			const float dot = (x + 1.f) * 2.f + (y + 6.f) * 7.f;
			return StrandFract(std::sin(dot) * in.HashFactor);
		}
		bool ShadeStrand(NodeContext &c, const StrandInputs &in, uint32_t x, uint32_t y, Rgba &pixel) {
			if (!in.Covered || float(x) + .5f >= in.Dimension[0] || float(y) + .5f >= in.Dimension[1]) {
				pixel = {0, 0, 0, 0};
				return true;
			}
			const float u = (float(x) + .5f) / in.Dimension[0], v = (float(y) + .5f) / in.Dimension[1];
			Float2 uv{u, v};
			float alpha = 1;
			if (in.Uv) {
				const auto sample = SampleNearest(*in.Uv, u, v);
				uv = {
					StrandMix(u, float(sample[0]), in.UvMix), StrandMix(v, 1.f - float(sample[1]), in.UvMix)
				};
				alpha = float(sample[3]);
			}
			if (!StrandFinite(c, "uv_map", alpha) || !StrandFinite(c, "uv_map", uv[0]) ||
				!StrandFinite(c, "uv_map", uv[1]))
				return false;
			float value = 0;
			if (in.Count > 0) {
				const Float2 ps{
					uv[0] + in.Position[0], uv[1] * (in.Dimension[1] / in.Dimension[0]) + in.Position[1]
				};
				const float px = ps[in.Axis == 0 ? 0 : 1], py = ps[in.Axis == 0 ? 1 : 0],
							dx = in.Dimension[in.Axis == 0 ? 0 : 1], dy = in.Dimension[in.Axis == 0 ? 1 : 0];
				if (!StrandFinite(c, "position", px) || !StrandFinite(c, "position", py)) return false;
				for (int32_t i = 0; i < in.Count; ++i) {
					const float fi = float(i), ry = StrandRandom(in, 1.f, fi),
								rs = StrandRandom(in, 2.f, fi) - .5f;
					const float opacity = StrandMix(in.Opacity[0], in.Opacity[1], StrandRandom(in, fi, 2.f));
					const float curve = StrandMix(in.Curve[0], in.Curve[1], StrandRandom(in, fi, 3.f));
					if (!StrandFinite(c, "curve", curve) ||
						((in.Mode == 0 || in.Mode == 1) && !StrandFinite(c, "opacity", opacity)))
						return false;
					float rx = StrandRandom(in, fi, 1.f);
					rx += rs * 2.f * (py - ry) * in.Slope;
					const float angle = (py - ry) * in.CurveScale * dy / 4.f + in.CurveShift * STRAND_TAU;
					if (!StrandFinite(c, "slope", rx) || !StrandFinite(c, "curve_scale", angle)) return false;
					rx += std::sin(angle) * curve / dx * 2.f;
					if (!StrandFinite(c, "curve", rx)) return false;
					if (in.Mode >= 0 && in.Mode <= 2 && !StrandFinite(c, "position", px - rx)) return false;
					if (in.Mode == 0 || in.Mode == 2) {
						const float sample =
							in.Mode == 0 ? 1.f - std::abs(px - rx) : 1.f - std::max(0.f, px - rx);
						if (!StrandFinite(c, "thickness", sample - (in.Middle - in.Thickness))) return false;
						float t = std::clamp(
							(sample - (in.Middle - in.Thickness)) /
								((in.Middle + in.Thickness) - (in.Middle - in.Thickness)),
							0.f,
							1.f
						);
						const float smooth = t * t * (3.f - 2.f * t);
						if (in.Mode == 0)
							value = std::max(value, smooth * opacity);
						else
							value += smooth * (1.f / float(in.Count));
					} else if (in.Mode == 1) {
						const float distance = std::abs(1.f - std::abs(px - rx) - in.Middle);
						value =
							std::max(value, (1.f - (distance < in.Thickness / 2.f ? 0.f : 1.f)) * opacity);
					}
					if (!StrandFinite(c, "opacity", value)) return false;
				}
			}
			value = StrandMix(
				in.LevelOut[0], in.LevelOut[1], (value - in.LevelIn[0]) / (in.LevelIn[1] - in.LevelIn[0])
			);
			if (!StrandFinite(c, "level_out", value)) return false;
			pixel = {value, value, value, alpha};
			return true;
		}
		bool QuoteStrand(NodeContext &c, const StrandInputs &in, uint64_t &work) {
			const bool mask =
				in.Mask || std::any_of(c.ImageArrays.begin(), c.ImageArrays.end(), [](const auto &input) {
					return input.first == "mask" && input.second && !input.second->Images.empty();
				});
			if (!source2d::ComplexBatchAdmission(
					c, 1, "surface_out", "surface_out", 1 + size_t(mask) + size_t(c.NoiseFieldRequested)
				))
				return false;
			const uint64_t perPixel = STRAND_BASE_WORK +
									  (in.Covered ? uint64_t(std::max(in.Count, int32_t(0))) * 512 : 0),
						   pixels = uint64_t(in.Canvas.Width) * in.Canvas.Height;
			if (work > STRAND_WORK_LIMIT || pixels > (STRAND_WORK_LIMIT - work) / perPixel)
				return c.Fail(
					Status::LimitExceeded, "Strand Noise complete batch exceeds work limit", "surface_out"
				);
			work += pixels * perPixel;
			return true;
		}
		bool DrawStrand(NodeContext &c) {
			ENGINE_PROFILE("imagegraph.source.strand_noise");
			StrandInputs in;
			uint64_t work = 0;
			if (!PrepareStrand(c, in) || !QuoteStrand(c, in, work)) return false;
			const uint64_t bytes = uint64_t(in.Canvas.Width) * in.Canvas.Height * 4;
			auto charge = c.ReserveWorkspace(in.Mask ? bytes : 0, "surface_out");
			if (!charge) return false;
			Image scratch;
			if (in.Mask)
				scratch = {
					in.Canvas.Width,
					in.Canvas.Height,
					std::vector<uint8_t>(size_t(bytes)),
					0,
					SurfaceFormat::RGBA8Unorm
				};
			auto *out = c.NewImage("surface_out", in.Canvas.Width, in.Canvas.Height, in.Format);
			if (!out) return false;
			for (uint32_t y = 0; y < in.Canvas.Height; ++y)
				for (uint32_t x = 0; x < in.Canvas.Width; ++x) {
					Rgba pixel{};
					if (!ShadeStrand(c, in, x, y, pixel)) return false;
					if (!WritePixel(*out, x, y, pixel))
						return c.Fail(
							Status::InvalidValue,
							"Strand Noise sample exceeds output storage range",
							"surface_out"
						);
					if (in.Mask) {
						pixel = ReadPixel(*out, x, y);
						const auto mask = SampleNearest(
							*in.Mask, (float(x) + .5f) / in.Canvas.Width, (float(y) + .5f) / in.Canvas.Height
						);
						const float brightness =
							(float(mask[0]) + float(mask[1]) + float(mask[2])) / 3.f * float(mask[3]);
						pixel[3] = float(pixel[3]) * brightness;
						if (!StrandFinite(c, "mask", brightness) || !WritePixel(scratch, x, y, pixel))
							return c.Fail(
								Status::InvalidValue, "Strand Noise mask exceeds finite storage range", "mask"
							);
					}
				}
			if (in.Mask)
				for (uint32_t y = 0; y < in.Canvas.Height; ++y)
					for (uint32_t x = 0; x < in.Canvas.Width; ++x)
						if (!WritePixel(*out, x, y, ReadPixel(scratch, x, y)))
							return c.Fail(
								Status::InvalidValue,
								"Strand Noise mask copy exceeds storage range",
								"surface_out"
							);
			return c.FailureCode == Status::Ok;
		}
	}
	bool AdmitSourceStrandNoise(NodeContext &context, uint64_t &work) {
		StrandInputs in;
		return PrepareStrand(context, in) && QuoteStrand(context, in, work);
	}
	std::span<const ExecutorEntry> SourceStrandNoiseExecutors() {
		static constexpr ExecutorEntry entries[]{{"pc.noise_strand", DrawStrand, true}};
		return entries;
	}
}
