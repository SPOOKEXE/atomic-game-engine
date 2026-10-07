#include "SourcePerlinExtra.hpp"

#include "../SourceMappedInputs.hpp"
#include "Families.hpp"
#include "Source2DComplexGenerator.hpp"
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
		constexpr uint64_t EXTRA_WORK_LIMIT = 64000000, EXTRA_BASE_WORK = 512, EXTRA_OCTAVE_WORK = 2048;
		using Float2 = std::array<float, 2>;
		struct ExtraInputs {
			source2d::ComplexCanvas Canvas;
			SurfaceFormat Format = SurfaceFormat::RGBA8Unorm;
			Float2 Dimension{}, Position{}, Scale{4, 4}, ParameterA{}, LevelIn{0, 1}, LevelOut{0, 1};
			std::array<Float2, 3> Colors{{{0, 1}, {0, 1}, {0, 1}}};
			const Image *Uv = nullptr, *Mask = nullptr, *ScaleMap = nullptr, *ParameterMap = nullptr;
			float Seed = 0, Rotation = 0, UvMix = 1;
			int32_t Iteration = 2, Type = 0, Mode = 0;
			bool Covered = false, Tile = true, UsesA = false;
		};
		bool ExtraFinite(NodeContext &c, std::string_view port, float value) {
			return std::isfinite(value) ||
				   c.Fail(Status::InvalidValue, "Extra Perlin shader arithmetic exceeds finite range", port);
		}
		bool ExtraFloat(NodeContext &c, std::string_view port, double value, float &out) {
			if (!std::isfinite(value) || std::abs(value) > double(std::numeric_limits<float>::max()))
				return c.Fail(Status::InvalidValue, "Extra Perlin value exceeds shader float range", port);
			out = float(value);
			return true;
		}
		bool ExtraPair(NodeContext &c, std::string_view port, Vector2 value, Float2 &out) {
			return ExtraFloat(c, port, value.X, out[0]) && ExtraFloat(c, port, value.Y, out[1]);
		}
		bool ExtraSurface(NodeContext &c, std::string_view port, const Image *image) {
			if (const auto *value = c.Find(port); value && std::holds_alternative<AtlasValue>(*value))
				return c.Fail(
					Status::UnsupportedExecution, "Extra Perlin raw sampler binding rejects Atlas", port
				);
			return !image ||
				   ValidSurfaceLayout(*image, Limits::MaximumDimension, Limits::MaximumEvaluationBytes) ||
				   c.Fail(Status::InvalidValue, "Extra Perlin sampler layout is invalid", port);
		}
		bool ExtraMappedControl(
			NodeContext &c, std::string_view port, std::string_view mapPort, Float2 &range, const Image *&map
		) {
			Vector2 value;
			if (!ReadSourceMappedRange(c, port, value)) return false;
			map = c.Input(mapPort);
			return ExtraSurface(c, mapPort, map) && ExtraPair(c, port, value, range);
		}
		bool PrepareExtra(NodeContext &c, ExtraInputs &in) {
			in.Uv = c.Input("uv_map");
			in.Mask = c.Input("mask");
			if (!ExtraSurface(c, "uv_map", in.Uv) || !ExtraSurface(c, "mask", in.Mask) ||
				!source2d::ResolveGeneratorDimensions(c, in.Mask, in.Canvas.Width, in.Canvas.Height))
				return false;
			if (in.Canvas.Width > c.Request.MaximumImageDimension ||
				in.Canvas.Height > c.Request.MaximumImageDimension)
				return c.Fail(
					Status::LimitExceeded, "Extra Perlin output exceeds request dimensions", "dimension"
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
			if (!ExtraPair(c, "dimension", in.Canvas.Raw, in.Dimension)) return false;
			const auto format = ResolveProcessorSurfaceFormat(c, nullptr);
			if (!format) return false;
			in.Format = *format;
			in.Covered = in.Dimension[0] > .5f && in.Dimension[1] > .5f;
			// grug clear target and absent fragments do not consume shader uniforms.
			if (!in.Covered) return c.FailureCode == Status::Ok;

			const auto iteration = c.Integer("iteration", 2), type = c.Integer("noise_type", 0),
					   mode = c.Integer("color_mode", 0);
			if (iteration < std::numeric_limits<int32_t>::min() ||
				iteration > std::numeric_limits<int32_t>::max())
				return c.Fail(
					Status::UnsupportedExecution,
					"Extra Perlin iteration exceeds shader integer range",
					"iteration"
				);
			if (type < std::numeric_limits<int32_t>::min() || type > std::numeric_limits<int32_t>::max())
				return c.Fail(
					Status::UnsupportedExecution,
					"Extra Perlin type exceeds shader integer range",
					"noise_type"
				);
			if (mode < 0 || mode > 2)
				return c.Fail(
					Status::UnsupportedExecution,
					"Extra Perlin color branch leaves source output uninitialized",
					"color_mode"
				);
			in.Iteration = int32_t(iteration);
			in.Type = int32_t(type);
			in.Mode = int32_t(mode);
			in.Tile = c.Boolean("tile", true);
			if (in.Type != 6) {
				if (!ExtraPair(c, "level_in", c.Vec2("level_in", {0, 1}), in.LevelIn) ||
					!ExtraPair(c, "level_out", c.Vec2("level_out", {0, 1}), in.LevelOut))
					return false;
				if (in.LevelIn[0] == in.LevelIn[1])
					return c.Fail(
						Status::UnsupportedExecution, "Extra Perlin equal levels divide by zero", "level_in"
					);
				if (!ExtraFinite(c, "level_in", in.LevelIn[1] - in.LevelIn[0])) return false;
			}
			if (in.Mode != 0) {
				constexpr std::array<std::string_view, 3> ports{
					"color_r_range", "color_g_range", "color_b_range"
				};
				for (size_t lane = 0; lane < 3; ++lane)
					if (!ExtraPair(c, ports[lane], c.Vec2(ports[lane], {0, 1}), in.Colors[lane]))
						return false;
			}
			if (in.Iteration <= 0) return c.FailureCode == Status::Ok;
			if (in.Uv && !ExtraFloat(c, "uv_mix", c.Scalar("uv_mix", 1), in.UvMix)) return false;
			if (!SourceRangeMapped(c, "scale"))
				return c.Fail(
					Status::UnsupportedExecution,
					"Extra Perlin unmapped Scale needs unresolved source sampler state",
					"scale"
				);
			in.UsesA = in.Type == 3 || in.Type == 4 || in.Type == 6 ||
					   ((in.Type == 1 || in.Type == 2 || in.Type == 5) && in.Iteration > 1);
			if (in.UsesA && !SourceRangeMapped(c, "parameter_a"))
				return c.Fail(
					Status::UnsupportedExecution,
					"Extra Perlin unmapped Parameter A needs unresolved source uniform state",
					"parameter_a"
				);
			if (!c.Find("seed"))
				return c.Fail(
					Status::UnsupportedExecution, "Extra Perlin requires resolved source Seed", "seed"
				);
			Vector2 position;
			if (!source2d::ReferenceVector(c, "position", in.Canvas.Raw, position) ||
				!ExtraPair(c, "position", position, in.Position) ||
				!ExtraFloat(c, "seed", c.Scalar("seed"), in.Seed) ||
				!ExtraFloat(c, "rotation", c.Scalar("rotation"), in.Rotation))
				return false;
			const auto *original = source2d::GeneratorOriginal(c, "scale");
			const auto *array = original ? std::get_if<ArrayValue>(original) : nullptr;
			if (array && (!array->Nested.empty() || !array->Items.empty() ||
						  std::any_of(array->Elements.begin(), array->Elements.end(), [](const auto &value) {
							  return !std::holds_alternative<double>(value) &&
									 !std::holds_alternative<int64_t>(value);
						  })))
				return c.Fail(
					Status::UnsupportedExecution,
					"Extra Perlin nested mapped Scale is not a defined vec2 upload",
					"scale"
				);
			if (!ExtraMappedControl(c, "scale", "scale_map", in.Scale, in.ScaleMap)) return false;
			if (in.UsesA &&
				!ExtraMappedControl(c, "parameter_a", "parameter_a_map", in.ParameterA, in.ParameterMap))
				return false;
			// grug Parameter B is uploaded by host but never affects any source result.
			return c.FailureCode == Status::Ok;
		}

		float ExtraFract(float value) {
			return value - std::floor(value);
		}
		float ExtraMod(float value, float divisor) {
			return value - divisor * std::floor(value / divisor);
		}
		float ExtraMix(float low, float high, float weight) {
			return low * (1.f - weight) + high * weight;
		}
		bool ExtraVectorFinite(NodeContext &c, std::string_view port, Float2 value) {
			return ExtraFinite(c, port, value[0]) && ExtraFinite(c, port, value[1]);
		}
		bool ExtraRandom(NodeContext &c, Float2 st, float seed, float &out) {
			const float angle = (st[0] + 21.4564f) * 12.9898f + (st[1] + 46.8564f) * 78.233f,
						factor = ExtraMod(43758.5453123f + seed, 100000.f);
			if (!ExtraFinite(c, "seed", angle) || !ExtraFinite(c, "seed", factor)) return false;
			out = ExtraFract(std::sin(angle) * factor / 10.f);
			return ExtraFinite(c, "seed", out);
		}
		bool ExtraRandomFloat(NodeContext &c, Float2 st, float seed, float &out) {
			float low = 0, high = 0;
			if (!ExtraRandom(c, st, std::floor(seed), low) ||
				!ExtraRandom(c, st, std::floor(seed) + 1.f, high))
				return false;
			out = ExtraMix(low, high, ExtraFract(seed));
			return ExtraFinite(c, "seed", out);
		}
		bool ExtraRandom2(NodeContext &c, Float2 st, float seed, Float2 &out) {
			return ExtraRandomFloat(c, st, seed, out[0]) &&
				   ExtraRandomFloat(c, st, seed + 1.864354564f, out[1]);
		}
		bool ExtraSmooth(NodeContext &c, float n, float count, float &out) {
			if (!ExtraFinite(c, "parameter_a", count)) return false;
			const float steps = std::floor(count);
			if (steps > float(EXTRA_WORK_LIMIT / 512))
				return c.Fail(
					Status::LimitExceeded, "Extra Perlin smooth loop exceeds CPU work limit", "parameter_a"
				);
			for (float i = 0; i < steps; i += 1.f) {
				n = n * n * (3.f - 2.f * n);
				if (!ExtraFinite(c, "parameter_a", n)) return false;
			}
			const float next = n * n * (3.f - 2.f * n);
			out = ExtraMix(n, next, ExtraFract(count));
			return ExtraFinite(c, "parameter_a", out);
		}
		float ExtraMapped(Float2 range, const Image *map, float u, float v) {
			if (!map) return range[0];
			const auto sample = SampleNearest(*map, u, v);
			return ExtraMix(
				range[0], range[1], (float(sample[0]) + float(sample[1]) + float(sample[2])) / 3.f
			);
		}
		bool
		ExtraParameters(NodeContext &c, const ExtraInputs &in, float u, float v, Float2 &scale, float &a) {
			scale = in.Scale;
			if (in.ScaleMap) {
				const float value = ExtraMapped(in.Scale, in.ScaleMap, u, v);
				scale = {value, value};
			}
			a = ExtraMapped(in.ParameterA, in.ParameterMap, u, v);
			if (!ExtraVectorFinite(c, "scale", scale) || !ExtraFinite(c, "parameter_a", a)) return false;
			if (in.Type == 3 && !(.5f - a < .5f + a))
				return c.Fail(
					Status::UnsupportedExecution,
					"Extra Perlin Camo smoothstep edges are equal or reversed",
					"parameter_a"
				);
			if (in.Type == 3 &&
				(!ExtraFinite(c, "parameter_a", .5f - a) || !ExtraFinite(c, "parameter_a", .5f + a) ||
				 !ExtraFinite(c, "parameter_a", (.5f + a) - (.5f - a))))
				return false;
			return true;
		}
		bool ExtraNoise(NodeContext &c, const ExtraInputs &in, Float2 st, Float2 scale, float a, float &out) {
			if (!ExtraVectorFinite(c, "scale", st)) return false;
			Float2 low{std::floor(st[0]), std::floor(st[1])}, high{low[0] + 1.f, low[1] + 1.f},
				fraction{ExtraFract(st[0]), ExtraFract(st[1])}, weight{};
			if (in.Tile)
				for (size_t lane = 0; lane < 2; ++lane) {
					if (scale[lane] == 0)
						return c.Fail(
							Status::UnsupportedExecution,
							"Extra Perlin tiled lattice modulo divides by zero",
							"scale"
						);
					low[lane] = ExtraMod(low[lane], scale[lane]);
					high[lane] = ExtraMod(high[lane], scale[lane]);
					if (!ExtraFinite(c, "scale", low[lane]) || !ExtraFinite(c, "scale", high[lane]))
						return false;
				}
			for (size_t lane = 0; lane < 2; ++lane) {
				weight[lane] = fraction[lane] * fraction[lane] * (3.f - 2.f * fraction[lane]);
				if (in.Type == 4 && !ExtraSmooth(c, fraction[lane], 2.f + a * 4.f, weight[lane]))
					return false;
			}
			std::array<float, 4> values{};
			for (size_t y = 0; y < 2; ++y)
				for (size_t x = 0; x < 2; ++x) {
					Float2 point{x ? high[0] : low[0], y ? high[1] : low[1]};
					if (in.Type == 0) {
						point = {point[0] * 2.f - 1.f, point[1] * 2.f - 1.f};
						Float2 random{};
						if (!ExtraRandom2(c, point, in.Seed, random)) return false;
						values[y * 2 + x] =
							random[0] * (fraction[0] - float(x)) + random[1] * (fraction[1] - float(y));
					} else if (!ExtraRandomFloat(c, point, in.Seed, values[y * 2 + x]))
						return false;
				}
			out = ExtraMix(
				ExtraMix(values[0], values[1], weight[0]),
				ExtraMix(values[2], values[3], weight[0]),
				weight[1]
			);
			if (in.Type == 0) out = std::abs(out);
			return ExtraFinite(c, "parameter_a", out);
		}
		bool
		ExtraOctaves(NodeContext &c, const ExtraInputs &in, Float2 st, Float2 scale, float a, float &out) {
			out = 0;
			if (in.Iteration <= 0) return true;
			const float count = float(in.Iteration), denominator = std::pow(2.f, count) - 1.f;
			float amplitude = 0;
			if (in.Type != 5) {
				amplitude = std::pow(2.f, in.Type == 0 ? count + 1.f : count - 1.f) / denominator;
				if (in.Type == 3) amplitude *= 1.25f;
				if (!ExtraFinite(c, "iteration", amplitude)) return false;
			}
			Float2 position = st;
			float memory = 0;
			const float iterations = in.Type == 3 ? count * 3.f : count;
			for (float i = 0; i < iterations; i += 1.f) {
				const bool last = i + 1.f >= iterations;
				float n = 0;
				if (!ExtraNoise(c, in, position, scale, a, n)) return false;
				if (in.Type == 3) {
					memory += n * amplitude;
					if (!ExtraFinite(c, "iteration", memory)) return false;
					if (ExtraMod(i, 3.f) == 2.f) {
						const float t = std::clamp((memory - (.5f - a)) / ((.5f + a) - (.5f - a)), 0.f, 1.f);
						out += t * t * (3.f - 2.f * t) * amplitude;
						memory = 0;
						if (last) return ExtraFinite(c, "iteration", out);
						for (size_t lane = 0; lane < 2; ++lane) {
							scale[lane] /= 1.5f;
							position[lane] /= 1.5f;
						}
						amplitude /= .75f;
					} else {
						for (size_t lane = 0; lane < 2; ++lane) {
							scale[lane] *= 1.5f;
							position[lane] *= 1.5f;
						}
						amplitude *= .75f;
					}
				} else if (in.Type == 4) {
					float value = 0;
					if (!ExtraSmooth(c, n, 1.f + a * 5.f * i / iterations, value)) return false;
					out += value * amplitude;
				} else if (in.Type == 5) {
					out = std::max(out, n);
					if (last) return ExtraFinite(c, "iteration", out);
					for (size_t lane = 0; lane < 2; ++lane) {
						scale[lane] *= 1.f + a * .1f;
						position[lane] *= 1.f + a * .1f;
					}
				} else
					out += n * amplitude;
				// grug last octave output does not consume the next octave state.
				if (last) return ExtraFinite(c, "iteration", out);
				if (!ExtraFinite(c, "iteration", out) || !ExtraVectorFinite(c, "scale", scale) ||
					!ExtraVectorFinite(c, "scale", position))
					return false;
				Float2 offset{};
				if (!ExtraRandom2(c, {i, i}, .574186f, offset)) return false;
				for (size_t lane = 0; lane < 2; ++lane)
					position[lane] += offset[lane] * scale[lane];
				if (in.Type == 1) {
					for (size_t lane = 0; lane < 2; ++lane) {
						scale[lane] *= 2.f;
						position[lane] *= 1.f + n + a;
					}
					amplitude *= .5f;
				} else if (in.Type == 2) {
					for (auto &lane : scale)
						lane *= 2.f;
					amplitude *= .5f;
					if (!ExtraRandom2(c, {out, out}, in.Seed, offset)) return false;
					for (size_t lane = 0; lane < 2; ++lane) {
						if (scale[lane] == 0)
							return c.Fail(
								Status::UnsupportedExecution,
								"Extra Perlin Noisy offset divides by zero Scale",
								"scale"
							);
						position[lane] += offset[lane] / scale[lane];
						position[lane] *= 2.f + a;
					}
				} else if (in.Type != 3 && in.Type != 5) {
					for (size_t lane = 0; lane < 2; ++lane) {
						scale[lane] *= 2.f;
						position[lane] *= 2.f;
					}
					amplitude *= .5f;
				}
				if (!ExtraVectorFinite(c, "scale", position) || !ExtraVectorFinite(c, "scale", scale) ||
					(in.Type != 5 && !ExtraFinite(c, "iteration", amplitude)))
					return false;
			}
			return true;
		}
		bool
		ExtraPerlin(NodeContext &c, const ExtraInputs &in, Float2 st, Float2 scale, float a, float &out) {
			if (in.Type == 6) {
				if (in.Iteration <= 0) {
					out = 0;
					return true;
				}
				for (float lane : scale)
					if (lane == 0)
						return c.Fail(
							Status::UnsupportedExecution,
							"Extra Perlin Vine offset divides by zero Scale",
							"scale"
						);
				std::array<float, 4> values{};
				constexpr std::array<Float2, 4> directions{{{1, 0}, {0, 1}, {-1, 0}, {0, -1}}};
				for (size_t i = 0; i < 4; ++i)
					if (!ExtraOctaves(
							c,
							in,
							{st[0] - directions[i][0] / scale[0] * (1.f + a),
							 st[1] - directions[i][1] / scale[1] * (1.f + a)},
							scale,
							a,
							values[i]
						))
						return false;
				out = std::abs(values[0] - values[2]) + std::abs(values[1] - values[3]);
				return ExtraFinite(c, "parameter_a", out);
			}
			if (!ExtraOctaves(c, in, st, scale, a, out)) return false;
			out = ExtraMix(
				in.LevelOut[0], in.LevelOut[1], (out - in.LevelIn[0]) / (in.LevelIn[1] - in.LevelIn[0])
			);
			return ExtraFinite(c, "level_out", out);
		}
		bool ShadeExtra(NodeContext &c, const ExtraInputs &in, uint32_t x, uint32_t y, Rgba &pixel) {
			if (!in.Covered || float(x) + .5f >= in.Dimension[0] || float(y) + .5f >= in.Dimension[1]) {
				pixel = {0, 0, 0, 0};
				return true;
			}
			const float u = (float(x) + .5f) / in.Dimension[0], v = (float(y) + .5f) / in.Dimension[1];
			Float2 uv{u, v}, scale{1, 1}, st{};
			float alpha = 1, a = 0;
			if (in.Uv) {
				const auto sample = SampleNearest(*in.Uv, u, v);
				alpha = float(sample[3]);
				if (in.Iteration > 0)
					uv = {
						ExtraMix(u, float(sample[0]), in.UvMix), ExtraMix(v, 1.f - float(sample[1]), in.UvMix)
					};
			}
			if (!ExtraFinite(c, "uv_map", alpha)) return false;
			if (in.Iteration > 0) {
				if (!ExtraVectorFinite(c, "uv_map", uv) || !ExtraParameters(c, in, u, v, scale, a))
					return false;
				const float angle = in.Rotation * .017453292519943295f;
				if (!ExtraFinite(c, "rotation", angle)) return false;
				const Float2 delta{
					uv[0] - in.Position[0] / in.Dimension[0],
					uv[1] * (in.Dimension[1] / in.Dimension[0]) - in.Position[1] / in.Dimension[1]
				};
				const float cosine = std::cos(angle), sine = std::sin(angle);
				st = {
					(delta[0] * cosine - delta[1] * sine) * scale[0],
					(delta[0] * sine + delta[1] * cosine) * scale[1]
				};
				if (!ExtraVectorFinite(c, "position", st)) return false;
			}
			ShaderGradientRgb3 color{};
			constexpr std::array<Float2, 3> offsets{{{0, 0}, {1.7227f, 4.55529f}, {6.9950f, 6.82063f}}};
			for (size_t channel = 0; channel < (in.Mode == 0 ? 1u : 3u); ++channel) {
				float value = 0;
				if (!ExtraPerlin(
						c, in, {st[0] + offsets[channel][0], st[1] + offsets[channel][1]}, scale, a, value
					))
					return false;
				color[channel] = in.Mode == 0 ? value
											  : in.Colors[channel][0] +
													value * (in.Colors[channel][1] - in.Colors[channel][0]);
			}
			if (in.Mode == 0)
				color = {color[0], color[0], color[0]};
			else if (in.Mode == 2)
				color = ShaderGradientRgb(color);
			for (float lane : color)
				if (!ExtraFinite(c, "color_mode", lane)) return false;
			pixel = {color[0], color[1], color[2], alpha};
			return true;
		}

		bool QuoteExtra(NodeContext &c, const ExtraInputs &in, uint64_t &work) {
			const bool mask =
				in.Mask || std::any_of(c.ImageArrays.begin(), c.ImageArrays.end(), [](const auto &input) {
					return input.first == "mask" && input.second && !input.second->Images.empty();
				});
			if (!source2d::ComplexBatchAdmission(
					c, 1, "surface_out", "surface_out", 1 + size_t(mask) + size_t(c.NoiseFieldRequested)
				))
				return false;
			const uint64_t pixels = uint64_t(in.Canvas.Width) * in.Canvas.Height;
			if (work > EXTRA_WORK_LIMIT || pixels > (EXTRA_WORK_LIMIT - work) / EXTRA_BASE_WORK)
				return c.Fail(
					Status::LimitExceeded, "Extra Perlin complete batch exceeds work limit", "surface_out"
				);
			uint64_t smooth = 0;
			if (in.Covered && in.Iteration > 0 && in.Type == 4)
				for (uint32_t y = 0; y < in.Canvas.Height; ++y)
					for (uint32_t x = 0; x < in.Canvas.Width; ++x) {
						if (float(x) + .5f >= in.Dimension[0] || float(y) + .5f >= in.Dimension[1]) continue;
						Float2 scale{};
						float a = 0;
						if (!ExtraParameters(
								c,
								in,
								(float(x) + .5f) / in.Dimension[0],
								(float(y) + .5f) / in.Dimension[1],
								scale,
								a
							))
							return false;
						const float axes = std::floor(2.f + a * 4.f),
									outer = std::floor(
										1.f +
										a * 5.f * float(std::max(in.Iteration - 1, 0)) / float(in.Iteration)
									);
						if (!ExtraFinite(c, "parameter_a", axes) || !ExtraFinite(c, "parameter_a", outer))
							return false;
						if (axes > float(EXTRA_WORK_LIMIT / 512) || outer > float(EXTRA_WORK_LIMIT / 512))
							return c.Fail(
								Status::LimitExceeded,
								"Extra Perlin Blocky smooth loops exceed work limit",
								"parameter_a"
							);
						smooth = std::max(
							smooth, 2 * uint64_t(std::max(0.f, axes)) + uint64_t(std::max(1.f, outer))
						);
					}
			const uint64_t channels = in.Mode == 0 ? 1 : 3,
						   iterations = in.Covered ? uint64_t(std::max(in.Iteration, 0)) : 0,
						   multiplier = in.Type == 3   ? 3
										: in.Type == 6 ? 4
													   : 1;
			const uint64_t perPixel =
				EXTRA_BASE_WORK + iterations * channels * multiplier * (EXTRA_OCTAVE_WORK + smooth * 512);
			if (perPixel > EXTRA_WORK_LIMIT || pixels > (EXTRA_WORK_LIMIT - work) / perPixel)
				return c.Fail(
					Status::LimitExceeded, "Extra Perlin complete batch exceeds work limit", "surface_out"
				);
			work += pixels * perPixel;
			// grug prove all selected source arithmetic before outputs or observer calls.
			for (uint32_t y = 0; y < in.Canvas.Height; ++y)
				for (uint32_t x = 0; x < in.Canvas.Width; ++x) {
					Rgba pixel{};
					if (!ShadeExtra(c, in, x, y, pixel)) return false;
				}
			return true;
		}

		bool DrawExtra(NodeContext &c) {
			ENGINE_PROFILE("imagegraph.source.perlin_extra");
			ExtraInputs in;
			uint64_t work = 0;
			if (!PrepareExtra(c, in) || !QuoteExtra(c, in, work)) return false;
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
					if (!ShadeExtra(c, in, x, y, pixel)) return false;
					if (!WritePixel(*out, x, y, pixel))
						return c.Fail(
							Status::InvalidValue,
							"Extra Perlin sample exceeds output storage range",
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
						if (!ExtraFinite(c, "mask", brightness) || !WritePixel(scratch, x, y, pixel))
							return c.Fail(
								Status::InvalidValue, "Extra Perlin mask exceeds finite storage range", "mask"
							);
					}
				}
			if (in.Mask)
				for (uint32_t y = 0; y < in.Canvas.Height; ++y)
					for (uint32_t x = 0; x < in.Canvas.Width; ++x)
						if (!WritePixel(*out, x, y, ReadPixel(scratch, x, y)))
							return c.Fail(
								Status::InvalidValue,
								"Extra Perlin mask copy exceeds storage range",
								"surface_out"
							);
			return c.FailureCode == Status::Ok;
		}
	}
	bool AdmitSourcePerlinExtra(NodeContext &context, uint64_t &work) {
		ExtraInputs in;
		return PrepareExtra(context, in) && QuoteExtra(context, in, work);
	}
	std::span<const ExecutorEntry> SourcePerlinExtraExecutors() {
		static constexpr ExecutorEntry entries[]{{"pc.perlin_extra", DrawExtra, true}};
		return entries;
	}
}
