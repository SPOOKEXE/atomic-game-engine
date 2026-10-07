#include "SourcePerlin.hpp"

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
namespace engine::imagegraph::detail {
	namespace {
		constexpr uint64_t PERLIN_WORK_LIMIT = 64000000, PERLIN_PIXEL_WORK = 512, PERLIN_OCTAVE_WORK = 2048;
		using Float2 = std::array<float, 2>;
		struct PerlinInputs {
			source2d::ComplexCanvas Canvas;
			SurfaceFormat Format = SurfaceFormat::RGBA8Unorm;
			Float2 Dimension{}, Position{}, Scale{4, 4}, Scaling{2, 2}, Amplitude{.5f, .5f}, LevelIn{0, 1},
				LevelOut{0, 1};
			std::array<Float2, 3> Colors{{{0, 1}, {0, 1}, {0, 1}}};
			const Image *Uv = nullptr, *Mask = nullptr, *ScaleMap = nullptr, *ScalingMap = nullptr,
						*AmplitudeMap = nullptr;
			float Seed = 0, Phase = 0, Rotation = 0, UvMix = 1;
			int32_t Iteration = 4;
			int Blend = 0, Mode = 0;
			bool Tile = true;
		};
		bool PerlinFinite(NodeContext &c, std::string_view port, float value) {
			return std::isfinite(value) ||
				   c.Fail(Status::InvalidValue, "Perlin shader arithmetic exceeds finite range", port);
		}
		bool PerlinFloat(NodeContext &c, std::string_view port, double value, float &out) {
			if (!std::isfinite(value) || std::abs(value) > double(std::numeric_limits<float>::max()))
				return c.Fail(Status::InvalidValue, "Perlin value exceeds finite shader range", port);
			out = float(value);
			return true;
		}
		bool PerlinPair(NodeContext &c, std::string_view port, Vector2 value, Float2 &out) {
			return PerlinFloat(c, port, value.X, out[0]) && PerlinFloat(c, port, value.Y, out[1]);
		}
		bool PerlinSurface(NodeContext &c, std::string_view port, const Image *image) {
			if (const auto *value = c.Find(port); value && std::holds_alternative<AtlasValue>(*value))
				return c.Fail(Status::UnsupportedExecution, "Perlin raw sampler binding rejects Atlas", port);
			return !image ||
				   ValidSurfaceLayout(*image, Limits::MaximumDimension, Limits::MaximumEvaluationBytes) ||
				   c.Fail(Status::InvalidValue, "Perlin sampler layout is invalid", port);
		}
		bool PerlinControl(
			NodeContext &c, std::string_view port, std::string_view mapPort, Float2 &range, const Image *&map
		) {
			Vector2 value;
			if (SourceRangeMapped(c, port)) {
				if (!ReadSourceMappedRange(c, port, value)) return false;
				map = c.Input(mapPort);
				if (!PerlinSurface(c, mapPort, map)) return false;
			} else if (port == "scale") {
				if (!ReadSourceMappedRange(c, port, value)) return false;
			} else {
				const double scalar = c.Scalar(port, port == "scaling" ? 2 : .5);
				value = {scalar, scalar};
			}
			return PerlinPair(c, port, value, range);
		}
		bool PreparePerlin(NodeContext &c, PerlinInputs &in) {
			if (!c.Find("seed"))
				return c.Fail(Status::UnsupportedExecution, "Perlin requires a resolved source seed", "seed");
			in.Uv = c.Input("uv_map");
			in.Mask = c.Input("mask");
			if (!PerlinSurface(c, "uv_map", in.Uv) || !PerlinSurface(c, "mask", in.Mask) ||
				!source2d::ResolveGeneratorDimensions(c, in.Mask, in.Canvas.Width, in.Canvas.Height))
				return false;
			if (in.Canvas.Width > c.Request.MaximumImageDimension ||
				in.Canvas.Height > c.Request.MaximumImageDimension)
				return c.Fail(Status::LimitExceeded, "Perlin output exceeds request dimensions", "dimension");
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
			const auto format = ResolveProcessorSurfaceFormat(c, nullptr);
			if (!format) return false;
			in.Format = *format;
			Vector2 position;
			if (!source2d::ReferenceVector(c, "position", in.Canvas.Raw, position) ||
				!PerlinPair(c, "dimension", in.Canvas.Raw, in.Dimension) ||
				!PerlinPair(c, "position", position, in.Position) ||
				!PerlinPair(c, "level_in", c.Vec2("level_in", {0, 1}), in.LevelIn) ||
				!PerlinPair(c, "level_out", c.Vec2("level_out", {0, 1}), in.LevelOut))
				return false;
			if (!PerlinFloat(c, "seed", c.Scalar("seed"), in.Seed) ||
				!PerlinFloat(c, "phase", c.Scalar("phase"), in.Phase) ||
				!PerlinFloat(c, "rotation", c.Scalar("rotation"), in.Rotation))
				return false;
			if (in.Uv && !PerlinFloat(c, "uv_mix", c.Scalar("uv_mix", 1), in.UvMix)) return false;
			in.Tile = c.Boolean("tile", true);
			const auto iterations = c.Integer("iteration", 4);
			const auto blend = c.SourceChoice("blend_method"), mode = c.SourceChoice("color_mode");
			if (iterations < std::numeric_limits<int32_t>::min() ||
				iterations > std::numeric_limits<int32_t>::max())
				return c.Fail(
					Status::UnsupportedExecution, "Perlin iteration exceeds shader integer range", "iteration"
				);
			if (blend < 0 || blend > 1 || std::trunc(blend) != blend)
				return c.Fail(
					Status::UnsupportedExecution, "Perlin blend selector is undefined", "blend_method"
				);
			if (mode < 0 || mode > 2 || std::trunc(mode) != mode)
				return c.Fail(
					Status::UnsupportedExecution, "Perlin color selector is undefined", "color_mode"
				);
			in.Iteration = int32_t(iterations);
			in.Blend = int(blend);
			in.Mode = int(mode);
			if (in.Iteration <= 0 && in.Blend == 0)
				return c.Fail(
					Status::UnsupportedExecution,
					"Perlin empty Add loop divides by zero amplitude total",
					"iteration"
				);
			if (in.LevelIn[0] == in.LevelIn[1])
				return c.Fail(
					Status::UnsupportedExecution, "Perlin equal input levels divide by zero", "level_in"
				);
			if (!PerlinFinite(c, "level_in", in.LevelIn[1] - in.LevelIn[0])) return false;
			if (in.Iteration > 0) {
				if (SourceRangeMapped(c, "scale")) {
					const auto *original = source2d::GeneratorOriginal(c, "scale");
					const auto *array = original ? std::get_if<ArrayValue>(original) : nullptr;
					if (array &&
						(!array->Nested.empty() || !array->Items.empty() ||
						 std::any_of(array->Elements.begin(), array->Elements.end(), [](const auto &value) {
							 return !std::holds_alternative<double>(value) &&
									!std::holds_alternative<int64_t>(value);
						 })))
						return c.Fail(
							Status::UnsupportedExecution,
							"Perlin mapped Scale nested source upload is not a defined vec2 uniform",
							"scale"
						);
				}
				if (in.Dimension[0] == 0 || in.Dimension[1] == 0)
					return c.Fail(
						Status::UnsupportedExecution, "Perlin raw dimension divides by zero", "dimension"
					);
				if (!PerlinControl(c, "scale", "scale_map", in.Scale, in.ScaleMap) ||
					!PerlinControl(c, "scaling", "scaling_map", in.Scaling, in.ScalingMap) ||
					!PerlinControl(c, "amplitude", "amplitude_map", in.Amplitude, in.AmplitudeMap))
					return false;
			}
			if (in.Mode != 0) {
				constexpr std::array<std::string_view, 3> ports{
					"color_r_range", "color_g_range", "color_b_range"
				};
				for (size_t channel = 0; channel < 3; ++channel)
					if (!PerlinPair(c, ports[channel], c.Vec2(ports[channel], {0, 1}), in.Colors[channel]))
						return false;
			}
			return c.FailureCode == Status::Ok;
		}
		float PerlinFract(float value) {
			return value - std::floor(value);
		}
		float PerlinMod(float value, float divisor) {
			return value - divisor * std::floor(value / divisor);
		}
		float PerlinMix(float a, float b, float weight) {
			return a * (1.f - weight) + b * weight;
		}
		float PerlinDot(Float2 a, Float2 b) {
			return a[0] * b[0] + a[1] * b[1];
		}
		bool PerlinVectorFinite(NodeContext &c, std::string_view port, Float2 value) {
			return PerlinFinite(c, port, value[0]) && PerlinFinite(c, port, value[1]);
		}
		float PerlinMapped(Float2 range, const Image *map, float u, float v) {
			if (!map) return range[0];
			const auto sample = SampleNearest(*map, u, v);
			const float weight = (float(sample[0]) + float(sample[1]) + float(sample[2])) / 3.f;
			return PerlinMix(range[0], range[1], weight);
		}
		bool PerlinParameters(
			NodeContext &c,
			const PerlinInputs &in,
			float u,
			float v,
			Float2 &scale,
			float &scaling,
			float &amplitude
		) {
			scale = in.Scale;
			if (in.ScaleMap) {
				const float value = PerlinMapped(in.Scale, in.ScaleMap, u, v);
				scale = {value, value};
			}
			scaling = PerlinMapped(in.Scaling, in.ScalingMap, u, v);
			amplitude = PerlinMapped(in.Amplitude, in.AmplitudeMap, u, v);
			if (in.Tile)
				for (auto &value : scale)
					value = std::floor(value);
			return PerlinVectorFinite(c, "scale", scale) && PerlinFinite(c, "scaling", scaling) &&
				   PerlinFinite(c, "amplitude", amplitude);
		}
		// grug source hash is smoothed absolute signed fract, not generic Perlin gradients.
		bool PerlinGradient(NodeContext &c, const PerlinInputs &in, Float2 point, Float2 &gradient) {
			const Float2 shifted{point[0] + 21.456f, point[1] + 46.856f};
			const float dot = PerlinDot(shifted, {12.989f, 78.233f});
			const float multiplier = 43758.545f + PerlinMod(in.Seed, 100000.f);
			if (!PerlinFinite(c, "scale", dot) || !PerlinFinite(c, "seed", multiplier)) return false;
			const float value = std::abs(PerlinFract(std::sin(dot) * multiplier) * 2.f - 1.f);
			const float t = std::clamp(value, 0.f, 1.f);
			const float random = t * t * (3.f - 2.f * t);
			const float angle = PerlinFract(random + in.Phase / 360.f) * 6.28319f;
			gradient = {std::cos(angle), std::sin(angle)};
			return PerlinVectorFinite(c, "phase", gradient);
		}
		bool
		PerlinNoise(NodeContext &c, const PerlinInputs &in, Float2 position, Float2 scale, float &value) {
			Float2 lower{std::floor(position[0]), std::floor(position[1])},
				upper{std::floor(position[0]) + 1.f, std::floor(position[1]) + 1.f};
			if (in.Tile) {
				if (scale[0] == 0 || scale[1] == 0)
					return c.Fail(
						Status::UnsupportedExecution, "Perlin tiled lattice modulus divides by zero", "scale"
					);
				for (size_t axis = 0; axis < 2; ++axis) {
					lower[axis] = PerlinMod(lower[axis], scale[axis]);
					upper[axis] = PerlinMod(upper[axis], scale[axis]);
				}
			}
			if (!PerlinVectorFinite(c, "scale", lower) || !PerlinVectorFinite(c, "scale", upper))
				return false;
			const Float2 fraction{PerlinFract(position[0]), PerlinFract(position[1])};
			const Float2 blend{
				fraction[0] * fraction[0] * (3.f - 2.f * fraction[0]),
				fraction[1] * fraction[1] * (3.f - 2.f * fraction[1])
			};
			Float2 a{}, b{}, cc{}, d{};
			if (!PerlinGradient(c, in, {lower[0], lower[1]}, a) ||
				!PerlinGradient(c, in, {upper[0], lower[1]}, b) ||
				!PerlinGradient(c, in, {lower[0], upper[1]}, cc) ||
				!PerlinGradient(c, in, {upper[0], upper[1]}, d))
				return false;
			const float first =
				PerlinMix(PerlinDot(fraction, a), PerlinDot({fraction[0] - 1.f, fraction[1]}, b), blend[0]);
			const float second = PerlinMix(
				PerlinDot({fraction[0], fraction[1] - 1.f}, cc),
				PerlinDot({fraction[0] - 1.f, fraction[1] - 1.f}, d),
				blend[0]
			);
			value = PerlinMix(first, second, blend[1]) + .5f;
			return PerlinFinite(c, "scale", value);
		}
		bool PerlinLevel(NodeContext &c, const PerlinInputs &in, float value, float &out) {
			const float progress = (value - in.LevelIn[0]) / (in.LevelIn[1] - in.LevelIn[0]);
			out = PerlinMix(in.LevelOut[0], in.LevelOut[1], progress);
			return PerlinFinite(c, "level_out", out);
		}
		bool PerlinOctaves(
			NodeContext &c,
			const PerlinInputs &in,
			Float2 position,
			Float2 scale,
			float scaling,
			float amplitude,
			float &out
		) {
			float weight = 1, total = 0, value = 0;
			for (int32_t iteration = 0; iteration < in.Iteration; ++iteration) {
				float noise = 0;
				if (!PerlinNoise(c, in, position, scale, noise)) return false;
				const float weighted = noise * weight;
				if (!PerlinFinite(c, "amplitude", weighted)) return false;
				if (in.Blend == 0)
					value += weighted;
				else
					value = std::max(value, weighted);
				total += weight;
				if (!PerlinFinite(c, "amplitude", value) ||
					(in.Blend == 0 && !PerlinFinite(c, "amplitude", total)))
					return false;
				for (size_t axis = 0; axis < 2; ++axis) {
					scale[axis] *= scaling;
					position[axis] *= scaling;
				}
				weight *= amplitude;
			}
			if (in.Blend == 0) {
				if (total == 0)
					return c.Fail(
						Status::UnsupportedExecution,
						"Perlin Add amplitude total divides by zero",
						"amplitude"
					);
				value /= total;
			}
			return PerlinLevel(c, in, value, out);
		}
		bool ShadePerlin(NodeContext &c, const PerlinInputs &in, uint32_t x, uint32_t y, Rgba &pixel) {
			const float u = (float(x) + .5f) / in.Canvas.Width, v = (float(y) + .5f) / in.Canvas.Height;
			Float2 uv{u, v};
			float alpha = 1;
			if (in.Uv) {
				const auto sample = SampleNearest(*in.Uv, u, v);
				uv = {
					PerlinMix(u, float(sample[0]), in.UvMix), PerlinMix(v, 1.f - float(sample[1]), in.UvMix)
				};
				alpha = float(sample[3]);
			}
			if (!PerlinFinite(c, "uv_map", alpha)) return false;
			Float2 scale{};
			float scaling = 0, amplitude = 0;
			Float2 st{};
			if (in.Iteration > 0) {
				if (!PerlinVectorFinite(c, "uv_map", uv) ||
					!PerlinParameters(c, in, u, v, scale, scaling, amplitude))
					return false;
				const Float2 pos{in.Position[0] / in.Dimension[0], in.Position[1] / in.Dimension[1]};
				if (!PerlinVectorFinite(c, "position", pos)) return false;
				const Float2 delta{uv[0] - pos[0], uv[1] - pos[1]};
				if (in.Tile)
					st = {PerlinFract(delta[0]) * scale[0], PerlinFract(delta[1]) * scale[1]};
				else {
					const float angle = in.Rotation * .017453292519943295f, cosine = std::cos(angle),
								sine = std::sin(angle);
					st = {
						(delta[0] * cosine - delta[1] * sine) * scale[0],
						(delta[0] * sine + delta[1] * cosine) * scale[1]
					};
				}
				if (!PerlinVectorFinite(c, "scale", st)) return false;
			}
			constexpr std::array<Float2, 3> offsets{{{0, 0}, {1.7227f, 4.55529f}, {6.9950f, 6.82063f}}};
			constexpr std::array<std::string_view, 3> rangePorts{
				"color_r_range", "color_g_range", "color_b_range"
			};
			std::array<float, 3> channels{};
			const size_t count = in.Mode == 0 ? 1 : 3;
			for (size_t channel = 0; channel < count; ++channel) {
				const Float2 coordinate{st[0] + offsets[channel][0], st[1] + offsets[channel][1]};
				float value = 0;
				if (!PerlinOctaves(c, in, coordinate, scale, scaling, amplitude, value)) return false;
				channels[channel] =
					in.Mode == 0
						? value
						: in.Colors[channel][0] + value * (in.Colors[channel][1] - in.Colors[channel][0]);
				if (!PerlinFinite(
						c,
						in.Mode == 0 ? std::string_view("level_out") : rangePorts[channel],
						channels[channel]
					))
					return false;
			}
			if (in.Mode == 0) channels[1] = channels[2] = channels[0];
			if (in.Mode == 2) {
				constexpr std::array<float, 3> shifts{1.f, 2.f / 3.f, 1.f / 3.f};
				for (size_t channel = 0; channel < 3; ++channel) {
					const float p = std::abs(PerlinFract(channels[0] + shifts[channel]) * 6.f - 3.f);
					pixel[channel] = channels[2] * PerlinMix(1.f, std::clamp(p - 1.f, 0.f, 1.f), channels[1]);
					if (!PerlinFinite(c, "color_mode", float(pixel[channel]))) return false;
				}
			} else
				for (size_t channel = 0; channel < 3; ++channel)
					pixel[channel] = channels[channel];
			pixel[3] = alpha;
			return true;
		}
		bool QuotePerlin(NodeContext &c, const PerlinInputs &in, uint64_t &work) {
			const bool mask =
				in.Mask || std::any_of(c.ImageArrays.begin(), c.ImageArrays.end(), [](const auto &item) {
					return item.first == "mask" && item.second && !item.second->Images.empty();
				});
			if (!source2d::ComplexBatchAdmission(
					c, 1, "surface_out", "surface_out", 1 + size_t(mask) + size_t(c.NoiseFieldRequested)
				))
				return false;
			const uint64_t channels = in.Mode == 0 ? 1 : 3, iterations = uint64_t(std::max(in.Iteration, 0));
			const uint64_t perPixel = PERLIN_PIXEL_WORK + iterations * channels * PERLIN_OCTAVE_WORK,
						   pixels = uint64_t(in.Canvas.Width) * in.Canvas.Height;
			if (work > PERLIN_WORK_LIMIT || perPixel > PERLIN_WORK_LIMIT ||
				pixels > (PERLIN_WORK_LIMIT - work) / perPixel)
				return c.Fail(
					Status::LimitExceeded, "Perlin complete batch exceeds work limit", "surface_out"
				);
			work += pixels * perPixel;
			if (in.Iteration <= 0) return true;
			// grug scan mapped divisors and amplitude totals before publishing first selected row.
			for (uint32_t y = 0; y < in.Canvas.Height; ++y)
				for (uint32_t x = 0; x < in.Canvas.Width; ++x) {
					Float2 scale{};
					float scaling = 0, amplitude = 0;
					if (!PerlinParameters(
							c,
							in,
							(float(x) + .5f) / in.Canvas.Width,
							(float(y) + .5f) / in.Canvas.Height,
							scale,
							scaling,
							amplitude
						))
						return false;
					float weight = 1, total = 0;
					for (int32_t iteration = 0; iteration < in.Iteration; ++iteration) {
						if (in.Tile && (scale[0] == 0 || scale[1] == 0))
							return c.Fail(
								Status::UnsupportedExecution,
								"Perlin tiled lattice modulus divides by zero",
								"scale"
							);
						if (!PerlinVectorFinite(c, "scaling", scale) || !PerlinFinite(c, "amplitude", weight))
							return false;
						total += weight;
						if (in.Blend == 0 && !PerlinFinite(c, "amplitude", total)) return false;
						for (auto &value : scale)
							value *= scaling;
						weight *= amplitude;
					}
					if (in.Blend == 0 && total == 0)
						return c.Fail(
							Status::UnsupportedExecution,
							"Perlin Add amplitude total divides by zero",
							"amplitude"
						);
				}
			return true;
		}
		bool DrawPerlin(NodeContext &c) {
			ENGINE_PROFILE("imagegraph.source.perlin");
			PerlinInputs in;
			uint64_t work = 0;
			if (!PreparePerlin(c, in) || !QuotePerlin(c, in, work)) return false;
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
					if (!ShadePerlin(c, in, x, y, pixel)) return false;
					if (!WritePixel(*out, x, y, pixel))
						return c.Fail(
							Status::InvalidValue, "Perlin sample exceeds output storage range", "surface_out"
						);
					if (in.Mask) {
						pixel = ReadPixel(*out, x, y);
						const auto mask = SampleNearest(
							*in.Mask, (float(x) + .5f) / in.Canvas.Width, (float(y) + .5f) / in.Canvas.Height
						);
						const float brightness =
							(float(mask[0]) + float(mask[1]) + float(mask[2])) / 3.f * float(mask[3]);
						pixel[3] = float(pixel[3]) * brightness;
						if (!PerlinFinite(c, "mask", brightness) || !WritePixel(scratch, x, y, pixel))
							return c.Fail(
								Status::InvalidValue, "Perlin mask exceeds finite storage range", "mask"
							);
					}
				}
			if (in.Mask)
				for (uint32_t y = 0; y < in.Canvas.Height; ++y)
					for (uint32_t x = 0; x < in.Canvas.Width; ++x)
						if (!WritePixel(*out, x, y, ReadPixel(scratch, x, y)))
							return c.Fail(
								Status::InvalidValue, "Perlin mask copy exceeds storage range", "surface_out"
							);
			return c.FailureCode == Status::Ok;
		}
	}
	bool AdmitSourcePerlin(NodeContext &context, uint64_t &work) {
		PerlinInputs in;
		return PreparePerlin(context, in) && QuotePerlin(context, in, work);
	}
	std::span<const ExecutorEntry> SourcePerlinExecutors() {
		static constexpr ExecutorEntry entries[]{{"pc.perlin", DrawPerlin, true}};
		return entries;
	}
}
