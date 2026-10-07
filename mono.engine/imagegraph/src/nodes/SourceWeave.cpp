#include "SourceWeave.hpp"

#include "Families.hpp"
#include "Source2DComplexGenerator.hpp"
#include "SourceNormalShaderCurve.hpp"
#include "SourceShaderGradient.hpp"

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
		constexpr uint64_t WEAVE_WORK_LIMIT = 64000000, WEAVE_PIXEL_WORK = 8192;
		using Float2 = std::array<float, 2>;
		using Float4 = std::array<float, 4>;
		struct WeaveInputs {
			source2d::ComplexCanvas Canvas;
			SurfaceFormat Format = SurfaceFormat::RGBA8Unorm;
			Float2 Dimension{}, Position{}, Scale{}, Width{};
			const Image *Uv = nullptr, *Mask = nullptr;
			float UvMix = 1, Cosine = 1, Sine = 0;
			bool Covered = false;
		};
		bool WeaveFinite(NodeContext &c, std::string_view port, float value) {
			return std::isfinite(value) ||
				   c.Fail(Status::InvalidValue, "Weave shader arithmetic exceeds finite range", port);
		}
		bool WeaveFloat(NodeContext &c, std::string_view port, double value, float &out) {
			if (!std::isfinite(value) || std::abs(value) > double(std::numeric_limits<float>::max()))
				return c.Fail(Status::InvalidValue, "Weave value exceeds shader float range", port);
			out = float(value);
			return true;
		}
		bool WeavePair(NodeContext &c, std::string_view port, Vector2 value, Float2 &out) {
			return WeaveFloat(c, port, value.X, out[0]) && WeaveFloat(c, port, value.Y, out[1]);
		}
		bool WeaveSurface(NodeContext &c, std::string_view port, const Image *image) {
			if (const auto *value = c.Find(port); value && std::holds_alternative<AtlasValue>(*value))
				return c.Fail(Status::UnsupportedExecution, "Weave raw sampler binding rejects Atlas", port);
			return !image ||
				   ValidSurfaceLayout(*image, Limits::MaximumDimension, Limits::MaximumEvaluationBytes) ||
				   c.Fail(Status::InvalidValue, "Weave sampler layout is invalid", port);
		}
		bool PrepareWeave(NodeContext &c, WeaveInputs &in) {
			in.Uv = c.Input("uv_map");
			in.Mask = c.Input("mask");
			if (!WeaveSurface(c, "uv_map", in.Uv) || !WeaveSurface(c, "mask", in.Mask) ||
				!source2d::ResolveGeneratorDimensions(c, in.Mask, in.Canvas.Width, in.Canvas.Height))
				return false;
			if (in.Canvas.Width > c.Request.MaximumImageDimension ||
				in.Canvas.Height > c.Request.MaximumImageDimension)
				return c.Fail(Status::LimitExceeded, "Weave output exceeds request dimensions", "dimension");
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
			if (!WeavePair(c, "dimension", in.Canvas.Raw, in.Dimension)) return false;
			const auto format = ResolveProcessorSurfaceFormat(c, nullptr);
			if (!format) return false;
			in.Format = *format;
			in.Covered = in.Dimension[0] > .5f && in.Dimension[1] > .5f;
			// grug clear target and absent fragments do not consume shader uniforms.
			if (!in.Covered) return c.FailureCode == Status::Ok;
			if (in.Uv && !WeaveFloat(c, "uv_mix", c.Scalar("uv_mix", 1), in.UvMix)) return false;
			Vector2 pos, scale;
			if (!source2d::ReferenceVector(c, "position", in.Canvas.Raw, pos) ||
				!source2d::ReferenceVector(c, "scale", in.Canvas.Raw, scale) ||
				!WeavePair(c, "position", pos, in.Position) || !WeavePair(c, "scale", scale, in.Scale) ||
				!WeavePair(c, "width", c.Vec2("width", {.5, .5}), in.Width))
				return false;
			if (in.Scale[0] == 0 || in.Scale[1] == 0)
				return c.Fail(Status::UnsupportedExecution, "Weave Scale divides by zero", "scale");
			for (size_t lane = 0; lane < 2; ++lane)
				if (in.Scale[lane] / in.Dimension[lane] == 0)
					return c.Fail(
						Status::UnsupportedExecution, "Weave normalized Scale is a zero divisor", "scale"
					);
			for (size_t lane = 0; lane < 2; ++lane)
				if (!WeaveFinite(c, "scale", in.Scale[lane] / in.Dimension[lane])) return false;
			float angle = 0;
			if (!WeaveFloat(c, "angle", c.Scalar("angle"), angle)) return false;
			angle *= .017453292519943295f;
			if (!WeaveFinite(c, "angle", angle)) return false;
			in.Cosine = std::cos(angle);
			in.Sine = std::sin(angle);
			return c.FailureCode == Status::Ok;
		}
		float WeaveFract(float value) {
			return value - std::floor(value);
		}
		float WeaveMix(float low, float high, float weight) {
			return low * (1.f - weight) + high * weight;
		}
		float WeaveMod(float x, float y) {
			return x - y * std::floor(x / y);
		}
		Float4 WeaveColor(NodeContext &c, std::string_view port, Colour fallback) {
			const auto color = c.Get<Colour>(port, fallback);
			return {
				float(color.Red) / 255.f,
				float(color.Green) / 255.f,
				float(color.Blue) / 255.f,
				float(color.Alpha) / 255.f
			};
		}
		bool WeaveRandom(NodeContext &c, Float2 point, float &value) {
			if (!c.Find("seed"))
				return c.Fail(
					Status::UnsupportedExecution, "Weave random branch requires resolved source Seed", "seed"
				);
			float seed = 0;
			if (!WeaveFloat(c, "seed", c.Scalar("seed"), seed)) return false;
			const auto random = [&](float lane, float &out) {
				const float offset = WeaveMod(lane, 100000.f) / 10.f;
				const float angle = (point[0] + offset) * 1892.9898f + (point[1] + offset) * 78.23453f;
				if (!WeaveFinite(c, "seed", angle)) return false;
				out = WeaveFract(std::sin(angle) * 437.54123f);
				return true;
			};
			float low = 0, high = 0;
			if (!random(std::floor(seed) / 5000.f, low) || !random((std::floor(seed) + 1.f) / 5000.f, high))
				return false;
			value = WeaveMix(low, high, WeaveFract(seed));
			return WeaveFinite(c, "seed", value);
		}
		bool WeaveAxis(NodeContext &c, const WeaveInputs &in, Float2 index, int32_t pattern, bool &axis) {
			axis = false;
			if (pattern != 2) {
				for (size_t lane = 0; lane < 2; ++lane) {
					const float divisor = std::floor(in.Dimension[lane] / in.Scale[lane]);
					if (!WeaveFinite(c, "scale", divisor)) return false;
					if (divisor == 0)
						return c.Fail(
							Status::UnsupportedExecution, "Weave index modulo divides by zero", "scale"
						);
					index[lane] = WeaveMod(index[lane], divisor);
					if (!WeaveFinite(c, "scale", index[lane])) return false;
				}
			}
			if (pattern == 0) {
				float value = 0;
				if (!WeaveRandom(c, index, value)) return false;
				axis = value > .5f;
			} else if (pattern == 1) {
				const float sum = index[0] + index[1];
				if (!WeaveFinite(c, "scale", sum)) return false;
				axis = WeaveMod(sum, 2.f) > 0;
			} else if (pattern == 2) {
				const Image *map = c.Input("weave_map");
				if (!WeaveSurface(c, "weave_map", map)) return false;
				if (!map)
					return c.Fail(
						Status::UnsupportedExecution,
						"Weave Map branch has unresolved source sampler binding",
						"weave_map"
					);
				const Float2 delta{
					(index[0] + .5f) * (in.Scale[0] / in.Dimension[0]),
					(index[1] + .5f) * (in.Scale[1] / in.Dimension[1])
				};
				Float2 uv{
					delta[0] * in.Cosine + delta[1] * in.Sine + in.Position[0] / in.Dimension[0],
					-delta[0] * in.Sine + delta[1] * in.Cosine + in.Position[1] / in.Dimension[1]
				};
				// grug matrix overflow must not reach sampler integer conversion.
				for (float lane : uv)
					if (!WeaveFinite(c, "weave_map", lane)) return false;
				// grug sampleTexture reads UV again even with mapBlend zero.
				if (in.Uv) {
					const auto sample = SampleNearest(*in.Uv, uv[0], uv[1]);
					const float weight = 0.f * in.UvMix;
					uv = {
						WeaveMix(uv[0], float(sample[0]), weight),
						WeaveMix(uv[1], 1.f - float(sample[1]), weight)
					};
				}
				for (float lane : uv)
					if (!WeaveFinite(c, "weave_map", lane)) return false;
				if (uv[0] < 0 || uv[0] > 1 || uv[1] < 0 || uv[1] > 1)
					return c.Fail(
						Status::UnsupportedExecution,
						"Weave Map outside coordinates need unresolved source sampleMode",
						"weave_map"
					);
				const auto sample = SampleNearest(*map, uv[0], uv[1]);
				const float value = float(sample[0]) * float(sample[3]);
				if (!WeaveFinite(c, "weave_map", value)) return false;
				axis = value > .5f;
			}
			return true;
		}
		bool WeaveGradient(NodeContext &c, float progress, Float4 &out) {
			const auto *value = c.Find("random_color");
			const auto *gradient = value ? std::get_if<Gradient>(value) : nullptr;
			if (!gradient || gradient->Keys.empty() || gradient->Keys.size() > 64)
				return c.Fail(
					Status::UnsupportedExecution,
					"Weave gradient exceeds defined GLSL key layout",
					"random_color"
				);
			const auto color = [&](size_t i) {
				const auto &v = gradient->Keys[i].Color;
				return Float4{
					float(v.Red) / 255.f,
					float(v.Green) / 255.f,
					float(v.Blue) / 255.f,
					float(v.Alpha) / 255.f
				};
			};
			for (size_t i = 0; i < gradient->Keys.size(); ++i) {
				float time = 0;
				if (!WeaveFloat(c, "random_color", gradient->Keys[i].Time, time)) return false;
				if (time == progress || (time > progress && i == 0)) {
					out = color(i);
					return true;
				}
				if (time > progress) {
					float before = 0;
					if (!WeaveFloat(c, "random_color", gradient->Keys[i - 1].Time, before)) return false;
					const float t = (progress - before) / (time - before);
					const auto a = color(i - 1), b = color(i);
					if (gradient->Mode == 1) {
						out = a;
						return true;
					}
					if (gradient->Mode > 6) break;
					const auto rgb =
						ShaderGradientMix({a[0], a[1], a[2]}, {b[0], b[1], b[2]}, t, gradient->Mode);
					out = {rgb[0], rgb[1], rgb[2], WeaveMix(a[3], b[3], t)};
					for (float lane : out)
						if (!WeaveFinite(c, "random_color", lane)) return false;
					return true;
				}
			}
			out = color(gradient->Keys.size() - 1);
			return true;
		}
		bool WeaveShade(NodeContext &c, float amount, Float4 &color) {
			if (!c.Boolean("shading_curved"))
				return c.Fail(
					Status::UnsupportedExecution,
					"Weave shading needs unresolved source curve uniform state",
					"shading_curved"
				);
			const auto *value = c.Find("shading_curve");
			const auto *curve = value ? std::get_if<Curve>(value) : nullptr;
			if (!curve || curve->Anchors.size() < 2 || curve->Anchors.size() > 9 ||
				float(curve->Header[1]) == 0)
				return c.Fail(
					Status::UnsupportedExecution, "Weave curve exceeds defined GLSL layout", "shading_curve"
				);
			for (double lane : curve->Header) {
				float upload = 0;
				if (!WeaveFloat(c, "shading_curve", lane, upload)) return false;
			}
			for (const auto &anchor : curve->Anchors)
				for (double lane : anchor) {
					float upload = 0;
					if (!WeaveFloat(c, "shading_curve", lane, upload)) return false;
				}
			float shading = 0;
			if (!WeaveFloat(c, "shading", c.Scalar("shading", .5), shading)) return false;
			const float evaluated = NormalEvalShaderCurve(*curve, amount);
			if (!WeaveFinite(c, "shading_curve", evaluated)) return false;
			const float weight = 1.f - (1.f - evaluated) * shading;
			if (!WeaveFinite(c, "shading", weight)) return false;
			const auto shade = WeaveColor(c, "shade_color", {0, 0, 0, 255});
			for (size_t i = 0; i < 3; ++i)
				color[i] = WeaveMix(shade[i], color[i], weight);
			return true;
		}
		bool WeaveChoice(NodeContext &c, std::string_view port, int32_t &choice) {
			const auto value = c.Integer(port, 0);
			if (value < std::numeric_limits<int32_t>::min() || value > std::numeric_limits<int32_t>::max())
				return c.Fail(Status::InvalidValue, "Weave choice exceeds shader integer range", port);
			choice = int32_t(value);
			return c.FailureCode == Status::Ok;
		}
		bool ShadeWeave(NodeContext &c, const WeaveInputs &in, uint32_t x, uint32_t y, Rgba &pixel) {
			if (!in.Covered || float(x) + .5f >= in.Dimension[0] || float(y) + .5f >= in.Dimension[1]) {
				pixel = {0, 0, 0, 0};
				return true;
			}
			const float u = (float(x) + .5f) / in.Dimension[0], v = (float(y) + .5f) / in.Dimension[1];
			Float2 uv{u, v};
			float alpha = 1;
			if (in.Uv) {
				const auto sample = SampleNearest(*in.Uv, u, v);
				uv = {WeaveMix(u, float(sample[0]), in.UvMix), WeaveMix(v, 1.f - float(sample[1]), in.UvMix)};
				alpha = float(sample[3]);
			}
			for (float lane : uv)
				if (!WeaveFinite(c, "uv_map", lane)) return false;
			if (!WeaveFinite(c, "uv_map", alpha)) return false;
			const Float2 delta{
				uv[0] - in.Position[0] / in.Dimension[0], uv[1] - in.Position[1] / in.Dimension[1]
			};
			const Float2 tx{
				(delta[0] * in.Cosine - delta[1] * in.Sine) / (in.Scale[0] / in.Dimension[0]),
				(delta[0] * in.Sine + delta[1] * in.Cosine) / (in.Scale[1] / in.Dimension[1])
			};
			for (float lane : tx)
				if (!WeaveFinite(c, "scale", lane)) return false;
			const Float2 index{std::floor(tx[0]), std::floor(tx[1])}, wuv{tx[0] - index[0], tx[1] - index[1]};
			const float wx = in.Width[0] / 2.f, wy = in.Width[1] / 2.f;
			const bool fx = std::abs(wuv[0] - .5f) < wx, fy = std::abs(wuv[1] - .5f) < wy;
			Float4 color = WeaveColor(c, "bg_color", {0, 0, 0, 255});
			if (fx || fy) {
				int32_t pattern = 0, type = 0;
				if (!WeaveChoice(c, "weave_pattern", pattern) || !WeaveChoice(c, "color_type", type))
					return false;
				bool axis = false;
				if (!WeaveAxis(c, in, index, pattern, axis)) return false;
				color = WeaveColor(c, "color", {255, 255, 255, 255});
				if (type == 1 && ((axis && fx) || (!axis && fx && !fy)))
					color = WeaveColor(c, "color_2", {255, 255, 255, 255});
				else if (type == 2) {
					Float2 id = index;
					if ((axis && fx) || (!axis && fx && !fy))
						id[1] = 0;
					else
						id[0] = 0;
					float random = 0, shift = 0;
					if (!WeaveRandom(c, id, random) || !WeaveFloat(c, "shift", c.Scalar("shift"), shift))
						return false;
					const float sum = random + shift;
					if (!WeaveFinite(c, "shift", sum)) return false;
					if (!WeaveGradient(c, WeaveFract(WeaveFract(sum) + 1.f), color)) return false;
				}
				bool shade = false;
				float distance = 0, width = 0;
				if (axis) {
					if (!fx && fy) {
						shade = true;
						distance = std::abs(wuv[0] - .5f);
						width = wx;
					} else {
						const float off = wuv[1] < .5f ? -1.f : 1.f;
						bool next = false;
						if (!WeaveAxis(c, in, {index[0], index[1] + off}, pattern, next)) return false;
						if (!next) {
							shade = true;
							distance = std::abs(wuv[1] - off - .5f);
							width = wy;
						}
					}
				} else {
					if (fx && !fy) {
						shade = true;
						distance = std::abs(wuv[1] - .5f);
						width = wy;
					} else {
						const float off = wuv[0] < .5f ? -1.f : 1.f;
						bool next = false;
						if (!WeaveAxis(c, in, {index[0] + off, index[1]}, pattern, next)) return false;
						if (next) {
							shade = true;
							distance = std::abs(wuv[0] - off - .5f);
							width = wx;
						}
					}
				}
				if (shade) {
					float span = 0;
					if (!WeaveFloat(c, "shade_span", c.Scalar("shade_span", .5), span)) return false;
					const float amount = (distance - width) / std::max(.0001f, span - width);
					if (!WeaveFinite(c, "shade_span", amount) || !WeaveShade(c, amount, color)) return false;
				}
			}
			color[3] *= alpha;
			for (float lane : color)
				if (!WeaveFinite(c, "surface_out", lane)) return false;
			pixel = {color[0], color[1], color[2], color[3]};
			return true;
		}

		bool QuoteWeave(NodeContext &c, const WeaveInputs &in, uint64_t &work) {
			const bool mask =
				in.Mask || std::any_of(c.ImageArrays.begin(), c.ImageArrays.end(), [](const auto &input) {
					return input.first == "mask" && input.second && !input.second->Images.empty();
				});
			if (!source2d::ComplexBatchAdmission(
					c, 1, "surface_out", "surface_out", 1 + size_t(mask) + size_t(c.NoiseFieldRequested)
				))
				return false;
			const uint64_t perPixel = in.Covered ? WEAVE_PIXEL_WORK : 512,
						   pixels = uint64_t(in.Canvas.Width) * in.Canvas.Height;
			if (work > WEAVE_WORK_LIMIT || pixels > (WEAVE_WORK_LIMIT - work) / perPixel)
				return c.Fail(
					Status::LimitExceeded, "Weave complete batch exceeds work limit", "surface_out"
				);
			work += pixels * perPixel;
			// grug source curve and sampler state must be proved before any output is made.
			for (uint32_t y = 0; y < in.Canvas.Height; ++y)
				for (uint32_t x = 0; x < in.Canvas.Width; ++x) {
					Rgba pixel{};
					if (!ShadeWeave(c, in, x, y, pixel)) return false;
				}
			return true;
		}
		bool DrawWeave(NodeContext &c) {
			ENGINE_PROFILE("imagegraph.source.weave");
			WeaveInputs in;
			uint64_t work = 0;
			if (!PrepareWeave(c, in) || !QuoteWeave(c, in, work)) return false;
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
					if (!ShadeWeave(c, in, x, y, pixel)) return false;
					if (!WritePixel(*out, x, y, pixel))
						return c.Fail(
							Status::InvalidValue, "Weave sample exceeds output storage range", "surface_out"
						);
					if (in.Mask) {
						pixel = ReadPixel(*out, x, y);
						const auto mask = SampleNearest(
							*in.Mask, (float(x) + .5f) / in.Canvas.Width, (float(y) + .5f) / in.Canvas.Height
						);
						const float brightness =
							(float(mask[0]) + float(mask[1]) + float(mask[2])) / 3.f * float(mask[3]);
						pixel[3] = float(pixel[3]) * brightness;
						if (!WeaveFinite(c, "mask", brightness) || !WritePixel(scratch, x, y, pixel))
							return c.Fail(
								Status::InvalidValue, "Weave mask exceeds finite storage range", "mask"
							);
					}
				}
			if (in.Mask)
				for (uint32_t y = 0; y < in.Canvas.Height; ++y)
					for (uint32_t x = 0; x < in.Canvas.Width; ++x)
						if (!WritePixel(*out, x, y, ReadPixel(scratch, x, y)))
							return c.Fail(
								Status::InvalidValue, "Weave mask copy exceeds storage range", "surface_out"
							);
			return c.FailureCode == Status::Ok;
		}
	}
	bool AdmitSourceWeave(NodeContext &context, uint64_t &work) {
		WeaveInputs in;
		return PrepareWeave(context, in) && QuoteWeave(context, in, work);
	}
	std::span<const ExecutorEntry> SourceWeaveExecutors() {
		static constexpr ExecutorEntry entries[]{{"pc.weave", DrawWeave, true}};
		return entries;
	}
}
