#include "SourceWaveletNoise.hpp"

#include "../SourceMappedInputs.hpp"
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
// "Wavelet Noise"
// The MIT License
// Copyright © 2020 Martijn Steinrucken
// Permission is hereby granted, free of charge, to any person obtaining a copy of this software and
// associated documentation files (the "Software"), to deal in the Software without restriction, including
// without limitation the rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is furnished to do so, subject to the
// following conditions: The above copyright notice and this permission notice shall be included in all copies
// or substantial portions of the Software. THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
// EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A
// PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR
// ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE. Email:
// countfrolic@gmail.com Twitter: @The_ArtOfCode YouTube: youtube.com/TheArtOfCodeIsCool Facebook:
// https://www.facebook.com/groups/theartofcode/

namespace engine::imagegraph::detail {
	namespace {
		namespace source_wavelet_noise {
			constexpr uint64_t WORK_LIMIT = 64000000, BASE_WORK = 512, COVERED_WORK = 8192;
			using Float2 = std::array<float, 2>;
			struct WaveInputs {
				source2d::ComplexCanvas Canvas;
				SurfaceFormat Format = SurfaceFormat::RGBA8Unorm;
				Float2 Dimension{}, Position{}, Scale{4, 4}, Progress{}, Detail{0, 1.24f}, LevelIn{0, 1},
					LevelOut{0, 1};
				const Image *Uv = nullptr, *Mask = nullptr, *ScaleMap = nullptr, *ProgressMap = nullptr,
							*DetailMap = nullptr;
				float Seed = 0, Rotation = 0, UvMix = 1;
				bool Covered = false;
			};
			bool WaveFinite(NodeContext &c, std::string_view port, float value) {
				return std::isfinite(value) ||
					   c.Fail(
						   Status::InvalidValue, "Wavelet Noise shader arithmetic exceeds finite range", port
					   );
			}
			bool WaveFloat(NodeContext &c, std::string_view port, double value, float &out) {
				if (!std::isfinite(value) || std::abs(value) > double(std::numeric_limits<float>::max()))
					return c.Fail(
						Status::InvalidValue, "Wavelet Noise value exceeds shader float range", port
					);
				out = float(value);
				return true;
			}
			bool WavePair(NodeContext &c, std::string_view port, Vector2 value, Float2 &out) {
				return WaveFloat(c, port, value.X, out[0]) && WaveFloat(c, port, value.Y, out[1]);
			}
			bool WaveSurface(NodeContext &c, std::string_view port, const Image *image) {
				if (const auto *value = c.Find(port); value && std::holds_alternative<AtlasValue>(*value))
					return c.Fail(
						Status::UnsupportedExecution, "Wavelet Noise raw sampler binding rejects Atlas", port
					);
				return !image ||
					   ValidSurfaceLayout(*image, Limits::MaximumDimension, Limits::MaximumEvaluationBytes) ||
					   c.Fail(Status::InvalidValue, "Wavelet Noise sampler layout is invalid", port);
			}
			bool WaveMappedControl(
				NodeContext &c,
				std::string_view port,
				std::string_view mapPort,
				Float2 &range,
				const Image *&map
			) {
				Vector2 value;
				if (!ReadSourceMappedRange(c, port, value)) return false;
				map = c.Input(mapPort);
				return WaveSurface(c, mapPort, map) && WavePair(c, port, value, range);
			}

			bool PrepareWave(NodeContext &c, WaveInputs &in) {
				in.Uv = c.Input("uv_map");
				in.Mask = c.Input("mask");
				if (!WaveSurface(c, "uv_map", in.Uv) || !WaveSurface(c, "mask", in.Mask) ||
					!source2d::ResolveGeneratorDimensions(c, in.Mask, in.Canvas.Width, in.Canvas.Height))
					return false;
				if (in.Canvas.Width > c.Request.MaximumImageDimension ||
					in.Canvas.Height > c.Request.MaximumImageDimension)
					return c.Fail(
						Status::LimitExceeded, "Wavelet Noise exceeds request dimensions", "dimension"
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
				if (!WavePair(c, "dimension", in.Canvas.Raw, in.Dimension)) return false;
				const auto format = ResolveProcessorSurfaceFormat(c, nullptr);
				if (!format) return false;
				in.Format = *format;
				in.Covered = in.Dimension[0] > .5f && in.Dimension[1] > .5f;
				if (!in.Covered) return c.FailureCode == Status::Ok;
				for (const auto port : std::array<std::string_view, 3>{"scale", "progress", "detail"})
					if (!SourceRangeMapped(c, port))
						return c.Fail(
							Status::UnsupportedExecution,
							"Wavelet Noise unmapped control needs unresolved source uniform or sampler state",
							port
						);
				if (!c.Find("seed"))
					return c.Fail(
						Status::UnsupportedExecution, "Wavelet Noise requires resolved source Seed", "seed"
					);
				Vector2 position;
				if (!source2d::ReferenceVector(c, "position", in.Canvas.Raw, position) ||
					!WavePair(c, "position", position, in.Position) ||
					!WaveFloat(c, "seed", c.Scalar("seed"), in.Seed) ||
					!WaveFloat(c, "rotation", c.Scalar("rotation"), in.Rotation) ||
					!WavePair(c, "level_in", c.Vec2("level_in", {0, 1}), in.LevelIn) ||
					!WavePair(c, "level_out", c.Vec2("level_out", {0, 1}), in.LevelOut))
					return false;
				if (in.LevelIn[0] == in.LevelIn[1])
					return c.Fail(
						Status::UnsupportedExecution, "Wavelet Noise equal levels divide by zero", "level_in"
					);
				if (!WaveFinite(c, "level_in", in.LevelIn[1] - in.LevelIn[0])) return false;
				if (in.Uv && !WaveFloat(c, "uv_mix", c.Scalar("uv_mix", 1), in.UvMix)) return false;
				const auto *original = source2d::GeneratorOriginal(c, "scale");
				const auto *array = original ? std::get_if<ArrayValue>(original) : nullptr;
				if (array && (!array->Nested.empty() || !array->Items.empty() ||
							  std::any_of(array->Elements.begin(), array->Elements.end(), [](const auto &v) {
								  return !std::holds_alternative<double>(v) &&
										 !std::holds_alternative<int64_t>(v);
							  })))
					return c.Fail(
						Status::UnsupportedExecution,
						"Wavelet Noise nested mapped Scale is not a defined vec2 upload",
						"scale"
					);
				return WaveMappedControl(c, "scale", "scale_map", in.Scale, in.ScaleMap) &&
					   WaveMappedControl(c, "progress", "progress_map", in.Progress, in.ProgressMap) &&
					   WaveMappedControl(c, "detail", "detail_map", in.Detail, in.DetailMap);
			}
			float WaveFract(float value) {
				return value - std::floor(value);
			}
			float WaveMix(float low, float high, float weight) {
				return low * (1.f - weight) + high * weight;
			}
			bool WaveVectorFinite(NodeContext &c, std::string_view port, Float2 value) {
				return WaveFinite(c, port, value[0]) && WaveFinite(c, port, value[1]);
			}
			float WaveMapped(Float2 range, const Image *map, float u, float v) {
				if (!map) return range[0];
				const auto pixel = SampleNearest(*map, u, v);
				return WaveMix(
					range[0], range[1], (float(pixel[0]) + float(pixel[1]) + float(pixel[2])) / 3.f
				);
			}
			bool
			WaveNoise(NodeContext &c, Float2 position, float progress, float detail, float seed, float &out) {
				float value = 0, scale = 1, weight = 0;
				for (size_t layer = 0; layer < 4; ++layer) {
					if (scale == 0)
						return c.Fail(
							Status::UnsupportedExecution,
							"Wavelet Noise layer scale divides by zero",
							"detail"
						);
					Float2 q{position[0] * scale, position[1] * scale};
					if (!WaveVectorFinite(c, "scale", q)) return false;
					Float2 g{std::floor(q[0]) * (123.34f + seed), std::floor(q[1]) * (233.53f + seed)};
					if (!WaveVectorFinite(c, "seed", g)) return false;
					g = {WaveFract(g[0]), WaveFract(g[1])};
					const float dot = g[0] * (g[0] + 23.234f) + g[1] * (g[1] + 23.234f);
					g[0] += dot;
					g[1] += dot;
					const float sum = g[0] + g[1], mod = sum - 2.f * std::floor(sum / 2.f);
					const float angle = WaveFract(g[0] * g[1]) * .0001f + progress * (mod - 1.f);
					if (!WaveFinite(c, "progress", angle)) return false;
					const float cosine = std::cos(angle), sine = std::sin(angle), x = WaveFract(q[0]) - .5f,
								y = WaveFract(q[1]) - .5f;
					q = {x * cosine - y * sine, x * sine + y * cosine};
					const float radius = q[0] * q[0] + q[1] * q[1];
					// grug pinned reversed smoothstep is this native clamped polynomial.
					const float t = std::clamp((radius - .25f) / (0.f - .25f), 0.f, 1.f),
								smoothing = t * t * (3.f - 2.f * t);
					const float phase = q[0] * 10.f + progress;
					if (!WaveFinite(c, "progress", phase)) return false;
					value += std::sin(phase) * smoothing / scale;
					weight += 1.f / scale;
					if (!WaveFinite(c, "detail", value) || !WaveFinite(c, "detail", weight)) return false;
					// grug final postupdate feeds no layer. do not reject unused overflow.
					if (layer < 3) {
						position = {
							position[0] * .54f - position[1] * .84f + float(layer),
							position[0] * .84f + position[1] * .54f + float(layer)
						};
						scale *= detail;
						if (!WaveVectorFinite(c, "scale", position) || !WaveFinite(c, "detail", scale))
							return false;
					}
				}
				if (weight == 0)
					return c.Fail(
						Status::UnsupportedExecution,
						"Wavelet Noise accumulated layer weight divides by zero",
						"detail"
					);
				out = value / weight;
				return WaveFinite(c, "detail", out);
			}
			bool ShadeWave(NodeContext &c, const WaveInputs &in, uint32_t x, uint32_t y, Rgba &pixel) {
				pixel = {};
				if (!in.Covered || float(x) + .5f >= in.Dimension[0] || float(y) + .5f >= in.Dimension[1])
					return true;
				const float u = (float(x) + .5f) / in.Dimension[0], v = (float(y) + .5f) / in.Dimension[1];
				Float2 uv{u, v}, scale = in.Scale;
				const float progress = WaveMapped(in.Progress, in.ProgressMap, u, v),
							detail = WaveMapped(in.Detail, in.DetailMap, u, v);
				if (in.ScaleMap) {
					const float mapped = WaveMapped(in.Scale, in.ScaleMap, u, v);
					scale = {mapped, mapped};
				}
				if (!WaveFinite(c, "progress", progress) || !WaveFinite(c, "detail", detail) ||
					!WaveVectorFinite(c, "scale", scale))
					return false;
				float alpha = 1;
				if (in.Uv) {
					const auto sample = SampleNearest(*in.Uv, u, v);
					alpha = float(sample[3]);
					uv = {
						WaveMix(u, float(sample[0]), in.UvMix), WaveMix(v, 1.f - float(sample[1]), in.UvMix)
					};
				}
				if (!WaveFinite(c, "uv_map", alpha) || !WaveVectorFinite(c, "uv_map", uv)) return false;
				const float angle = in.Rotation * .017453292519943295f,
							aspect = in.Dimension[1] / in.Dimension[0];
				if (!WaveFinite(c, "rotation", angle) || !WaveFinite(c, "dimension", aspect)) return false;
				const Float2 delta{
					uv[0] - in.Position[0] / in.Dimension[0],
					uv[1] * aspect - in.Position[1] / in.Dimension[1]
				};
				const float cosine = std::cos(angle), sine = std::sin(angle);
				Float2 position{
					(delta[0] * cosine - delta[1] * sine) * scale[0] / 16.f * 5.f,
					(delta[0] * sine + delta[1] * cosine) * scale[1] / 16.f * 5.f
				};
				if (!WaveVectorFinite(c, "position", position)) return false;
				const float z = 2.9864f + progress;
				if (!WaveFinite(c, "progress", z)) return false;
				float value = 0;
				if (!WaveNoise(c, position, z, detail, in.Seed, value)) return false;
				value = value * .5f + .5f;
				value = WaveMix(
					in.LevelOut[0], in.LevelOut[1], (value - in.LevelIn[0]) / (in.LevelIn[1] - in.LevelIn[0])
				);
				if (!WaveFinite(c, "level_out", value)) return false;
				pixel = {value, value, value, alpha};
				return true;
			}
			bool QuoteWave(NodeContext &c, const WaveInputs &in, uint64_t &work) {
				const bool mask =
					in.Mask || std::any_of(c.ImageArrays.begin(), c.ImageArrays.end(), [](const auto &v) {
						return v.first == "mask" && v.second && !v.second->Images.empty();
					});
				if (!source2d::ComplexBatchAdmission(
						c, 1, "surface_out", "surface_out", 1 + size_t(mask) + size_t(c.NoiseFieldRequested)
					))
					return false;
				const uint64_t pixels = uint64_t(in.Canvas.Width) * in.Canvas.Height,
							   cost = BASE_WORK + (in.Covered ? COVERED_WORK : 0);
				if (work > WORK_LIMIT || pixels > (WORK_LIMIT - work) / cost)
					return c.Fail(
						Status::LimitExceeded,
						"Wavelet Noise complete batch exceeds CPU work limit",
						"surface_out"
					);
				work += pixels * cost;
				const auto format = *DescribeSurfaceFormat(in.Format);
				for (uint32_t y = 0; y < in.Canvas.Height; ++y)
					for (uint32_t x = 0; x < in.Canvas.Width; ++x) {
						Rgba pixel{};
						if (!ShadeWave(c, in, x, y, pixel)) return false;
						for (size_t lane = 0; lane < format.Channels; ++lane)
							if (format.FloatingPoint && format.BitsPerChannel == 16 &&
								std::abs(pixel[lane]) > 65504.)
								return c.Fail(
									Status::InvalidValue,
									"Wavelet Noise exceeds half-float storage range",
									"surface_out"
								);
						if (in.Mask) {
							const auto mask = SampleNearest(
								*in.Mask,
								(float(x) + .5f) / in.Canvas.Width,
								(float(y) + .5f) / in.Canvas.Height
							);
							const float brightness =
								(float(mask[0]) + float(mask[1]) + float(mask[2])) / 3.f * float(mask[3]);
							float storedAlpha = format.Channels == 1 ? 1.f : float(pixel[3]);
							if (format.Channels != 1) {
								if (!format.FloatingPoint) {
									const float maximum = format.BitsPerChannel == 4 ? 15.f : 255.f;
									storedAlpha =
										std::round(std::clamp(storedAlpha, 0.f, 1.f) * maximum) / maximum;
								} else if (format.BitsPerChannel == 16)
									storedAlpha = DecodeHalf(EncodeHalf(storedAlpha));
							}
							if (!WaveFinite(c, "mask", brightness) ||
								!WaveFinite(c, "mask", storedAlpha * brightness))
								return false;
						}
					}
				return true;
			}
			bool DrawWave(NodeContext &c) {
				ENGINE_PROFILE("imagegraph.source.wavelet_noise");
				WaveInputs in;
				uint64_t work = 0;
				if (!PrepareWave(c, in) || !QuoteWave(c, in, work)) return false;
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
						if (!ShadeWave(c, in, x, y, pixel)) return false;
						if (!WritePixel(*out, x, y, pixel))
							return c.Fail(
								Status::InvalidValue,
								"Wavelet Noise sample exceeds output storage range",
								"surface_out"
							);
						if (in.Mask) {
							pixel = ReadPixel(*out, x, y);
							const auto mask = SampleNearest(
								*in.Mask,
								(float(x) + .5f) / in.Canvas.Width,
								(float(y) + .5f) / in.Canvas.Height
							);
							const float brightness =
								(float(mask[0]) + float(mask[1]) + float(mask[2])) / 3.f * float(mask[3]);
							pixel[3] = float(pixel[3]) * brightness;
							if (!WaveFinite(c, "mask", brightness) ||
								!WaveFinite(c, "mask", float(pixel[3])) || !WritePixel(scratch, x, y, pixel))
								return c.Fail(
									Status::InvalidValue,
									"Wavelet Noise mask exceeds finite storage range",
									"mask"
								);
						}
					}
				if (in.Mask)
					for (uint32_t y = 0; y < in.Canvas.Height; ++y)
						for (uint32_t x = 0; x < in.Canvas.Width; ++x)
							if (!WritePixel(*out, x, y, ReadPixel(scratch, x, y)))
								return c.Fail(
									Status::InvalidValue,
									"Wavelet Noise mask copy exceeds storage range",
									"surface_out"
								);
				return c.FailureCode == Status::Ok;
			}
		}
	}
	bool AdmitSourceWaveletNoise(NodeContext &c, uint64_t &work) {
		source_wavelet_noise::WaveInputs in;
		return source_wavelet_noise::PrepareWave(c, in) && source_wavelet_noise::QuoteWave(c, in, work);
	}
	std::span<const ExecutorEntry> SourceWaveletNoiseExecutors() {
		static constexpr ExecutorEntry entries[]{{"pc.wavelet_noise", source_wavelet_noise::DrawWave, true}};
		return entries;
	}
}
