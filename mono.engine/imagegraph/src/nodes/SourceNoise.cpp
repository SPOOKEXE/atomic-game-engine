#include "SourceNoise.hpp"

#include "Families.hpp"
#include "Source2DComplexGenerator.hpp"
#include "Source2DGenerator.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <vector>
namespace engine::imagegraph::detail {
	namespace {
		constexpr uint64_t NOISE_WORK_LIMIT = 64000000, NOISE_PIXEL_WORK = 512;
		struct NoiseInputs {
			uint32_t Width = 0, Height = 0;
			SurfaceFormat Format = SurfaceFormat::RGBA8Unorm;
			const Image *Uv = nullptr, *Mask = nullptr;
			float Seed = 0, UvMix = 1;
			std::array<float, 2> LevelIn{0, 1}, LevelOut{0, 1};
			std::array<std::array<float, 2>, 3> Ranges{{{0, 1}, {0, 1}, {0, 1}}};
			int Mode = 0;
		};
		bool NoiseFloat(NodeContext &context, std::string_view port, double value, float &out) {
			if (!std::isfinite(value) || std::abs(value) > double(std::numeric_limits<float>::max()))
				return context.Fail(Status::InvalidValue, "Noise value exceeds finite shader range", port);
			out = float(value);
			return true;
		}
		bool NoisePair(NodeContext &context, std::string_view port, std::array<float, 2> &out) {
			Vector2 value = context.Vec2(port, {0, 1});
			const auto *input = context.Find(port);
			if (input) {
				if (const auto *v = std::get_if<Vector3>(input))
					value = {v->X, v->Y};
				else if (const auto *v = std::get_if<Vector4>(input))
					value = {v->X, v->Y};
				else if (const auto *array = std::get_if<ArrayValue>(input)) {
					if (!array->Nested.empty() || !array->Items.empty())
						return context.Fail(
							Status::UnsupportedExecution, "Noise range getter shape is unrepresented", port
						);
					value = {0, 0};
					for (size_t index = 0; index < std::min(size_t(2), array->Elements.size()); ++index) {
						double number = 0;
						if (const auto *v = std::get_if<double>(&array->Elements[index]))
							number = *v;
						else if (const auto *v = std::get_if<int64_t>(&array->Elements[index]))
							number = double(*v);
						else
							return context.Fail(
								Status::InvalidValue, "Noise range needs numeric components", port
							);
						(index == 0 ? value.X : value.Y) = number;
					}
				} else if (!std::holds_alternative<Vector2>(*input) &&
						   !std::holds_alternative<double>(*input) &&
						   !std::holds_alternative<int64_t>(*input))
					return context.Fail(
						Status::UnsupportedExecution, "Noise range getter shape is unrepresented", port
					);
			}
			return NoiseFloat(context, port, value.X, out[0]) && NoiseFloat(context, port, value.Y, out[1]);
		}
		bool NoiseSurface(NodeContext &context, std::string_view port, const Image *surface) {
			if (const auto *value = context.Find(port); value && std::holds_alternative<AtlasValue>(*value))
				return context.Fail(
					Status::UnsupportedExecution, "Noise raw sampler binding rejects Atlas", port
				);
			return !surface ||
				   ValidSurfaceLayout(*surface, Limits::MaximumDimension, Limits::MaximumEvaluationBytes) ||
				   context.Fail(Status::InvalidValue, "Noise sampler layout is invalid", port);
		}
		bool PrepareNoise(NodeContext &context, NoiseInputs &inputs) {
			if (!context.Find("seed"))
				return context.Fail(
					Status::UnsupportedExecution, "Noise requires a resolved source seed", "seed"
				);
			inputs.Uv = context.Input("uv_map");
			inputs.Mask = context.Input("mask");
			if (!NoiseSurface(context, "uv_map", inputs.Uv) || !NoiseSurface(context, "mask", inputs.Mask))
				return false;
			if (!source2d::ResolveGeneratorDimensions(context, inputs.Mask, inputs.Width, inputs.Height))
				return false;
			if (inputs.Width > context.Request.MaximumImageDimension ||
				inputs.Height > context.Request.MaximumImageDimension)
				return context.Fail(
					Status::LimitExceeded, "Noise output exceeds request dimensions", "dimension"
				);
			const auto format = ResolveProcessorSurfaceFormat(context, nullptr);
			if (!format) return false;
			inputs.Format = *format;
			const auto mode = context.SourceChoice("color_mode");
			if (mode < 0 || mode > 2)
				return context.Fail(Status::InvalidValue, "Noise color mode is invalid", "color_mode");
			inputs.Mode = int(mode);
			if (!NoiseFloat(context, "seed", context.Scalar("seed"), inputs.Seed) ||
				!NoisePair(context, "level_in", inputs.LevelIn) ||
				!NoisePair(context, "level_out", inputs.LevelOut))
				return false;
			if (!std::isfinite(inputs.LevelIn[1] - inputs.LevelIn[0]))
				return context.Fail(
					Status::InvalidValue,
					"Noise input level difference exceeds finite shader range",
					"level_in"
				);
			if (inputs.LevelIn[0] == inputs.LevelIn[1])
				return context.Fail(
					Status::UnsupportedExecution,
					"Noise equal input levels divide by zero in the source shader",
					"level_in"
				);
			if (inputs.Mode != 0) {
				if (!NoisePair(context, "color_r_range", inputs.Ranges[0]) ||
					!NoisePair(context, "color_g_range", inputs.Ranges[1]) ||
					!NoisePair(context, "color_b_range", inputs.Ranges[2]))
					return false;
			}
			if (inputs.Uv && !NoiseFloat(context, "uv_mix", context.Scalar("uv_mix", 1), inputs.UvMix))
				return false;
			return context.FailureCode == Status::Ok;
		}
		float Fract(float value) {
			return value - std::floor(value);
		}
		bool NoiseRandom(NodeContext &context, float x, float y, float seed, float &out) {
			const float wrapped = seed - 100000.f * std::floor(seed / 100000.f);
			const float offset = wrapped / 10.f;
			const float dot = (x + offset) * 1892.9898f + (y + offset) * 78.23453f;
			if (!std::isfinite(dot))
				return context.Fail(Status::InvalidValue, "Noise hash exceeds finite shader range", "uv_map");
			out = Fract(std::sin(dot) * 437.54123f);
			return true;
		}
		bool NoiseLevel(NodeContext &context, const NoiseInputs &inputs, float x, float y, float &out) {
			const float base = std::floor(inputs.Seed), fraction = Fract(inputs.Seed);
			float first = 0, second = 0;
			if (!NoiseRandom(context, x, y, base / 5000.f, first) ||
				!NoiseRandom(context, x, y, (base + 1.f) / 5000.f, second))
				return false;
			const float value = first * (1.f - fraction) + second * fraction;
			const float progress = (value - inputs.LevelIn[0]) / (inputs.LevelIn[1] - inputs.LevelIn[0]);
			out = inputs.LevelOut[0] * (1.f - progress) + inputs.LevelOut[1] * progress;
			return std::isfinite(out) ||
				   context.Fail(
					   Status::InvalidValue, "Noise level remapping exceeds finite shader range", "level_out"
				   );
		}
		bool
		ShadeNoise(NodeContext &context, const NoiseInputs &inputs, uint32_t x, uint32_t y, Rgba &result) {
			float u = (float(x) + .5f) / float(inputs.Width), v = (float(y) + .5f) / float(inputs.Height),
				  alpha = 1;
			if (inputs.Uv) {
				const auto sample = SampleNearest(*inputs.Uv, u, v);
				const float mapX = float(sample[0]), mapY = 1.f - float(sample[1]);
				alpha = float(sample[3]);
				u = u * (1.f - inputs.UvMix) + mapX * inputs.UvMix;
				v = v * (1.f - inputs.UvMix) + mapY * inputs.UvMix;
				if (!std::isfinite(u) || !std::isfinite(v) || !std::isfinite(alpha))
					return context.Fail(
						Status::InvalidValue, "Noise UV mixing exceeds finite shader range", "uv_map"
					);
			}
			std::array<float, 3> channels{};
			constexpr std::array<std::array<float, 2>, 3> offsets{
				{{0, 0}, {1.7227f, 4.55529f}, {6.9950f, 6.82063f}}
			};
			const int count = inputs.Mode == 0 ? 1 : 3;
			for (int channel = 0; channel < count; ++channel) {
				float value;
				if (!NoiseLevel(context, inputs, u + offsets[channel][0], v + offsets[channel][1], value))
					return false;
				channels[channel] = inputs.Mode == 0
										? value
										: inputs.Ranges[channel][0] +
											  value * (inputs.Ranges[channel][1] - inputs.Ranges[channel][0]);
				if (!std::isfinite(channels[channel]))
					return context.Fail(
						Status::InvalidValue,
						"Noise color range exceeds finite shader range",
						channel == 0   ? "color_r_range"
						: channel == 1 ? "color_g_range"
									   : "color_b_range"
					);
			}
			if (inputs.Mode == 0) channels[1] = channels[2] = channels[0];
			if (inputs.Mode == 2) {
				constexpr std::array<float, 3> shifts{1.f, 2.f / 3.f, 1.f / 3.f};
				for (size_t channel = 0; channel < 3; ++channel) {
					const float p = std::abs(Fract(channels[0] + shifts[channel]) * 6.f - 3.f);
					result[channel] =
						channels[2] * ((1.f - channels[1]) + std::clamp(p - 1.f, 0.f, 1.f) * channels[1]);
				}
			} else
				for (size_t channel = 0; channel < 3; ++channel)
					result[channel] = channels[channel];
			result[3] = alpha;
			for (double value : result)
				if (!std::isfinite(value))
					return context.Fail(
						Status::InvalidValue, "Noise HSV conversion exceeds finite shader range", "color_mode"
					);
			const bool half =
				inputs.Format == SurfaceFormat::RGBA16Float || inputs.Format == SurfaceFormat::R16Float;
			const bool full =
				inputs.Format == SurfaceFormat::RGBA32Float || inputs.Format == SurfaceFormat::R32Float;
			if (half || full)
				for (size_t channel = 0; channel < DescribeSurfaceFormat(inputs.Format)->Channels; ++channel)
					if (std::abs(result[channel]) >
						(half ? 65504. : double(std::numeric_limits<float>::max())))
						return context.Fail(
							Status::InvalidValue, "Noise sample exceeds output storage range", "surface_out"
						);
			return true;
		}
		bool QuoteNoise(NodeContext &context, const NoiseInputs &inputs, uint64_t &work) {
			const bool maskScratch =
				inputs.Mask ||
				std::any_of(context.ImageArrays.begin(), context.ImageArrays.end(), [](const auto &input) {
					return input.first == "mask" && input.second && !input.second->Images.empty();
				});
			// grug reserve a conservative whole-batch target quote, including optional mask scratch and field
			// copies.
			if (!source2d::ComplexBatchAdmission(
					context,
					1,
					"surface_out",
					"surface_out",
					1 + size_t(maskScratch) + size_t(context.NoiseFieldRequested)
				))
				return false;
			const uint64_t pixels = uint64_t(inputs.Width) * inputs.Height;
			if (work > NOISE_WORK_LIMIT || pixels > (NOISE_WORK_LIMIT - work) / NOISE_PIXEL_WORK)
				return context.Fail(
					Status::LimitExceeded, "Noise complete batch exceeds work limit", "surface_out"
				);
			work += pixels * NOISE_PIXEL_WORK;
			return true;
		}
		bool ValidateNoiseSamples(NodeContext &context, const NoiseInputs &inputs) {
			Rgba sample{};
			for (uint32_t y = 0; y < inputs.Height; ++y)
				for (uint32_t x = 0; x < inputs.Width; ++x) {
					if (!ShadeNoise(context, inputs, x, y, sample)) return false;
					if (inputs.Mask) {
						const auto mask = SampleNearest(
							*inputs.Mask, (float(x) + .5f) / inputs.Width, (float(y) + .5f) / inputs.Height
						);
						const float brightness =
							(float(mask[0]) + float(mask[1]) + float(mask[2])) / 3.f * float(mask[3]);
						// grug stored alpha is quantized before masking; raw shader alpha can exceed it.
						if (!std::isfinite(brightness))
							return context.Fail(
								Status::InvalidValue, "Noise mask exceeds finite shader range", "mask"
							);
					}
				}
			return true;
		}
		bool DrawNoise(NodeContext &context) {
			ENGINE_PROFILE("imagegraph.source.noise");
			NoiseInputs inputs;
			uint64_t work = 0;
			if (!PrepareNoise(context, inputs) || !QuoteNoise(context, inputs, work) ||
				!ValidateNoiseSamples(context, inputs))
				return false;
			const uint64_t bytes = uint64_t(inputs.Width) * inputs.Height * 4;
			auto charge = context.ReserveWorkspace(inputs.Mask ? bytes : 0, "surface_out");
			if (!charge) return false;
			Image masked;
			if (inputs.Mask)
				masked = {
					inputs.Width,
					inputs.Height,
					std::vector<uint8_t>(size_t(bytes)),
					0,
					SurfaceFormat::RGBA8Unorm
				};
			auto *output = context.NewImage("surface_out", inputs.Width, inputs.Height, inputs.Format);
			if (!output) return false;
			for (uint32_t y = 0; y < inputs.Height; ++y)
				for (uint32_t x = 0; x < inputs.Width; ++x) {
					Rgba pixel{};
					if (!ShadeNoise(context, inputs, x, y, pixel)) return false;
					if (!WritePixel(*output, x, y, pixel))
						return context.Fail(
							Status::InvalidValue, "Noise sample exceeds output storage range", "surface_out"
						);
					if (inputs.Mask) {
						pixel = ReadPixel(*output, x, y);
						const auto mask = SampleNearest(
							*inputs.Mask, (float(x) + .5f) / inputs.Width, (float(y) + .5f) / inputs.Height
						);
						const float brightness =
							(float(mask[0]) + float(mask[1]) + float(mask[2])) / 3.f * float(mask[3]);
						pixel[3] = float(pixel[3]) * brightness;
						if (!WritePixel(masked, x, y, pixel))
							return context.Fail(
								Status::InvalidValue, "Noise mask exceeds finite storage range", "mask"
							);
					}
				}
			if (inputs.Mask)
				for (uint32_t y = 0; y < inputs.Height; ++y)
					for (uint32_t x = 0; x < inputs.Width; ++x)
						if (!WritePixel(*output, x, y, ReadPixel(masked, x, y)))
							return context.Fail(
								Status::InvalidValue, "Noise masked copy exceeds storage range", "surface_out"
							);
			return context.FailureCode == Status::Ok;
		}
	}
	bool AdmitSourceNoise(NodeContext &context, uint64_t &batchWork) {
		NoiseInputs inputs;
		return PrepareNoise(context, inputs) && QuoteNoise(context, inputs, batchWork);
	}
	std::span<const ExecutorEntry> SourceNoiseExecutors() {
		static constexpr ExecutorEntry entries[]{{"pc.noise", DrawNoise, true}};
		return entries;
	}
}
