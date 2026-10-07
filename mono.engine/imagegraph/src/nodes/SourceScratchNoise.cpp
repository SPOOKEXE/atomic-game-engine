#include "SourceScratchNoise.hpp"

#include "../SourceMappedInputs.hpp"
#include "Families.hpp"
#include "Source2DComplexGenerator.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string_view>
#include <variant>
#include <vector>
// Scratch noise by Peace
// Copyright (c) 2026 @Peace @lumiey
// https://www.shadertoy.com/view/7X2SWW
namespace engine::imagegraph::detail {
	namespace {
		namespace source_scratch_noise {
			constexpr uint64_t WORK_LIMIT = 64000000, BASE_WORK = 512, COVERED_WORK = 4096,
							   OCTAVE_WORK = 8192;
			using Float2 = std::array<float, 2>;
			struct ScratchInputs {
				source2d::ComplexCanvas Canvas;
				SurfaceFormat Format = SurfaceFormat::RGBA8Unorm;
				Float2 Dimension{}, Position{}, Scale{.5f, .5f}, Thickness{}, Wavyness{0, .5f},
					Softness{0, 3}, OctaveShift{10, 10};
				const Image *Uv = nullptr, *Mask = nullptr, *ThicknessMap = nullptr, *WavynessMap = nullptr,
							*SoftnessMap = nullptr;
				float Seed = 0, Rotation = 0, UvMix = 1, OctaveRotation = 30, OctaveScale = 1;
				int32_t Octaves = 8;
				bool Covered = false;
			};
			bool ScratchFinite(NodeContext &c, std::string_view port, float value) {
				return std::isfinite(value) ||
					   c.Fail(
						   Status::InvalidValue, "Scratch Noise shader arithmetic exceeds finite range", port
					   );
			}
			bool ScratchFloat(NodeContext &c, std::string_view port, double value, float &out) {
				if (!std::isfinite(value) || std::abs(value) > double(std::numeric_limits<float>::max()))
					return c.Fail(
						Status::InvalidValue, "Scratch Noise value exceeds shader float range", port
					);
				out = float(value);
				return true;
			}
			bool ScratchPair(NodeContext &c, std::string_view port, Vector2 value, Float2 &out) {
				return ScratchFloat(c, port, value.X, out[0]) && ScratchFloat(c, port, value.Y, out[1]);
			}
			bool ScratchSurface(NodeContext &c, std::string_view port, const Image *image) {
				if (const auto *value = c.Find(port); value && std::holds_alternative<AtlasValue>(*value))
					return c.Fail(
						Status::UnsupportedExecution, "Scratch Noise raw sampler binding rejects Atlas", port
					);
				return !image ||
					   ValidSurfaceLayout(*image, Limits::MaximumDimension, Limits::MaximumEvaluationBytes) ||
					   c.Fail(Status::InvalidValue, "Scratch Noise sampler layout is invalid", port);
			}
			bool ScratchMappedControl(
				NodeContext &c,
				std::string_view port,
				std::string_view mapPort,
				Float2 &range,
				const Image *&map
			) {
				Vector2 value;
				if (!ReadSourceMappedRange(c, port, value)) return false;
				map = c.Input(mapPort);
				return ScratchSurface(c, mapPort, map) && ScratchPair(c, port, value, range);
			}

			bool PrepareScratch(NodeContext &c, ScratchInputs &in) {
				in.Uv = c.Input("uv_map");
				in.Mask = c.Input("mask");
				if (!ScratchSurface(c, "uv_map", in.Uv) || !ScratchSurface(c, "mask", in.Mask) ||
					!source2d::ResolveGeneratorDimensions(c, in.Mask, in.Canvas.Width, in.Canvas.Height))
					return false;
				if (in.Canvas.Width > c.Request.MaximumImageDimension ||
					in.Canvas.Height > c.Request.MaximumImageDimension)
					return c.Fail(
						Status::LimitExceeded, "Scratch Noise exceeds request dimensions", "dimension"
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
				if (!ScratchPair(c, "dimension", in.Canvas.Raw, in.Dimension)) return false;
				const auto format = ResolveProcessorSurfaceFormat(c, nullptr);
				if (!format) return false;
				in.Format = *format;
				in.Covered = in.Dimension[0] > .5f && in.Dimension[1] > .5f;
				if (!in.Covered) return c.FailureCode == Status::Ok;
				const auto *octaveValue = c.Find("octaves");
				double octaves = source2d::GeneratorRoundHalfEven(c.Scalar("octaves", 8));
				if (octaveValue)
					if (const auto *integer = std::get_if<int64_t>(octaveValue)) octaves = double(*integer);
				if (!std::isfinite(octaves) || octaves < double(std::numeric_limits<int32_t>::min()) ||
					octaves > double(std::numeric_limits<int32_t>::max()))
					return c.Fail(
						Status::UnsupportedExecution,
						"Scratch Noise Octaves exceeds shader integer range",
						"octaves"
					);
				in.Octaves = int32_t(octaves);
				if (in.Octaves <= 0) return c.FailureCode == Status::Ok;
				for (const auto port : std::array<std::string_view, 3>{"thickness", "wavyness", "softness"})
					if (!SourceRangeMapped(c, port))
						return c.Fail(
							Status::UnsupportedExecution,
							"Scratch Noise unmapped control needs unresolved source uniform or sampler state",
							port
						);
				if (!c.Find("seed"))
					return c.Fail(
						Status::UnsupportedExecution, "Scratch Noise requires resolved source Seed", "seed"
					);
				Vector2 position;
				if (!source2d::ReferenceVector(c, "position", in.Canvas.Raw, position) ||
					!ScratchPair(c, "position", position, in.Position) ||
					!ScratchPair(c, "scale", c.Vec2("scale", {.5, .5}), in.Scale) ||
					!ScratchFloat(c, "seed", c.Scalar("seed"), in.Seed) ||
					!ScratchFloat(c, "rotation", c.Scalar("rotation"), in.Rotation))
					return false;
				if (in.Scale[0] == 0 || in.Scale[1] == 0)
					return c.Fail(
						Status::UnsupportedExecution, "Scratch Noise transform divides by zero Scale", "scale"
					);
				if (in.Uv && !ScratchFloat(c, "uv_mix", c.Scalar("uv_mix", 1), in.UvMix)) return false;
				if (in.Octaves > 1 &&
					(!ScratchPair(c, "octave_shift", c.Vec2("octave_shift", {10, 10}), in.OctaveShift) ||
					 !ScratchFloat(
						 c, "octave_rotation", c.Scalar("octave_rotation", 30), in.OctaveRotation
					 ) ||
					 !ScratchFloat(c, "octave_scale", c.Scalar("octave_scale", 1), in.OctaveScale)))
					return false;
				return ScratchMappedControl(c, "thickness", "thickness_map", in.Thickness, in.ThicknessMap) &&
					   ScratchMappedControl(c, "wavyness", "wavyness_map", in.Wavyness, in.WavynessMap) &&
					   ScratchMappedControl(c, "softness", "softness_map", in.Softness, in.SoftnessMap);
			}
			float ScratchFract(float value) {
				return value - std::floor(value);
			}
			float ScratchMix(float low, float high, float weight) {
				return low * (1.f - weight) + high * weight;
			}
			bool ScratchVectorFinite(NodeContext &c, std::string_view port, Float2 value) {
				return ScratchFinite(c, port, value[0]) && ScratchFinite(c, port, value[1]);
			}
			float ScratchMapped(Float2 range, const Image *map, float u, float v) {
				if (!map) return range[0];
				const auto pixel = SampleNearest(*map, u, v);
				return ScratchMix(
					range[0], range[1], (float(pixel[0]) + float(pixel[1]) + float(pixel[2])) / 3.f
				);
			}
			bool ScratchPosition(
				NodeContext &c, const ScratchInputs &in, uint32_t x, uint32_t y, Float2 &position
			) {
				const float u = (float(x) + .5f) / in.Dimension[0], v = (float(y) + .5f) / in.Dimension[1];
				Float2 uv{u, v};
				if (in.Uv) {
					const auto pixel = SampleNearest(*in.Uv, u, v);
					uv = {
						ScratchMix(u, float(pixel[0]), in.UvMix),
						ScratchMix(v, 1.f - float(pixel[1]), in.UvMix)
					};
				}
				if (!ScratchVectorFinite(c, "uv_map", uv)) return false;
				const Float2 delta{
					uv[0] - in.Position[0] / in.Dimension[0], uv[1] - in.Position[1] / in.Dimension[1]
				};
				const float angle = in.Rotation * .017453292519943295f;
				if (!ScratchFinite(c, "rotation", angle)) return false;
				const float cosine = std::cos(angle), sine = std::sin(angle);
				position = {
					(delta[0] * cosine - delta[1] * sine) / in.Scale[0],
					(delta[0] * sine + delta[1] * cosine) / in.Scale[1]
				};
				return ScratchVectorFinite(c, "position", position);
			}
			bool ScratchCandidate(
				NodeContext &c,
				Float2 position,
				float width,
				float thickness,
				float wavyness,
				float seed,
				float &out
			) {
				Float2 cell{std::floor(position[0]), std::floor(position[1])};
				const Float2 angles{
					(cell[0] * 127.1324f + cell[1] * 311.7874f) * (152.6178612f + seed / 10000.f),
					(cell[0] * 269.8355f + cell[1] * 183.3961f) * (437.5453123f + seed / 10000.f)
				};
				if (!ScratchVectorFinite(c, "seed", angles)) return false;
				const Float2 hash{
					ScratchFract(std::sin(angles[0]) * 43758.5453f) * 3104.f,
					ScratchFract(std::sin(angles[1]) * 43758.5453f) * 554.f
				};
				Float2 p{(position[0] - cell[0]) * 2.f - 1.f, (position[1] - cell[1]) * 2.f - 1.f};
				const float angle = hash[0] + hash[1], cosine = std::cos(angle), sine = std::sin(angle),
							shift = std::sin(hash[0] - hash[1]);
				p = {p[0] * cosine - p[1] * sine + shift, p[1] * cosine + p[0] * sine + shift};
				const float x = std::abs(p[0] - std::cos(hash[0] + p[1] * 1.57f) * wavyness);
				const float low = thickness + width, high = thickness - width;
				if (!ScratchFinite(c, "thickness", low) || !ScratchFinite(c, "thickness", high) ||
					!ScratchFinite(c, "wavyness", x))
					return false;
				if (low == high)
					return c.Fail(
						Status::UnsupportedExecution,
						"Scratch Noise smoothstep edges are equal",
						width == 0 ? "softness" : "thickness"
					);
				if (!ScratchFinite(c, "softness", high - low)) return false;
				// grug native equation keeps pinned reversed-edge clamped polynomial.
				const float numerator = x - low;
				if (!ScratchFinite(c, "thickness", numerator)) return false;
				const float quotient = numerator / (high - low);
				if (!ScratchFinite(c, "thickness", quotient)) return false;
				const float t = std::clamp(quotient, 0.f, 1.f);
				if (!ScratchFinite(c, "thickness", t)) return false;
				out = t * t * (3.f - 2.f * t) * (p[1] * .5f + .5f);
				return ScratchFinite(c, "wavyness", out);
			}
			bool ShadeScratch(NodeContext &c, const ScratchInputs &in, uint32_t x, uint32_t y, Rgba &pixel) {
				pixel = {};
				if (!in.Covered || float(x) + .5f >= in.Dimension[0] || float(y) + .5f >= in.Dimension[1])
					return true;
				const float u = (float(x) + .5f) / in.Dimension[0], v = (float(y) + .5f) / in.Dimension[1];
				float alpha = 1;
				if (in.Uv) {
					const auto sample = SampleNearest(*in.Uv, u, v);
					alpha = float(sample[3]);
				}
				if (!ScratchFinite(c, "uv_map", alpha)) return false;
				float value = 0;
				if (in.Octaves > 0) {
					const float thickness = ScratchMapped(in.Thickness, in.ThicknessMap, u, v),
								wavyness = ScratchMapped(in.Wavyness, in.WavynessMap, u, v),
								softness = ScratchMapped(in.Softness, in.SoftnessMap, u, v);
					if (!ScratchFinite(c, "thickness", thickness) ||
						!ScratchFinite(c, "wavyness", wavyness) || !ScratchFinite(c, "softness", softness))
						return false;
					std::array<Float2, 4> quad{};
					const uint32_t left = x & ~uint32_t(1), top = y & ~uint32_t(1);
					// grug native fine quad includes helper lanes beyond sprite and canvas edges.
					for (uint32_t row = 0; row < 2; ++row)
						for (uint32_t column = 0; column < 2; ++column)
							if (!ScratchPosition(c, in, left + column, top + row, quad[row * 2 + column]))
								return false;
					const uint32_t row = y & 1, column = x & 1;
					Float2 widthVector{};
					for (size_t lane = 0; lane < 2; ++lane)
						widthVector[lane] = std::abs(quad[row * 2 + 1][lane] - quad[row * 2][lane]) +
											std::abs(quad[2 + column][lane] - quad[column][lane]);
					if (!ScratchVectorFinite(c, "softness", widthVector)) return false;
					float width =
						std::sqrt(widthVector[0] * widthVector[0] + widthVector[1] * widthVector[1]) *
						softness;
					if (!ScratchFinite(c, "softness", width)) return false;
					Float2 position = quad[row * 2 + column];
					const float angle = in.OctaveRotation * .017453292519943295f, cosine = std::cos(angle),
								sine = std::sin(angle);
					for (int32_t octave = 0; octave < in.Octaves; ++octave) {
						float candidate = 0;
						if (!ScratchCandidate(c, position, width, thickness, wavyness, in.Seed, candidate))
							return false;
						value = std::max(value, candidate);
						if (octave + 1 < in.Octaves) {
							position = {
								position[0] * cosine - position[1] * sine - in.OctaveShift[0],
								position[0] * sine + position[1] * cosine - in.OctaveShift[1]
							};
							width *= in.OctaveScale;
							if (!ScratchVectorFinite(c, "octave_shift", position) ||
								!ScratchFinite(c, "octave_scale", width))
								return false;
						}
					}
				}
				pixel = {value, value, value, alpha};
				return true;
			}
			bool QuoteScratch(NodeContext &c, const ScratchInputs &in, uint64_t &work) {
				const bool mask =
					in.Mask || std::any_of(c.ImageArrays.begin(), c.ImageArrays.end(), [](const auto &v) {
						return v.first == "mask" && v.second && !v.second->Images.empty();
					});
				if (!source2d::ComplexBatchAdmission(
						c, 1, "surface_out", "surface_out", 1 + size_t(mask) + size_t(c.NoiseFieldRequested)
					))
					return false;
				const uint64_t pixels = uint64_t(in.Canvas.Width) * in.Canvas.Height,
							   cost = BASE_WORK +
									  (in.Covered
										   ? COVERED_WORK + uint64_t(std::max(in.Octaves, 0)) * OCTAVE_WORK
										   : 0);
				if (work > WORK_LIMIT || pixels > (WORK_LIMIT - work) / cost)
					return c.Fail(
						Status::LimitExceeded,
						"Scratch Noise complete batch exceeds CPU work limit",
						"surface_out"
					);
				work += pixels * cost;
				const auto format = *DescribeSurfaceFormat(in.Format);
				for (uint32_t y = 0; y < in.Canvas.Height; ++y)
					for (uint32_t x = 0; x < in.Canvas.Width; ++x) {
						Rgba pixel{};
						if (!ShadeScratch(c, in, x, y, pixel)) return false;
						for (size_t lane = 0; lane < format.Channels; ++lane)
							if (format.FloatingPoint && format.BitsPerChannel == 16 &&
								std::abs(pixel[lane]) > 65504.)
								return c.Fail(
									Status::InvalidValue,
									"Scratch Noise exceeds half-float storage range",
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
							if (!ScratchFinite(c, "mask", brightness) ||
								!ScratchFinite(c, "mask", storedAlpha * brightness))
								return false;
						}
					}
				return true;
			}
			bool DrawScratch(NodeContext &c) {
				ENGINE_PROFILE("imagegraph.source.scratch_noise");
				ScratchInputs in;
				uint64_t work = 0;
				if (!PrepareScratch(c, in) || !QuoteScratch(c, in, work)) return false;
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
						if (!ShadeScratch(c, in, x, y, pixel)) return false;
						if (!WritePixel(*out, x, y, pixel))
							return c.Fail(
								Status::InvalidValue,
								"Scratch Noise sample exceeds output storage range",
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
							if (!ScratchFinite(c, "mask", brightness) ||
								!ScratchFinite(c, "mask", float(pixel[3])) ||
								!WritePixel(scratch, x, y, pixel))
								return c.Fail(
									Status::InvalidValue,
									"Scratch Noise mask exceeds finite storage range",
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
									"Scratch Noise mask copy exceeds storage range",
									"surface_out"
								);
				return c.FailureCode == Status::Ok;
			}
		}
	}
	bool AdmitSourceScratchNoise(NodeContext &c, uint64_t &work) {
		source_scratch_noise::ScratchInputs in;
		return source_scratch_noise::PrepareScratch(c, in) && source_scratch_noise::QuoteScratch(c, in, work);
	}
	std::span<const ExecutorEntry> SourceScratchNoiseExecutors() {
		static constexpr ExecutorEntry entries[]{
			{"pc.noise_scratch", source_scratch_noise::DrawScratch, true}
		};
		return entries;
	}
}
