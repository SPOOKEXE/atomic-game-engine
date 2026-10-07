#include "SourceAnisoNoise.hpp"

#include "../SourceMappedInputs.hpp"
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
// Native binary32 equations from the pinned Pixel Composer anisotropic shader.
namespace engine::imagegraph::detail {
	namespace {
		namespace source_aniso_noise {
			constexpr uint64_t WORK_LIMIT = 64000000, BASE_WORK = 512, COVERED_WORK = 512;
			using Float2 = std::array<float, 2>;
			struct AnisoInputs {
				source2d::ComplexCanvas Canvas;
				SurfaceFormat Format = SurfaceFormat::RGBA8Unorm;
				Float2 Dimension{}, Position{}, XAmount{2, 2}, YAmount{16, 16}, Angle{}, LevelIn{0, 1},
					LevelOut{0, 1};
				const Image *Uv = nullptr, *Mask = nullptr, *XMap = nullptr, *YMap = nullptr,
							*AngleMap = nullptr;
				float Seed = 0, ColourSeed = 0, UvMix = 1;
				int Mode = 0;
				bool Covered = false, Tile = false;
			};
			bool AnisoFinite(NodeContext &c, std::string_view port, float value) {
				return std::isfinite(value) || c.Fail(
												   Status::InvalidValue,
												   "Anisotropic Noise shader arithmetic exceeds finite range",
												   port
											   );
			}
			bool AnisoFloat(NodeContext &c, std::string_view port, double value, float &out) {
				if (!std::isfinite(value) || std::abs(value) > double(std::numeric_limits<float>::max()))
					return c.Fail(
						Status::InvalidValue, "Anisotropic Noise value exceeds shader float range", port
					);
				out = float(value);
				return true;
			}
			bool AnisoPair(NodeContext &c, std::string_view port, Vector2 value, Float2 &out) {
				return AnisoFloat(c, port, value.X, out[0]) && AnisoFloat(c, port, value.Y, out[1]);
			}
			bool AnisoSurface(NodeContext &c, std::string_view port, const Image *image) {
				if (const auto *value = c.Find(port); value && std::holds_alternative<AtlasValue>(*value))
					return c.Fail(
						Status::UnsupportedExecution,
						"Anisotropic Noise raw sampler binding rejects Atlas",
						port
					);
				return !image ||
					   ValidSurfaceLayout(*image, Limits::MaximumDimension, Limits::MaximumEvaluationBytes) ||
					   c.Fail(Status::InvalidValue, "Anisotropic Noise sampler layout is invalid", port);
			}
			bool AnisoMappedControl(
				NodeContext &c,
				std::string_view port,
				std::string_view mapPort,
				Float2 &range,
				const Image *&map
			) {
				Vector2 value;
				if (!ReadSourceMappedRange(c, port, value)) return false;
				map = SourceRangeMapped(c, port) ? c.Input(mapPort) : nullptr;
				return AnisoSurface(c, mapPort, map) && AnisoPair(c, port, value, range);
			}

			bool AnisoTile(NodeContext &c, bool &out) {
				out = false;
				const auto domain = c.InputDomain("tile");
				if ((domain && domain->Kind == SourceSocketKind::Surface) || c.Input("tile"))
					return c.Fail(
						Status::UnsupportedExecution,
						"Anisotropic Noise Tile needs an unrepresented source surface handle",
						"tile"
					);
				const auto *value = c.Find("tile");
				if (!value) return c.FailureCode == Status::Ok;
				if (const auto *flag = std::get_if<bool>(value)) {
					out = *flag;
					return true;
				}
				double number = 0;
				if (const auto *scalar = std::get_if<double>(value))
					number = *scalar;
				else if (const auto *integer = std::get_if<int64_t>(value))
					number = double(*integer);
				else
					return c.Fail(
						Status::UnsupportedExecution,
						"Anisotropic Noise Tile needs a source boolean or number",
						"tile"
					);
				if (!std::isfinite(number))
					return c.Fail(Status::InvalidValue, "Anisotropic Noise Tile must be finite", "tile");
				out = number > .5;
				return true;
			}
			bool PrepareAniso(NodeContext &c, AnisoInputs &in) {
				in.Uv = c.Input("uv_map");
				in.Mask = c.Input("mask");
				if (!AnisoSurface(c, "uv_map", in.Uv) || !AnisoSurface(c, "mask", in.Mask) ||
					!source2d::ResolveComplexCanvas(c, in.Canvas))
					return false;
				if (in.Canvas.Width > c.Request.MaximumImageDimension ||
					in.Canvas.Height > c.Request.MaximumImageDimension)
					return c.Fail(
						Status::LimitExceeded, "Anisotropic Noise exceeds request dimensions", "dimension"
					);
				if (!AnisoPair(c, "dimension", in.Canvas.Raw, in.Dimension)) return false;
				const auto format = ResolveProcessorSurfaceFormat(c, nullptr);
				if (!format) return false;
				in.Format = *format;
				in.Covered = in.Dimension[0] > .5f && in.Dimension[1] > .5f;
				if (!in.Covered) return c.FailureCode == Status::Ok;
				const auto mode = c.Integer("render_mode");
				if (mode < 0 || mode > 1)
					return c.Fail(
						Status::UnsupportedExecution,
						"Anisotropic Noise render mode is undefined",
						"render_mode"
					);
				in.Mode = int(mode);
				if (!AnisoTile(c, in.Tile)) return false;
				if (!c.Find("seed") || (mode == 0 && !c.Find("seed_2")))
					return c.Fail(
						Status::UnsupportedExecution,
						"Anisotropic Noise requires resolved source seeds",
						c.Find("seed") ? "seed_2" : "seed"
					);
				Vector2 position;
				if (!source2d::ReferenceVector(c, "position", in.Canvas.Raw, position) ||
					!AnisoPair(c, "position", position, in.Position) ||
					!AnisoFloat(c, "seed", c.Scalar("seed"), in.Seed) ||
					(mode == 0 && !AnisoFloat(c, "seed_2", c.Scalar("seed_2"), in.ColourSeed)) ||
					!AnisoPair(c, "level_in", c.Vec2("level_in", {0, 1}), in.LevelIn) ||
					!AnisoPair(c, "level_out", c.Vec2("level_out", {0, 1}), in.LevelOut))
					return false;
				if (in.LevelIn[0] == in.LevelIn[1])
					return c.Fail(
						Status::UnsupportedExecution, "Anisotropic Noise levels divide by zero", "level_in"
					);
				if (!AnisoFinite(c, "level_in", in.LevelIn[1] - in.LevelIn[0])) return false;
				if (in.Uv && !AnisoFloat(c, "uv_mix", c.Scalar("uv_mix", 1), in.UvMix)) return false;
				return AnisoMappedControl(c, "x_amount", "x_amount_map", in.XAmount, in.XMap) &&
					   AnisoMappedControl(c, "y_amount", "y_amount_map", in.YAmount, in.YMap) &&
					   AnisoMappedControl(c, "rotation", "rotation_map", in.Angle, in.AngleMap);
			}
			float AnisoFract(float value) {
				return value - std::floor(value);
			}
			float AnisoMix(float low, float high, float weight) {
				return low * (1.f - weight) + high * weight;
			}
			bool AnisoVectorFinite(NodeContext &c, std::string_view port, Float2 value) {
				return AnisoFinite(c, port, value[0]) && AnisoFinite(c, port, value[1]);
			}
			float AnisoMapped(Float2 range, const Image *map, float u, float v) {
				if (!map) return range[0];
				const auto pixel = SampleNearest(*map, u, v);
				return AnisoMix(
					range[0], range[1], (float(pixel[0]) + float(pixel[1]) + float(pixel[2])) / 3.f
				);
			}
			bool Random(NodeContext &c, Float2 point, float seed, float &out, std::string_view port) {
				const auto one = [&](float s, float &v) {
					const float shifted = s + 453.456f;
					const float remainder = shifted - 100.f * std::floor(shifted / 100.f);
					const float phase = (point[0] * 12.9898f + point[1] * 78.233f) * remainder * 12.588f;
					if (!AnisoFinite(c, port, phase)) return false;
					v = AnisoFract(std::sin(phase) * 43758.5453123f);
					return true;
				};
				float low = 0, high = 0;
				if (!one(std::floor(seed), low) || !one(std::floor(seed) + 1.f, high)) return false;
				out = AnisoMix(low, high, AnisoFract(seed));
				return AnisoFinite(c, port, out);
			}
			bool ShadeAniso(NodeContext &c, const AnisoInputs &in, uint32_t x, uint32_t y, Rgba &pixel) {
				pixel = {};
				if (!in.Covered || float(x) + .5f >= in.Dimension[0] || float(y) + .5f >= in.Dimension[1])
					return true;
				const float u = (float(x) + .5f) / in.Dimension[0], v = (float(y) + .5f) / in.Dimension[1];
				Float2 uv{u, v};
				float alpha = 1;
				if (in.Uv) {
					const auto p = SampleNearest(*in.Uv, u, v);
					uv = {AnisoMix(u, float(p[0]), in.UvMix), AnisoMix(v, 1.f - float(p[1]), in.UvMix)};
					alpha = float(p[3]);
				}
				if (!AnisoVectorFinite(c, "uv_map", uv) || !AnisoFinite(c, "uv_map", alpha)) return false;
				const float nx = AnisoMapped(in.XAmount, in.XMap, u, v),
							ny = in.YMap ? AnisoMapped(in.YAmount, in.YMap, u, v) : in.YAmount[1];
				const float angle = AnisoMapped(in.Angle, in.AngleMap, u, v) * .017453292519943295f;
				if (!AnisoFinite(c, "x_amount", nx) || !AnisoFinite(c, "y_amount", ny) ||
					!AnisoFinite(c, "rotation", angle))
					return false;
				const float cosine = std::cos(angle), sine = std::sin(angle);
				const Float2 delta{
					uv[0] - in.Position[0] / in.Dimension[0],
					uv[1] * (in.Dimension[1] / in.Dimension[0]) - in.Position[1] / in.Dimension[1]
				};
				const Float2 point{delta[0] * cosine - delta[1] * sine, delta[0] * sine + delta[1] * cosine};
				if (!AnisoVectorFinite(c, "position", point)) return false;
				float yy = std::floor(point[1] * ny);
				if (!AnisoFinite(c, "y_amount", yy)) return false;
				const float absY = std::abs(yy);
				yy = (absY - 289.653f * std::floor(absY / 289.653f)) * (yy < 0 ? -1.f : yy > 0 ? 1.f : 0.f);
				float random = 0;
				if (!Random(c, {1.f, yy}, in.Seed, random, "seed")) return false;
				float xx = (point[0] + random) * nx;
				if (!AnisoFinite(c, "x_amount", xx)) return false;
				if (in.Tile) xx = AnisoFract(AnisoFract(xx / 2.f) + 1.f) * 2.f;
				const float x0 = std::floor(xx),
							weight = (xx - x0 - in.LevelIn[0]) / (in.LevelIn[1] - in.LevelIn[0]);
				if (!AnisoFinite(c, "level_in", weight)) return false;
				const float progress = AnisoMix(in.LevelOut[0], in.LevelOut[1], weight);
				if (!AnisoFinite(c, "level_out", progress)) return false;
				float value = progress;
				if (in.Mode == 0) {
					float first = 0, last = 0;
					if (!Random(c, {x0, yy}, in.ColourSeed, first, "seed_2") ||
						!Random(c, {x0 + 1.f, yy}, in.ColourSeed, last, "seed_2"))
						return false;
					value = AnisoMix(first, last, progress);
				}
				if (!AnisoFinite(c, "surface_out", value)) return false;
				pixel = {value, value, value, alpha};
				return true;
			}
			bool QuoteAniso(NodeContext &c, const AnisoInputs &in, uint64_t &work) {
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
						"Anisotropic Noise complete batch exceeds CPU work limit",
						"surface_out"
					);
				work += pixels * cost;
				const auto format = *DescribeSurfaceFormat(in.Format);
				for (uint32_t y = 0; y < in.Canvas.Height; ++y)
					for (uint32_t x = 0; x < in.Canvas.Width; ++x) {
						Rgba pixel{};
						if (!ShadeAniso(c, in, x, y, pixel)) return false;
						for (size_t lane = 0; lane < format.Channels; ++lane)
							if (format.FloatingPoint && format.BitsPerChannel == 16 &&
								std::abs(pixel[lane]) > 65504.)
								return c.Fail(
									Status::InvalidValue,
									"Anisotropic Noise exceeds half-float storage range",
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
							if (!AnisoFinite(c, "mask", brightness) ||
								!AnisoFinite(c, "mask", storedAlpha * brightness))
								return false;
						}
					}
				return true;
			}
			bool DrawAniso(NodeContext &c) {
				ENGINE_PROFILE("imagegraph.source.aniso_noise");
				AnisoInputs in;
				uint64_t work = 0;
				if (!PrepareAniso(c, in) || !QuoteAniso(c, in, work)) return false;
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
						if (!ShadeAniso(c, in, x, y, pixel)) return false;
						if (!WritePixel(*out, x, y, pixel))
							return c.Fail(
								Status::InvalidValue,
								"Anisotropic Noise sample exceeds output storage range",
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
							if (!AnisoFinite(c, "mask", brightness) ||
								!AnisoFinite(c, "mask", float(pixel[3])) || !WritePixel(scratch, x, y, pixel))
								return c.Fail(
									Status::InvalidValue,
									"Anisotropic Noise mask exceeds finite storage range",
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
									"Anisotropic Noise mask copy exceeds storage range",
									"surface_out"
								);
				return c.FailureCode == Status::Ok;
			}
		}
	}
	bool AdmitSourceAnisoNoise(NodeContext &c, uint64_t &work) {
		source_aniso_noise::AnisoInputs in;
		return source_aniso_noise::PrepareAniso(c, in) && source_aniso_noise::QuoteAniso(c, in, work);
	}
	bool AnisotropicNoise(NodeContext &c) {
		return source_aniso_noise::DrawAniso(c);
	}
}
