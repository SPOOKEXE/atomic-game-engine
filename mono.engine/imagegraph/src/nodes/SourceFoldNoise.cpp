#include "SourceFoldNoise.hpp"

#include "Families.hpp"
#include "Source2DComplexGenerator.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string_view>
#include <variant>
#include <vector>
// Based on FabriceNeyret2 - plop 2 shader.
namespace engine::imagegraph::detail {
	namespace {
		namespace source_fold_noise {
			constexpr uint64_t WORK_LIMIT = 64000000;
			constexpr uint64_t BASE_WORK = 512;
			constexpr uint64_t COVERED_ITERATION_WORK = 4096;
			using Float2 = std::array<float, 2>;
			struct FoldInputs {
				source2d::ComplexCanvas Canvas;
				SurfaceFormat Format = SurfaceFormat::RGBA8Unorm;
				Float2 Dimension{}, Position{}, Scale{2, 2}, Density{3, 1}, LevelIn{0, 1}, LevelOut{0, 1};
				const Image *Uv = nullptr, *Mask = nullptr;
				float Rotation = 0, UvMix = 1, Stretch = 2, Amplitude = 1.3f;
				int32_t Iteration = 2, Mode = 0;
				bool Covered = false;
			};
			bool FoldFinite(NodeContext &c, std::string_view port, float value) {
				return std::isfinite(value) ||
					   c.Fail(
						   Status::InvalidValue, "Fold Noise shader arithmetic exceeds finite range", port
					   );
			}
			bool FoldFloat(NodeContext &c, std::string_view port, double value, float &out) {
				if (!std::isfinite(value) || std::abs(value) > double(std::numeric_limits<float>::max()))
					return c.Fail(Status::InvalidValue, "Fold Noise value exceeds shader float range", port);
				out = float(value);
				return true;
			}
			bool FoldPair(NodeContext &c, std::string_view port, Vector2 value, Float2 &out) {
				return FoldFloat(c, port, value.X, out[0]) && FoldFloat(c, port, value.Y, out[1]);
			}
			bool FoldSurface(NodeContext &c, std::string_view port, const Image *image) {
				if (const auto *value = c.Find(port); value && std::holds_alternative<AtlasValue>(*value))
					return c.Fail(
						Status::UnsupportedExecution, "Fold Noise raw sampler binding rejects Atlas", port
					);
				return !image ||
					   ValidSurfaceLayout(*image, Limits::MaximumDimension, Limits::MaximumEvaluationBytes) ||
					   c.Fail(Status::InvalidValue, "Fold Noise sampler layout is invalid", port);
			}

			bool PrepareFold(NodeContext &c, FoldInputs &in) {
				in.Uv = c.Input("uv_map");
				in.Mask = c.Input("mask");
				if (!FoldSurface(c, "uv_map", in.Uv) || !FoldSurface(c, "mask", in.Mask) ||
					!source2d::ResolveGeneratorDimensions(c, in.Mask, in.Canvas.Width, in.Canvas.Height))
					return false;
				if (in.Canvas.Width > c.Request.MaximumImageDimension ||
					in.Canvas.Height > c.Request.MaximumImageDimension)
					return c.Fail(
						Status::LimitExceeded, "Fold Noise exceeds request dimensions", "dimension"
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
				if (!FoldPair(c, "dimension", in.Canvas.Raw, in.Dimension)) return false;
				const auto format = ResolveProcessorSurfaceFormat(c, nullptr);
				if (!format) return false;
				in.Format = *format;
				in.Covered = in.Dimension[0] > .5f && in.Dimension[1] > .5f;
				if (!in.Covered) return c.FailureCode == Status::Ok;
				double iteration = source2d::GeneratorRoundHalfEven(c.Scalar("iteration", 2));
				if (const auto *value = c.Find("iteration"))
					if (const auto *integer = std::get_if<int64_t>(value)) iteration = double(*integer);
				if (!std::isfinite(iteration) || iteration < double(std::numeric_limits<int32_t>::min()) ||
					iteration > double(std::numeric_limits<int32_t>::max()))
					return c.Fail(
						Status::UnsupportedExecution,
						"Fold Noise iteration exceeds shader integer range",
						"iteration"
					);
				in.Iteration = int32_t(iteration);
				const double mode = c.SourceChoice("mode", 0);
				if (mode != 0 && mode != 1)
					return c.Fail(
						Status::UnsupportedExecution,
						"Fold Noise mode is outside source selector choices",
						"mode"
					);
				in.Mode = int32_t(mode);
				Vector2 position;
				if (!source2d::ReferenceVector(c, "position", in.Canvas.Raw, position) ||
					!FoldPair(c, "position", position, in.Position) ||
					!FoldPair(c, "scale", c.Vec2("scale", {2, 2}), in.Scale) ||
					!FoldFloat(c, "rotation", c.Scalar("rotation"), in.Rotation) ||
					!FoldPair(c, "level_in", c.Vec2("level_in", {0, 1}), in.LevelIn) ||
					!FoldPair(c, "level_out", c.Vec2("level_out", {0, 1}), in.LevelOut))
					return false;
				if (in.LevelIn[0] == in.LevelIn[1])
					return c.Fail(
						Status::UnsupportedExecution, "Fold Noise equal levels divide by zero", "level_in"
					);
				if (!FoldFinite(c, "level_in", in.LevelIn[1] - in.LevelIn[0])) return false;
				if (in.Uv && !FoldFloat(c, "uv_mix", c.Scalar("uv_mix", 1), in.UvMix)) return false;
				if (in.Iteration > 0 &&
					(!FoldFloat(c, "stretch", c.Scalar("stretch", 2), in.Stretch) ||
					 !FoldFloat(c, "amplitude", c.Scalar("amplitude", 1.3), in.Amplitude) ||
					 !FoldPair(c, "detail", c.Vec2("detail", {3, 1}), in.Density)))
					return false;
				return c.FailureCode == Status::Ok;
			}
			float FoldMix(float low, float high, float weight) {
				return low * (1.f - weight) + high * weight;
			}
			bool FoldVectorFinite(NodeContext &c, std::string_view port, Float2 value) {
				return FoldFinite(c, port, value[0]) && FoldFinite(c, port, value[1]);
			}
			bool ShadeFold(NodeContext &c, const FoldInputs &in, uint32_t x, uint32_t y, Rgba &pixel) {
				pixel = {};
				if (!in.Covered || float(x) + .5f >= in.Dimension[0] || float(y) + .5f >= in.Dimension[1])
					return true;
				const float u = (float(x) + .5f) / in.Dimension[0], v = (float(y) + .5f) / in.Dimension[1];
				Float2 uv{u, v};
				float alpha = 1;
				if (in.Uv) {
					const auto sample = SampleNearest(*in.Uv, u, v);
					alpha = float(sample[3]);
					uv = {
						FoldMix(u, float(sample[0]), in.UvMix), FoldMix(v, 1.f - float(sample[1]), in.UvMix)
					};
				}
				if (!FoldFinite(c, "uv_map", alpha) || !FoldVectorFinite(c, "uv_map", uv)) return false;
				const float angle = in.Rotation * .017453292519943295f,
							aspect = in.Dimension[1] / in.Dimension[0];
				if (!FoldFinite(c, "rotation", angle) || !FoldFinite(c, "dimension", aspect)) return false;
				const Float2 delta{
					uv[0] - in.Position[0] / in.Dimension[0],
					uv[1] * aspect - in.Position[1] / in.Dimension[1]
				};
				if (!FoldVectorFinite(c, "position", delta)) return false;
				const float cosine = std::cos(angle), sine = std::sin(angle);
				Float2 point{
					(delta[0] * cosine - delta[1] * sine) * in.Scale[0],
					(delta[0] * sine + delta[1] * cosine) * in.Scale[1]
				};
				if (!FoldVectorFinite(c, "scale", point)) return false;
				for (int32_t i = 0; i < in.Iteration; ++i) {
					const Float2 cosineInput{point[1] * in.Density[0], point[0] * in.Density[0] + in.Stretch};
					if (!FoldVectorFinite(c, "detail", cosineInput)) return false;
					point = {
						point[0] + std::cos(cosineInput[0]) / 3.f, point[1] + std::cos(cosineInput[1]) / 3.f
					};
					if (!FoldVectorFinite(c, "detail", point)) return false;
					// grug sine reads updated point, but both components update together.
					const Float2 sineInput{point[1] * in.Density[1] + in.Stretch, point[0] * in.Density[1]};
					if (!FoldVectorFinite(c, "detail", sineInput)) return false;
					point = {
						(point[0] + std::sin(sineInput[0]) / 2.f) * in.Amplitude,
						(point[1] + std::sin(sineInput[1]) / 2.f) * in.Amplitude
					};
					if (!FoldVectorFinite(c, "amplitude", point)) return false;
				}
				const Float2 values{
					point[0] - 2.f * std::floor(point[0] / 2.f) - 1.f,
					point[1] - 2.f * std::floor(point[1] / 2.f) - 1.f
				};
				if (!FoldVectorFinite(c, "scale", values)) return false;
				std::array<float, 3> color{};
				if (in.Mode == 0) {
					const float length = std::sqrt(values[0] * values[0] + values[1] * values[1]);
					if (!FoldFinite(c, "scale", length)) return false;
					color = {length, length, length};
					alpha = (1.f + length) * alpha;
				} else
					color = {std::abs(values[0]), std::abs(values[1]), 0};
				for (auto &channel : color) {
					const float numerator = channel - in.LevelIn[0];
					if (!FoldFinite(c, "level_in", numerator)) return false;
					const float weight = numerator / (in.LevelIn[1] - in.LevelIn[0]);
					if (!FoldFinite(c, "level_in", weight)) return false;
					channel = FoldMix(in.LevelOut[0], in.LevelOut[1], weight);
					if (!FoldFinite(c, "level_out", channel)) return false;
				}
				if (!FoldFinite(c, "uv_map", alpha)) return false;
				pixel = {color[0], color[1], color[2], alpha};
				return true;
			}
			bool QuoteFold(NodeContext &c, const FoldInputs &in, uint64_t &work) {
				const bool mask =
					in.Mask || std::any_of(c.ImageArrays.begin(), c.ImageArrays.end(), [](const auto &v) {
						return v.first == "mask" && v.second && !v.second->Images.empty();
					});
				if (!source2d::ComplexBatchAdmission(
						c, 1, "surface_out", "surface_out", 1 + size_t(mask) + size_t(c.NoiseFieldRequested)
					))
					return false;
				const uint64_t pixels = uint64_t(in.Canvas.Width) * in.Canvas.Height,
							   cost = BASE_WORK + (in.Covered ? COVERED_ITERATION_WORK *
																	uint64_t(std::max(in.Iteration, 0))
															  : 0);
				if (work > WORK_LIMIT || pixels > (WORK_LIMIT - work) / cost)
					return c.Fail(
						Status::LimitExceeded,
						"Fold Noise complete batch exceeds CPU work limit",
						"surface_out"
					);
				work += pixels * cost;
				const auto format = *DescribeSurfaceFormat(in.Format);
				for (uint32_t y = 0; y < in.Canvas.Height; ++y)
					for (uint32_t x = 0; x < in.Canvas.Width; ++x) {
						Rgba pixel{};
						if (!ShadeFold(c, in, x, y, pixel)) return false;
						for (size_t lane = 0; lane < format.Channels; ++lane)
							if (format.FloatingPoint && format.BitsPerChannel == 16 &&
								std::abs(pixel[lane]) > 65504.)
								return c.Fail(
									Status::InvalidValue,
									"Fold Noise exceeds half-float storage range",
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
							if (!FoldFinite(c, "mask", brightness) ||
								!FoldFinite(c, "mask", storedAlpha * brightness))
								return false;
						}
					}
				return true;
			}
			bool DrawFold(NodeContext &c) {
				ENGINE_PROFILE("imagegraph.source.fold_noise");
				FoldInputs in;
				uint64_t work = 0;
				if (!PrepareFold(c, in) || !QuoteFold(c, in, work)) return false;
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
						if (!ShadeFold(c, in, x, y, pixel)) return false;
						if (!WritePixel(*out, x, y, pixel))
							return c.Fail(
								Status::InvalidValue,
								"Fold Noise sample exceeds output storage range",
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
							if (!FoldFinite(c, "mask", brightness) ||
								!FoldFinite(c, "mask", float(pixel[3])) || !WritePixel(scratch, x, y, pixel))
								return c.Fail(
									Status::InvalidValue,
									"Fold Noise mask exceeds finite storage range",
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
									"Fold Noise mask copy exceeds storage range",
									"surface_out"
								);
				return c.FailureCode == Status::Ok;
			}
		}
	}
	bool AdmitSourceFoldNoise(NodeContext &c, uint64_t &work) {
		source_fold_noise::FoldInputs in;
		return source_fold_noise::PrepareFold(c, in) && source_fold_noise::QuoteFold(c, in, work);
	}
	bool FoldNoise(NodeContext &c) {
		return source_fold_noise::DrawFold(c);
	}
}
