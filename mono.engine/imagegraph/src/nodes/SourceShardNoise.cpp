#include "SourceShardNoise.hpp"

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
		constexpr uint64_t SHARD_WORK_LIMIT = 64000000, SHARD_BASE_WORK = 512, SHARD_SAMPLE_WORK = 27 * 512;
		constexpr float SHARD_TAU = 6.283185307179586f, SHARD_RADIANS = .017453292519943295f;
		using Float2 = std::array<float, 2>;
		using Float3 = std::array<float, 3>;
		struct ShardInputs {
			source2d::ComplexCanvas Canvas;
			SurfaceFormat Format = SurfaceFormat::RGBA8Unorm;
			Float2 Dimension{}, Position{}, Scale{4, 4}, Progress{0, 0}, Sharpness{0, 1}, LevelIn{0, 1},
				LevelOut{0, 1};
			const Image *Uv = nullptr, *Mask = nullptr, *ScaleMap = nullptr, *ProgressMap = nullptr,
						*SharpnessMap = nullptr;
			float Seed = 0, Rotation = 0, UvMix = 1;
			bool Covered = false;
		};
		bool ShardFinite(NodeContext &c, std::string_view port, float value) {
			return std::isfinite(value) ||
				   c.Fail(Status::InvalidValue, "Shard Noise shader arithmetic exceeds finite range", port);
		}
		bool ShardFloat(NodeContext &c, std::string_view port, double value, float &out) {
			if (!std::isfinite(value) || std::abs(value) > double(std::numeric_limits<float>::max()))
				return c.Fail(Status::InvalidValue, "Shard Noise value exceeds shader float range", port);
			out = float(value);
			return true;
		}
		bool ShardPair(NodeContext &c, std::string_view port, Vector2 value, Float2 &out) {
			return ShardFloat(c, port, value.X, out[0]) && ShardFloat(c, port, value.Y, out[1]);
		}
		bool ShardSurface(NodeContext &c, std::string_view port, const Image *image) {
			if (const auto *value = c.Find(port); value && std::holds_alternative<AtlasValue>(*value))
				return c.Fail(
					Status::UnsupportedExecution, "Shard Noise raw sampler binding rejects Atlas", port
				);
			return !image ||
				   ValidSurfaceLayout(*image, Limits::MaximumDimension, Limits::MaximumEvaluationBytes) ||
				   c.Fail(Status::InvalidValue, "Shard Noise sampler layout is invalid", port);
		}
		bool ShardMappedControl(
			NodeContext &c, std::string_view port, std::string_view mapPort, Float2 &range, const Image *&map
		) {
			Vector2 value;
			if (!ReadSourceMappedRange(c, port, value)) return false;
			map = c.Input(mapPort);
			return ShardSurface(c, mapPort, map) && ShardPair(c, port, value, range);
		}
		bool PrepareShard(NodeContext &c, ShardInputs &in) {
			in.Uv = c.Input("uv_map");
			in.Mask = c.Input("mask");
			if (!ShardSurface(c, "uv_map", in.Uv) || !ShardSurface(c, "mask", in.Mask) ||
				!source2d::ResolveGeneratorDimensions(c, in.Mask, in.Canvas.Width, in.Canvas.Height))
				return false;
			if (in.Canvas.Width > c.Request.MaximumImageDimension ||
				in.Canvas.Height > c.Request.MaximumImageDimension)
				return c.Fail(
					Status::LimitExceeded, "Shard Noise output exceeds request dimensions", "dimension"
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
			if (!ShardPair(c, "dimension", in.Canvas.Raw, in.Dimension)) return false;
			const auto format = ResolveProcessorSurfaceFormat(c, nullptr);
			if (!format) return false;
			in.Format = *format;
			in.Covered = in.Dimension[0] > .5f && in.Dimension[1] > .5f;
			// grug clear target and absent fragments do not consume shader uniforms.
			if (!in.Covered) return c.FailureCode == Status::Ok;
			// grug unmapped source leaves UseSurf stale. scalar controls also upload one lane into vec2.
			for (const std::string_view port :
				 {std::string_view("progress"), std::string_view("sharpness"), std::string_view("scale")})
				if (!SourceRangeMapped(c, port))
					return c.Fail(
						Status::UnsupportedExecution,
						"Shard Noise unmapped control needs unresolved source shader uniform state",
						port
					);
			if (!c.Find("seed"))
				return c.Fail(
					Status::UnsupportedExecution, "Shard Noise requires a resolved source seed", "seed"
				);
			if (!ShardFloat(c, "seed", c.Scalar("seed"), in.Seed) ||
				!ShardFloat(c, "rotation", c.Scalar("rotation"), in.Rotation))
				return false;
			if (in.Uv && !ShardFloat(c, "uv_mix", c.Scalar("uv_mix", 1), in.UvMix)) return false;
			Vector2 position;
			if (!source2d::ReferenceVector(c, "position", in.Canvas.Raw, position) ||
				!ShardPair(c, "position", position, in.Position) ||
				!ShardPair(c, "level_in", c.Vec2("level_in", {0, 1}), in.LevelIn) ||
				!ShardPair(c, "level_out", c.Vec2("level_out", {0, 1}), in.LevelOut))
				return false;
			if (in.LevelIn[0] == in.LevelIn[1])
				return c.Fail(
					Status::UnsupportedExecution, "Shard Noise equal input levels divide by zero", "level_in"
				);
			if (!ShardFinite(c, "level_in", in.LevelIn[1] - in.LevelIn[0])) return false;
			const auto *original = source2d::GeneratorOriginal(c, "scale");
			const auto *array = original ? std::get_if<ArrayValue>(original) : nullptr;
			if (array && (!array->Nested.empty() || !array->Items.empty() ||
						  std::any_of(array->Elements.begin(), array->Elements.end(), [](const auto &value) {
							  return !std::holds_alternative<double>(value) &&
									 !std::holds_alternative<int64_t>(value);
						  })))
				return c.Fail(
					Status::UnsupportedExecution,
					"Shard Noise mapped Scale nested source upload is not a defined vec2 uniform",
					"scale"
				);
			if (!ShardMappedControl(c, "scale", "scale_map", in.Scale, in.ScaleMap) ||
				!ShardMappedControl(c, "progress", "progress_map", in.Progress, in.ProgressMap) ||
				!ShardMappedControl(c, "sharpness", "sharpness_map", in.Sharpness, in.SharpnessMap))
				return false;
			return c.FailureCode == Status::Ok;
		}
		float ShardFract(float value) {
			return value - std::floor(value);
		}
		float ShardMod(float value, float divisor) {
			return value - divisor * std::floor(value / divisor);
		}
		float ShardMix(float low, float high, float weight) {
			return low * (1.f - weight) + high * weight;
		}
		float ShardDot(Float3 a, Float3 b) {
			return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
		}
		bool ShardVectorFinite(NodeContext &c, std::string_view port, Float3 value) {
			for (float lane : value)
				if (!ShardFinite(c, port, lane)) return false;
			return true;
		}
		float ShardMapped(Float2 range, const Image *map, float u, float v) {
			if (!map) return range[0];
			const auto sample = SampleNearest(*map, u, v);
			const float amount = (float(sample[0]) + float(sample[1]) + float(sample[2])) / 3.f;
			return ShardMix(range[0], range[1], amount);
		}
		bool ShardParameters(
			NodeContext &c,
			const ShardInputs &in,
			float u,
			float v,
			Float2 &scale,
			float &progress,
			float &sharpness
		) {
			scale = in.Scale;
			if (in.ScaleMap) {
				const float value = ShardMapped(in.Scale, in.ScaleMap, u, v);
				scale = {value, value};
			}
			progress = ShardMapped(in.Progress, in.ProgressMap, u, v);
			sharpness = ShardMapped(in.Sharpness, in.SharpnessMap, u, v);
			if (!ShardFinite(c, "scale", scale[0]) || !ShardFinite(c, "scale", scale[1]) ||
				!ShardFinite(c, "progress", progress) || !ShardFinite(c, "sharpness", sharpness))
				return false;
			if (sharpness < 0)
				return c.Fail(
					Status::UnsupportedExecution,
					"Shard Noise negative sharpness has an undefined GLSL power base",
					"sharpness"
				);
			sharpness = std::pow(sharpness, 2.f) * 20.f;
			return ShardFinite(c, "sharpness", sharpness);
		}
		bool ShardHash(NodeContext &c, float seed, Float3 point, Float3 &hash) {
			const float offset = ShardMod(seed, 10000.f) / 100.f;
			constexpr std::array<Float3, 3> coefficients{
				{{127.1324f, 311.7874f, 829.3683f},
				 {269.8355f, 183.3961f, 614.5965f},
				 {615.2689f, 264.1657f, 278.1687f}}
			};
			constexpr Float3 factors{152.6178612f, 437.5453123f, 962.6718165f};
			for (size_t lane = 0; lane < 3; ++lane) {
				const float angle = ShardDot(point, coefficients[lane]) * (factors[lane] + offset);
				if (!ShardFinite(c, "seed", angle)) return false;
				hash[lane] = ShardFract(std::sin(angle) * 43758.5453f);
			}
			return ShardVectorFinite(c, "seed", hash);
		}
		// grug keep both hashes per cell and source inverse-square-root tanh form.
		bool ShardNoise(NodeContext &c, const ShardInputs &in, Float3 point, float sharpness, float &out) {
			if (!ShardVectorFinite(c, "scale", point)) return false;
			Float3 lattice{}, fraction{};
			for (size_t lane = 0; lane < 3; ++lane) {
				lattice[lane] = std::floor(point[lane]);
				fraction[lane] = ShardFract(point[lane]);
			}
			float value = 0, total = 0;
			for (int z = -1; z <= 1; ++z)
				for (int y = -1; y <= 1; ++y)
					for (int x = -1; x <= 1; ++x) {
						const Float3 neighbor{float(x), float(y), float(z)};
						Float3 key{}, hash{};
						for (size_t lane = 0; lane < 3; ++lane)
							key[lane] = lattice[lane] + neighbor[lane];
						if (!ShardHash(c, in.Seed, key, hash)) return false;
						Float3 delta{};
						for (size_t lane = 0; lane < 3; ++lane)
							delta[lane] = fraction[lane] - (neighbor[lane] + hash[lane]);
						const float exponent = -SHARD_TAU * ShardDot(delta, delta);
						if (!ShardFinite(c, "scale", exponent)) return false;
						const float weight = std::exp2(exponent);
						const Float3 shifted{key[0] + 11.f, key[1] + 31.f, key[2] + 47.f};
						if (!ShardHash(c, in.Seed, shifted, hash)) return false;
						for (auto &lane : hash)
							lane -= .5f;
						const float slope = sharpness * ShardDot(delta, hash), square = 1.f + slope * slope;
						if (!ShardFinite(c, "sharpness", slope) || !ShardFinite(c, "sharpness", square))
							return false;
						value += weight * slope * (1.f / std::sqrt(square));
						total += weight;
						if (!ShardFinite(c, "sharpness", value) || !ShardFinite(c, "scale", total))
							return false;
					}
			if (total == 0)
				return c.Fail(
					Status::UnsupportedExecution, "Shard Noise weight total divides by zero", "scale"
				);
			out = (value / total) * .5f + .5f;
			return ShardFinite(c, "sharpness", out);
		}
		bool ShadeShard(NodeContext &c, const ShardInputs &in, uint32_t x, uint32_t y, Rgba &pixel) {
			if (!in.Covered || float(x) + .5f >= in.Dimension[0] || float(y) + .5f >= in.Dimension[1]) {
				pixel = {0, 0, 0, 0};
				return true;
			}
			const float u = (float(x) + .5f) / in.Dimension[0], v = (float(y) + .5f) / in.Dimension[1];
			Float2 scale{};
			float progress = 0, sharpness = 0;
			if (!ShardParameters(c, in, u, v, scale, progress, sharpness)) return false;
			Float2 uv{u, v};
			float alpha = 1;
			if (in.Uv) {
				const auto sample = SampleNearest(*in.Uv, u, v);
				uv = {ShardMix(u, float(sample[0]), in.UvMix), ShardMix(v, 1.f - float(sample[1]), in.UvMix)};
				alpha = float(sample[3]);
			}
			if (!ShardFinite(c, "uv_map", uv[0]) || !ShardFinite(c, "uv_map", uv[1]) ||
				!ShardFinite(c, "uv_map", alpha))
				return false;
			const Float2 ntx{uv[0], uv[1] * (in.Dimension[1] / in.Dimension[0])};
			const Float2 delta{
				ntx[0] - in.Position[0] / in.Dimension[0], ntx[1] - in.Position[1] / in.Dimension[1]
			};
			if (!ShardFinite(c, "position", delta[0]) || !ShardFinite(c, "position", delta[1])) return false;
			const float angle = in.Rotation * SHARD_RADIANS;
			if (!ShardFinite(c, "rotation", angle)) return false;
			const float cosine = std::cos(angle), sine = std::sin(angle);
			const Float2 position{
				(delta[0] * cosine - delta[1] * sine) * scale[0] / 16.f,
				(delta[0] * sine + delta[1] * cosine) * scale[1] / 16.f
			};
			progress /= 100.f;
			const Float3 point{
				16.f * (position[0] + progress), 16.f * (position[1] + progress), 16.f * (progress * .5f)
			};
			float value = 0;
			if (!ShardNoise(c, in, point, sharpness, value)) return false;
			const float ratio = (value - in.LevelIn[0]) / (in.LevelIn[1] - in.LevelIn[0]);
			value = ShardMix(in.LevelOut[0], in.LevelOut[1], ratio);
			if (!ShardFinite(c, "level_out", value)) return false;
			pixel = {value, value, value, alpha};
			return true;
		}
		bool QuoteShard(NodeContext &c, const ShardInputs &in, uint64_t &work) {
			const bool mask =
				in.Mask || std::any_of(c.ImageArrays.begin(), c.ImageArrays.end(), [](const auto &input) {
					return input.first == "mask" && input.second && !input.second->Images.empty();
				});
			if (!source2d::ComplexBatchAdmission(
					c, 1, "surface_out", "surface_out", 1 + size_t(mask) + size_t(c.NoiseFieldRequested)
				))
				return false;
			const uint64_t perPixel = SHARD_BASE_WORK + (in.Covered ? SHARD_SAMPLE_WORK : 0),
						   pixels = uint64_t(in.Canvas.Width) * in.Canvas.Height;
			if (work > SHARD_WORK_LIMIT || pixels > (SHARD_WORK_LIMIT - work) / perPixel)
				return c.Fail(
					Status::LimitExceeded, "Shard Noise complete batch exceeds work limit", "surface_out"
				);
			work += pixels * perPixel;
			if (!in.Covered) return true;
			// grug mapped sharpness power domain is checked for all covered pixels before first output.
			for (uint32_t y = 0; y < in.Canvas.Height; ++y)
				for (uint32_t x = 0; x < in.Canvas.Width; ++x) {
					if (float(x) + .5f >= in.Dimension[0] || float(y) + .5f >= in.Dimension[1]) continue;
					Float2 scale{};
					float progress = 0, sharpness = 0;
					if (!ShardParameters(
							c,
							in,
							(float(x) + .5f) / in.Dimension[0],
							(float(y) + .5f) / in.Dimension[1],
							scale,
							progress,
							sharpness
						))
						return false;
				}
			return true;
		}
		bool DrawShard(NodeContext &c) {
			ENGINE_PROFILE("imagegraph.source.shard_noise");
			ShardInputs in;
			uint64_t work = 0;
			if (!PrepareShard(c, in) || !QuoteShard(c, in, work)) return false;
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
					if (!ShadeShard(c, in, x, y, pixel)) return false;
					if (!WritePixel(*out, x, y, pixel))
						return c.Fail(
							Status::InvalidValue,
							"Shard Noise sample exceeds output storage range",
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
						if (!ShardFinite(c, "mask", brightness) || !WritePixel(scratch, x, y, pixel))
							return c.Fail(
								Status::InvalidValue, "Shard Noise mask exceeds finite storage range", "mask"
							);
					}
				}
			if (in.Mask)
				for (uint32_t y = 0; y < in.Canvas.Height; ++y)
					for (uint32_t x = 0; x < in.Canvas.Width; ++x)
						if (!WritePixel(*out, x, y, ReadPixel(scratch, x, y)))
							return c.Fail(
								Status::InvalidValue,
								"Shard Noise mask copy exceeds storage range",
								"surface_out"
							);
			return c.FailureCode == Status::Ok;
		}
	}
	bool AdmitSourceShardNoise(NodeContext &context, uint64_t &work) {
		ShardInputs in;
		return PrepareShard(context, in) && QuoteShard(context, in, work);
	}
	std::span<const ExecutorEntry> SourceShardNoiseExecutors() {
		static constexpr ExecutorEntry entries[]{{"pc.shard_noise", DrawShard, true}};
		return entries;
	}
}
