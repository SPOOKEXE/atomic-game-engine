#include "SourceGaussianNoise.hpp"

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
namespace engine::imagegraph::detail {
	namespace {
		namespace source_gaussian_noise {
			constexpr uint64_t WORK_LIMIT = 64000000, COVERED_WORK = 4096, UNCOVERED_WORK = 512;
			using Float2 = std::array<float, 2>;
			struct GaussianInputs {
				source2d::ComplexCanvas Canvas;
				SurfaceFormat Format = SurfaceFormat::RGBA8Unorm;
				Float2 Dimension{}, Position{}, Scale{1, 1}, LevelIn{0, 1}, LevelOut{0, 1};
				const Image *First = nullptr, *Second = nullptr;
				float Seed = 0, Rotation = 0, Mean = .5f, Variance = .5f;
				bool Covered = false, Conversion = false;
			};
			bool GaussianFinite(NodeContext &c, std::string_view port, float value) {
				return std::isfinite(value) ||
					   c.Fail(
						   Status::InvalidValue, "Gaussian Noise shader arithmetic exceeds finite range", port
					   );
			}
			bool GaussianFloat(NodeContext &c, std::string_view port, double value, float &out) {
				if (!std::isfinite(value) || std::abs(value) > double(std::numeric_limits<float>::max()))
					return c.Fail(
						Status::InvalidValue, "Gaussian Noise value exceeds shader float range", port
					);
				out = float(value);
				return true;
			}
			bool GaussianPair(NodeContext &c, std::string_view port, Vector2 value, Float2 &out) {
				return GaussianFloat(c, port, value.X, out[0]) && GaussianFloat(c, port, value.Y, out[1]);
			}
			bool GaussianSurface(NodeContext &c, std::string_view port, const Image *image) {
				if (const auto *value = c.Find(port); value && std::holds_alternative<AtlasValue>(*value))
					return c.Fail(
						Status::UnsupportedExecution, "Gaussian Noise raw sampler binding rejects Atlas", port
					);
				return !image ||
					   ValidSurfaceLayout(*image, Limits::MaximumDimension, Limits::MaximumEvaluationBytes) ||
					   c.Fail(Status::InvalidValue, "Gaussian Noise sampler layout is invalid", port);
			}

			bool GaussianConversion(NodeContext &c, bool &out) {
				out = false;
				const auto domain = c.InputDomain("use_conversion");
				if ((domain && domain->Kind == SourceSocketKind::Surface) || c.Input("use_conversion"))
					return c.Fail(
						Status::UnsupportedExecution,
						"Gaussian Noise conversion boolean needs an unrepresented source surface handle",
						"use_conversion"
					);
				const auto *value = c.Find("use_conversion");
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
						"Gaussian Noise conversion needs a resolved source boolean or number",
						"use_conversion"
					);
				if (!std::isfinite(number))
					return c.Fail(
						Status::InvalidValue,
						"Gaussian Noise conversion value must be finite",
						"use_conversion"
					);
				// grug source Bool getter uses bool(value), whose real threshold is strictly above .5.
				out = number > .5;
				return true;
			}
			bool PrepareGaussian(NodeContext &c, GaussianInputs &in) {
				if (!source2d::ResolveGeneratorDimensions(c, nullptr, in.Canvas.Width, in.Canvas.Height))
					return false;
				if (in.Canvas.Width > c.Request.MaximumImageDimension ||
					in.Canvas.Height > c.Request.MaximumImageDimension)
					return c.Fail(
						Status::LimitExceeded, "Gaussian Noise exceeds request dimensions", "dimension"
					);
				in.Canvas.Raw = c.Vec2("dimension", {1, 1});
				if (!c.IsLinked("dimension")) {
					const auto unit = c.Integer("dimension_unit", 1);
					if (unit == 1) {
						in.Canvas.Raw.X *= c.Project.SurfaceWidth;
						in.Canvas.Raw.Y *= c.Project.SurfaceHeight;
					}
				}
				if (!GaussianPair(c, "dimension", in.Canvas.Raw, in.Dimension)) return false;
				const auto format = ResolveProcessorSurfaceFormat(c, nullptr);
				if (!format) return false;
				in.Format = *format;
				in.Covered = in.Dimension[0] > .5f && in.Dimension[1] > .5f;
				if (!in.Covered) return c.FailureCode == Status::Ok;
				if (!GaussianConversion(c, in.Conversion) ||
					!GaussianPair(c, "position", c.Vec2("position"), in.Position) ||
					!GaussianPair(c, "scale", c.Vec2("scale", {1, 1}), in.Scale) ||
					!GaussianFloat(c, "rotation", c.Scalar("rotation"), in.Rotation) ||
					!GaussianFloat(c, "mean", c.Scalar("mean", .5), in.Mean) ||
					!GaussianFloat(c, "varience", c.Scalar("varience", .5), in.Variance) ||
					!GaussianPair(c, "level_in", c.Vec2("level_in", {0, 1}), in.LevelIn) ||
					!GaussianPair(c, "level_out", c.Vec2("level_out", {0, 1}), in.LevelOut))
					return false;
				if (in.LevelIn[0] == in.LevelIn[1])
					return c.Fail(
						Status::UnsupportedExecution, "Gaussian Noise equal levels divide by zero", "level_in"
					);
				if (!GaussianFinite(c, "level_in", in.LevelIn[1] - in.LevelIn[0])) return false;
				if (in.Conversion) {
					in.First = c.Input("conv_surf_1");
					in.Second = c.Input("conv_surf_2");
					if (!GaussianSurface(c, "conv_surf_1", in.First) ||
						!GaussianSurface(c, "conv_surf_2", in.Second))
						return false;
					if (!in.First)
						return c.Fail(
							Status::UnsupportedExecution,
							"Gaussian Noise first conversion surface has unresolved source binding",
							"conv_surf_1"
						);
					if (!in.Second)
						return c.Fail(
							Status::UnsupportedExecution,
							"Gaussian Noise second conversion surface has unresolved source binding",
							"conv_surf_2"
						);
				} else {
					if (!c.Find("seed"))
						return c.Fail(
							Status::UnsupportedExecution,
							"Gaussian Noise requires resolved source Seed",
							"seed"
						);
					if (!GaussianFloat(c, "seed", c.Scalar("seed"), in.Seed)) return false;
				}
				return c.FailureCode == Status::Ok;
			}
			float GaussianMix(float low, float high, float weight) {
				return low * (1.f - weight) + high * weight;
			}
			bool GaussianVectorFinite(NodeContext &c, std::string_view port, Float2 value) {
				return GaussianFinite(c, port, value[0]) && GaussianFinite(c, port, value[1]);
			}
			bool GaussianRandom(NodeContext &c, Float2 point, float seed, float &out) {
				const float angle = point[0] * 78.233f + point[1] * 128.852f;
				const float mod = seed - 100000.f * std::floor(seed / 100000.f),
							factor = 43758.5453f + mod / 10.f;
				if (!GaussianFinite(c, "seed", angle) || !GaussianFinite(c, "seed", mod) ||
					!GaussianFinite(c, "seed", factor))
					return false;
				const float value = std::sin(angle) * factor;
				if (!GaussianFinite(c, "seed", value)) return false;
				out = value - std::floor(value);
				return GaussianFinite(c, "seed", out);
			}
			bool
			ShadeGaussian(NodeContext &c, const GaussianInputs &in, uint32_t x, uint32_t y, Rgba &pixel) {
				pixel = {};
				if (!in.Covered || float(x) + .5f >= in.Dimension[0] || float(y) + .5f >= in.Dimension[1])
					return true;
				const float u = (float(x) + .5f) / in.Dimension[0], v = (float(y) + .5f) / in.Dimension[1],
							angle = in.Rotation * .017453292519943295f;
				if (!GaussianFinite(c, "rotation", angle)) return false;
				const float cosine = std::cos(angle), sine = std::sin(angle);
				const Float2 point{
					(u * cosine - v * sine) * in.Scale[0] - in.Position[0],
					(u * sine + v * cosine) * in.Scale[1] - in.Position[1]
				};
				if (!GaussianVectorFinite(c, "position", point)) return false;
				float first = 0, second = 0;
				if (in.Conversion) {
					first = float(SampleNearest(*in.First, point[0], point[1])[0]);
					second = float(SampleNearest(*in.Second, point[0], point[1])[0]);
					if (!GaussianFinite(c, "conv_surf_1", first) || !GaussianFinite(c, "conv_surf_2", second))
						return false;
					if (!(first > 0 && first <= 1))
						return c.Fail(
							Status::UnsupportedExecution,
							"Gaussian Noise conversion red must lie in (0,1] for log and square root",
							"conv_surf_1"
						);
				} else {
					if (!GaussianRandom(c, {point[0] + 3.9613f, point[1] + 1.6452f}, in.Seed, first) ||
						!GaussianRandom(c, {point[0] + .1654f, point[1] + 2.9873f}, in.Seed, second))
						return false;
					first = std::max(first, .001f);
				}
				constexpr float pi = 3.14159265358979323846f;
				const float phase = 2.f * pi * second;
				if (!GaussianFinite(c, in.Conversion ? "conv_surf_2" : "seed", phase)) return false;
				const float normal = std::sqrt(-2.f * std::log(first)) * std::cos(phase);
				if (!GaussianFinite(c, in.Conversion ? "conv_surf_1" : "seed", normal)) return false;
				float value = in.Mean + normal * in.Variance;
				if (!GaussianFinite(c, "varience", value)) return false;
				const float numerator = value - in.LevelIn[0];
				if (!GaussianFinite(c, "level_in", numerator)) return false;
				const float weight = numerator / (in.LevelIn[1] - in.LevelIn[0]);
				if (!GaussianFinite(c, "level_in", weight)) return false;
				value = GaussianMix(in.LevelOut[0], in.LevelOut[1], weight);
				if (!GaussianFinite(c, "level_out", value)) return false;
				pixel = {value, value, value, 1};
				return true;
			}
			bool QuoteGaussian(NodeContext &c, const GaussianInputs &in, uint64_t &work) {
				if (!source2d::ComplexBatchAdmission(
						c, 1, "surface_out", "surface_out", 1 + size_t(c.NoiseFieldRequested)
					))
					return false;
				const uint64_t pixels = uint64_t(in.Canvas.Width) * in.Canvas.Height,
							   cost = in.Covered ? COVERED_WORK : UNCOVERED_WORK;
				if (work > WORK_LIMIT || pixels > (WORK_LIMIT - work) / cost)
					return c.Fail(
						Status::LimitExceeded,
						"Gaussian Noise complete batch exceeds CPU work limit",
						"surface_out"
					);
				work += pixels * cost;
				const auto format = *DescribeSurfaceFormat(in.Format);
				for (uint32_t y = 0; y < in.Canvas.Height; ++y)
					for (uint32_t x = 0; x < in.Canvas.Width; ++x) {
						Rgba pixel{};
						if (!ShadeGaussian(c, in, x, y, pixel)) return false;
						for (size_t lane = 0; lane < format.Channels; ++lane)
							if (format.FloatingPoint && format.BitsPerChannel == 16 &&
								std::abs(pixel[lane]) > 65504.)
								return c.Fail(
									Status::InvalidValue,
									"Gaussian Noise exceeds half-float storage range",
									"surface_out"
								);
					}
				return true;
			}
			bool DrawGaussian(NodeContext &c) {
				ENGINE_PROFILE("imagegraph.source.gaussian_noise");
				GaussianInputs in;
				uint64_t work = 0;
				if (!PrepareGaussian(c, in) || !QuoteGaussian(c, in, work)) return false;
				auto *output = c.NewImage("surface_out", in.Canvas.Width, in.Canvas.Height, in.Format);
				if (!output) return false;
				for (uint32_t y = 0; y < in.Canvas.Height; ++y)
					for (uint32_t x = 0; x < in.Canvas.Width; ++x) {
						Rgba pixel{};
						if (!ShadeGaussian(c, in, x, y, pixel)) return false;
						if (!WritePixel(*output, x, y, pixel))
							return c.Fail(
								Status::InvalidValue,
								"Gaussian Noise exceeds output storage range",
								"surface_out"
							);
					}
				return c.FailureCode == Status::Ok;
			}
		}
	}
	bool AdmitSourceGaussianNoise(NodeContext &c, uint64_t &work) {
		source_gaussian_noise::GaussianInputs in;
		return source_gaussian_noise::PrepareGaussian(c, in) &&
			   source_gaussian_noise::QuoteGaussian(c, in, work);
	}
	bool GaussianNoise(NodeContext &c) {
		return source_gaussian_noise::DrawGaussian(c);
	}
}
