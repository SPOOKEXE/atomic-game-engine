#include "Families.hpp"
#include "Sampler.hpp"
#include "SourceNormalShaderCurve.hpp"

#include <numbers>

// Pixel Composer b69eca232217360cf1502ef0223523d818606652 normal processors and shaders.
// Each intermediate write preserves the source surface format; undefined shader math is refused.

namespace engine::imagegraph::detail {
	namespace {
		using NormalVector = std::array<float, 3>;
		float NormalDot(const NormalVector &a, const NormalVector &b) {
			return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
		}
		bool NormalUnit(NormalVector &value) {
			const float length = std::sqrt(NormalDot(value, value));
			if (!std::isfinite(length) || length == 0) return false;
			for (float &component : value)
				component /= length;
			return true;
		}
		NormalVector NormalSubtract(const NormalVector &a, const NormalVector &b) {
			return {a[0] - b[0], a[1] - b[1], a[2] - b[2]};
		}
		std::array<float, 4> NormalFloatPixel(const Image &source, float u, float v) {
			const auto pixel = SampleNearest(source, u, v);
			return {float(pixel[0]), float(pixel[1]), float(pixel[2]), float(pixel[3])};
		}
		bool NormalStore(
			NodeContext &context, Image &image, uint32_t x, uint32_t y, const std::array<float, 4> &pixel
		) {
			if (WritePixel(image, x, y, {pixel[0], pixel[1], pixel[2], pixel[3]})) return true;
			return context.Fail(
				Status::UnsupportedExecution, "source normal shader produced a nonfinite pixel", "surface_out"
			);
		}
		bool NormalWork(NodeContext &context, uint64_t pixels, uint64_t samples) {
			if (samples && pixels > 64'000'000 / samples)
				return context.Fail(
					Status::LimitExceeded, "normal shader exceeds bounded sample work", "surface_out"
				);
			return true;
		}
		struct NormalTransform {
			float Cosine = 1, Sine = 0;
			std::array<float, 2> Scale{1, 1}, Anchor{.5f, .5f}, Position{};
		};
		bool ReadNormalTransform(
			NodeContext &context, int index, const Image &source, NormalTransform &transform
		) {
			const std::string suffix = "_" + std::to_string(index);
			const auto position = UnitVector(context, "position" + suffix, source.Width, source.Height);
			const auto anchor = context.Vec2("anchor" + suffix, {.5, .5}),
					   scale = context.Vec2("scale" + suffix, {1, 1});
			const float rotation = float(context.Scalar("rotation" + suffix) * std::numbers::pi / 180);
			transform.Cosine = std::cos(rotation);
			transform.Sine = std::sin(rotation);
			transform.Scale = {float(scale.X), float(scale.Y)};
			transform.Anchor = {float(anchor.X), float(anchor.Y)};
			transform.Position = {float(position.X / source.Width), float(position.Y / source.Height)};
			for (float value :
				 {rotation,
				  transform.Scale[0],
				  transform.Scale[1],
				  transform.Anchor[0],
				  transform.Anchor[1],
				  transform.Position[0],
				  transform.Position[1]})
				if (!std::isfinite(value))
					return context.Fail(
						Status::UnsupportedExecution,
						"normal transform exceeds finite shader uniforms",
						"scale" + suffix
					);
			if (transform.Scale[0] == 0 || transform.Scale[1] == 0)
				return context.Fail(
					Status::UnsupportedExecution,
					"source normal transform divides by zero scale",
					"scale" + suffix
				);
			return context.FailureCode == Status::Ok;
		}
		std::array<float, 2> NormalCoordinates(const NormalTransform &transform, float u, float v) {
			const float x = u - transform.Anchor[0], y = v - transform.Anchor[1];
			// Source uses row-vector multiplication by mat2(cos,sin,-sin,cos).
			return {
				(x * transform.Cosine + y * transform.Sine) / transform.Scale[0] + transform.Anchor[0] -
					transform.Position[0],
				(-x * transform.Sine + y * transform.Cosine) / transform.Scale[1] + transform.Anchor[1] -
					transform.Position[1]
			};
		}
		bool NormalBlend(NodeContext &context) {
			const auto *source = context.Input("normal_1"), *second = context.Input("surface_2"),
					   *mask = context.Input("mask");
			if (!source)
				return context.Fail(
					Status::TypeMismatch, "normal blend requires its first surface", "normal_1"
				);
			const auto format = ResolveProcessorSurfaceFormat(context, source);
			if (!format || !NormalWork(context, uint64_t(source->Width) * source->Height, 3)) return false;
			NormalTransform firstTransform, secondTransform;
			if (!ReadNormalTransform(context, 1, *source, firstTransform) ||
				(second && !ReadNormalTransform(context, 2, *source, secondTransform)))
				return false;
			const int64_t mode = context.Integer("blend_mode");
			const float intensity = float(context.Scalar("intensity", 1));
			if (!std::isfinite(intensity))
				return context.Fail(
					Status::UnsupportedExecution,
					"normal intensity exceeds finite shader uniforms",
					"intensity"
				);
			Image *output = context.NewImage("surface_out", source->Width, source->Height, *format);
			if (!output) return false;
			for (uint32_t y = 0; y < output->Height; y++)
				for (uint32_t x = 0; x < output->Width; x++) {
					const float u = float((x + .5) / output->Width), v = float((y + .5) / output->Height);
					const auto uv1 = NormalCoordinates(firstTransform, u, v);
					auto result = NormalFloatPixel(*source, uv1[0], uv1[1]);
					if (second) {
						const auto uv2 = NormalCoordinates(secondTransform, u, v);
						const auto foreground = NormalFloatPixel(*second, uv2[0], uv2[1]);
						if (foreground[3] != 0) {
							NormalVector original{result[0] - .5f, result[1] - .5f, result[2] - 1},
								incoming{foreground[0] - .5f, foreground[1] - .5f, foreground[2] - 1},
								normal = original;
							for (size_t component = 0; component < 3; component++) {
								if (mode == 0)
									normal[component] = original[component] + incoming[component];
								else if (mode == 1)
									normal[component] = std::max(original[component], incoming[component]);
								else if (mode == 3)
									normal[component] = original[component] - incoming[component];
								else if (mode == 4)
									normal[component] = std::min(original[component], incoming[component]);
								else if (mode == 6)
									normal[component] = incoming[component];
								normal[component] =
									original[component] * (1 - intensity) + normal[component] * intensity;
								normal[component] += component == 2 ? 1.f : .5f;
							}
							if (mask) {
								const auto sample = NormalFloatPixel(*mask, u, v);
								const float amount = (sample[0] + sample[1] + sample[2]) / 3 * sample[3];
								// This source mask mixes the offset-removed original with the packed result.
								for (size_t component = 0; component < 3; component++)
									normal[component] =
										original[component] * (1 - amount) + normal[component] * amount;
							}
							// Source renormalizes packed RGB, rather than decoding a signed normal first.
							if (context.Boolean("normalize", true) && !NormalUnit(normal))
								return context.Fail(
									Status::UnsupportedExecution,
									"source normal blend normalizes a zero vector",
									"normalize"
								);
							result = {normal[0], normal[1], normal[2], 1};
						}
					}
					if (!NormalStore(context, *output, x, y, result)) return false;
				}
			return context.FailureCode == Status::Ok;
		}

		bool NormalHeight(NodeContext &context) {
			const auto *source = context.Input("normal_in");
			if (!source)
				return context.Fail(
					Status::TypeMismatch, "normal integration requires a source surface", "normal_in"
				);
			const auto format = ResolveProcessorSurfaceFormat(context, source);
			if (!format) return false;
			const int64_t authored = context.Integer("max_itr", -1);
			const uint64_t iterations = authored == -1 ? uint64_t(std::max(source->Width, source->Height)) * 2
													   : uint64_t(std::max<int64_t>(authored, 0));
			const uint64_t sweep = uint64_t(context.Integer("sweep_direction", 1)) & 15;
			const float intensity = float(context.Scalar("normal_height", 1));
			const double rawBase = context.Scalar("base_height");
			if (!std::isfinite(intensity) || !std::isfinite(rawBase) || std::abs(rawBase * 255) > INT32_MAX)
				return context.Fail(
					Status::UnsupportedExecution,
					"normal integration controls exceed defined numeric conversion",
					"base_height"
				);
			if (iterations && sweep && intensity == 0)
				return context.Fail(
					Status::UnsupportedExecution,
					"source normal integration divides by zero height",
					"normal_height"
				);
			uint64_t active = 0;
			for (uint64_t bit = 1; bit <= 8; bit <<= 1)
				active += (sweep & bit) != 0;
			if (iterations > 64'000'000 / (1 + active * 2) ||
				!NormalWork(
					context, uint64_t(source->Width) * source->Height, iterations * (1 + active * 2) + 1
				))
				return context.Fail(
					Status::LimitExceeded, "normal integration exceeds bounded iterations", "max_itr"
				);
			Image *output = context.NewImage("surface_out", source->Width, source->Height, *format);
			if (!output) return false;
			const auto layout = CheckedSurfaceLayout(
				source->Width, source->Height, SurfaceFormat::R16Float, Limits::MaximumOutputBytes
			);
			if (!layout)
				return context.Fail(
					Status::LimitExceeded, "normal integration scratch exceeds native bounds", "normal_in"
				);
			auto charge = context.ReserveWorkspace(layout->Bytes * 2, "normal_in");
			if (!charge) return false;
			std::array<Image, 2> scratch;
			// make_color_grey rounds to an integer before packing RGB, including wraparound.
			const double lower = std::floor(rawBase * 255), fraction = rawBase * 255 - lower;
			const double rounded = lower + (fraction > .5 || (fraction == .5 && std::fmod(lower, 2.0) != 0));
			double byte = std::fmod(rounded, 256.0);
			if (byte < 0) byte += 256;
			const float base = float(byte / 255);
			for (auto &image : scratch) {
				image.Width = source->Width;
				image.Height = source->Height;
				image.Format = SurfaceFormat::R16Float;
				image.Pixels.resize(size_t(layout->Bytes));
				for (uint32_t y = 0; y < image.Height; y++)
					for (uint32_t x = 0; x < image.Width; x++)
						if (!WritePixel(image, x, y, {base, base, base, 1}))
							return context.Fail(
								Status::InvalidValue,
								"normal height scratch initialization failed",
								"base_height"
							);
			}
			for (uint64_t iteration = 0; iteration < iterations; iteration++) {
				for (uint32_t y = 0; y < source->Height; y++)
					for (uint32_t x = 0; x < source->Width; x++) {
						const float u = float((x + .5) / source->Width), v = float((y + .5) / source->Height);
						const float tx = 1.f / source->Width, ty = 1.f / source->Height;
						float height = 0;
						const auto add = [&](float sx, float sy, size_t channel, float sign) {
							const float neighbor = NormalFloatPixel(scratch[0], sx, sy)[0];
							const float normal = NormalFloatPixel(*source, sx, sy)[channel] - .5f;
							height += neighbor + sign * normal / intensity;
						};
						if (sweep & 1) add(u, v + ty, 1, -1);
						if (sweep & 2) add(u - tx, v, 0, -1);
						if (sweep & 4) add(u, v - ty, 1, 1);
						if (sweep & 8) add(u + tx, v, 0, 1);
						if (active) height /= float(active);
						if (!NormalStore(context, scratch[1], x, y, {height, height, height, 1}))
							return false;
					}
				std::swap(scratch[0], scratch[1]);
			}
			for (uint32_t y = 0; y < source->Height; y++)
				for (uint32_t x = 0; x < source->Width; x++) {
					const float height = float(ReadPixel(scratch[0], x, y)[0]);
					if (!NormalStore(context, *output, x, y, {height, height, height, 1})) return false;
				}
			return context.FailureCode == Status::Ok;
		}
		struct NormalLightControl {
			int64_t Type = 0, Attenuation = 0;
			NormalVector Position{}, EndPosition{};
			std::array<float, 4> Colour{1, 1, 1, 1}, EndColour{1, 1, 1, 1};
			float Range = 0, Intensity = 0, RadialBands = 0, RadialStart = 0, RadialRatio = .5f,
				  RadialShadow = 0, Bands = 0;
			const Curve *AttenCurve = nullptr;
		};
		std::array<float, 4> NormalColour(
			const NodeContext &context, std::string_view id, Colour fallback = {255, 255, 255, 255}
		) {
			const auto colour = context.Get<Colour>(id, fallback);
			return {colour.Red / 255.f, colour.Green / 255.f, colour.Blue / 255.f, colour.Alpha / 255.f};
		}
		bool NormalLightChoice(NodeContext &context, std::string_view id, int64_t &choice) {
			const auto value = context.Find(id);
			const auto number = value ? SourceChoiceNumber(*value) : std::optional<double>{0};
			if (!number || !std::isfinite(*number) || std::trunc(*number) != *number || *number < 0 ||
				*number > 3)
				return context.Fail(
					Status::UnsupportedExecution, "normal light requires a defined source choice", "type"
				);
			choice = int64_t(*number);
			return true;
		}
		bool
		ReadNormalLight(NodeContext &context, size_t group, const Image &source, NormalLightControl &light) {
			const std::string suffix = "_" + std::to_string(group);
			if (!NormalLightChoice(context, "type" + suffix, light.Type) ||
				!NormalLightChoice(context, "attenuation" + suffix, light.Attenuation))
				return false;
			const Vector2 position = UnitVector(context, "position" + suffix, source.Width, source.Height),
						  end = UnitVector(context, "end_position" + suffix, source.Width, source.Height);
			light.Position = {
				float(position.X / source.Width),
				float(position.Y / source.Height),
				-float(context.Scalar("distance" + suffix)) / 100
			};
			light.EndPosition = {
				float(end.X / source.Width),
				float(end.Y / source.Height),
				-float(context.Scalar("end_distance" + suffix)) / 100
			};
			light.Range = float(context.Scalar("range" + suffix, 16)) / std::max(source.Width, source.Height);
			light.Intensity = float(context.Scalar("intensity" + suffix, 4));
			light.Colour = NormalColour(context, "color" + suffix);
			light.EndColour = NormalColour(context, "end_color" + suffix);
			light.RadialBands = float(context.Integer("radial_banding" + suffix));
			light.RadialStart = float(context.Scalar("radial_start" + suffix));
			light.RadialRatio = float(context.Scalar("radial_band_ratio" + suffix, .5));
			light.RadialShadow = float(context.Scalar("radial_shadow" + suffix));
			light.Bands = float(context.Integer("banding" + suffix));
			const Value *curve = context.Find("atten_curve" + suffix);
			light.AttenCurve = curve ? std::get_if<Curve>(curve) : nullptr;
			if (light.Type != 1 && light.Range == 0)
				return context.Fail(
					Status::UnsupportedExecution,
					"source normal light divides by zero range",
					"range" + suffix
				);
			for (const float value :
				 {light.Position[0],
				  light.Position[1],
				  light.Position[2],
				  light.EndPosition[0],
				  light.EndPosition[1],
				  light.EndPosition[2],
				  light.Range,
				  light.Intensity,
				  light.RadialBands,
				  light.RadialStart,
				  light.RadialRatio,
				  light.RadialShadow,
				  light.Bands})
				if (!std::isfinite(value))
					return context.Fail(
						Status::UnsupportedExecution,
						"normal light controls exceed finite shader uniforms",
						"position" + suffix
					);
			if (light.Type != 1 && light.Attenuation == 3) {
				if (!light.AttenCurve || light.AttenCurve->Anchors.empty() ||
					light.AttenCurve->Anchors.size() > 9)
					return context.Fail(
						Status::UnsupportedExecution,
						"normal attenuation curve exceeds source GLSL uniform storage",
						"atten_curve" + suffix
					);
			}
			return context.FailureCode == Status::Ok;
		}
		bool NormalLightPixel(
			NodeContext &context,
			const NormalLightControl &light,
			const Image *normalMap,
			const Image *heightMap,
			float heightScale,
			float u,
			float v,
			std::array<float, 4> &pixel
		) {
			float height = 0;
			if (heightMap) {
				const auto heightPixel = NormalFloatPixel(*heightMap, u, v);
				height = (heightPixel[0] + heightPixel[1] + heightPixel[2]) / 3 * heightScale;
			}
			const NormalVector current{u, v, height};
			NormalVector direction{};
			float brightness = 0;
			auto colour = light.Colour;
			if (light.Type == 1) {
				direction = NormalSubtract(light.Position, {.5f, .5f, 0});
				if (!NormalUnit(direction) && normalMap)
					return context.Fail(
						Status::UnsupportedExecution,
						"source sun light normalizes a zero direction",
						"position"
					);
				direction[0] *= -1;
				brightness = 1;
			} else if (light.Type == 2) {
				const auto segment = NormalSubtract(light.EndPosition, light.Position),
						   offset = NormalSubtract(current, light.Position);
				const float denominator = NormalDot(segment, segment);
				if (denominator == 0)
					return context.Fail(
						Status::UnsupportedExecution,
						"source line light divides by zero segment length",
						"end_position"
					);
				const float ratio = std::clamp(NormalDot(offset, segment) / denominator, 0.f, 1.f);
				NormalVector closest{};
				for (size_t component = 0; component < 3; component++)
					closest[component] = light.Position[component] + ratio * segment[component];
				direction = NormalSubtract(closest, current);
				brightness = 1 - std::sqrt(NormalDot(direction, direction)) / light.Range;
				for (size_t channel = 0; channel < 4; channel++)
					colour[channel] = light.Colour[channel] * (1 - ratio) + light.EndColour[channel] * ratio;
				if (!NormalUnit(direction) && normalMap)
					return context.Fail(
						Status::UnsupportedExecution,
						"source line light normalizes a zero direction",
						"end_position"
					);
			} else {
				direction = NormalSubtract(light.Position, current);
				brightness = 1 - std::sqrt(NormalDot(direction, direction)) / light.Range;
				if (!NormalUnit(direction) && (normalMap || light.Type == 3))
					return context.Fail(
						Status::UnsupportedExecution,
						"source normal light normalizes a zero direction",
						"position"
					);
				if (light.Type == 3) {
					auto spot = NormalSubtract(light.Position, light.EndPosition);
					if (!NormalUnit(spot))
						return context.Fail(
							Status::UnsupportedExecution,
							"source spot light normalizes a zero axis",
							"end_position"
						);
					const float cosine = NormalDot(spot, direction);
					if (cosine < -1 || cosine > 1)
						return context.Fail(
							Status::UnsupportedExecution,
							"source spot light acos argument is undefined",
							"end_position"
						);
					brightness = 1 - std::acos(cosine) / light.Range;
				} else if (light.RadialBands > 1) {
					constexpr float tau = 6.28318530718f;
					const float angle = std::atan2(v - light.Position[1], u - light.Position[0]) + tau / 2 +
										light.RadialStart * std::numbers::pi_v<float> / 180;
					const float bands = angle / tau * light.RadialBands,
								remainder = bands - std::floor(bands);
					if (remainder < light.RadialRatio) brightness *= light.RadialShadow;
				}
			}
			brightness = std::max(0.f, brightness);
			if (light.Type != 1) {
				if (light.Attenuation == 0)
					brightness = std::pow(brightness, 2.f);
				else if (light.Attenuation == 1)
					brightness = 1 - std::pow(1 - brightness, 2.f);
				else if (light.Attenuation == 3)
					brightness = NormalEvalShaderCurve(*light.AttenCurve, brightness);
			}
			brightness *= light.Intensity;
			if (light.Bands > 0) brightness = std::ceil(brightness * light.Bands) / light.Bands;
			float diffuse = 1;
			if (normalMap) {
				const auto packed = NormalFloatPixel(*normalMap, u, v);
				NormalVector normal{packed[0] * -2 + 1, packed[1] * -2 + 1, packed[2] * -2 + 1};
				if (!NormalUnit(normal))
					return context.Fail(
						Status::UnsupportedExecution,
						"source normal light normalizes a zero normal",
						"normal_map"
					);
				diffuse = std::max(NormalDot(normal, direction), 0.f);
			}
			pixel = {
				diffuse * colour[0] * colour[3] * brightness,
				diffuse * colour[1] * colour[3] * brightness,
				diffuse * colour[2] * colour[3] * brightness,
				1
			};
			return true;
		}
		bool NormalLight(NodeContext &context) {
			const Image *source = context.Input("surface_in"), *normal = context.Input("normal_map"),
						*height = context.Input("height_map");
			if (!source)
				return context.Fail(
					Status::TypeMismatch, "normal light requires its source surface", "surface_in"
				);
			auto groupCharge = context.ReserveWorkspace(
				context.Authored.DynamicInputs.size() * (sizeof(size_t) + sizeof(NormalLightControl)) + 4096,
				"light_only"
			);
			if (!groupCharge) return false;
			std::vector<size_t> groups;
			groups.reserve(context.Authored.DynamicInputs.size());
			for (const auto &input : context.Authored.DynamicInputs) {
				size_t group = 0;
				const auto *slot = FindDynamicTemplate(context.Entry, input.Id, group);
				if (slot && slot->SourceIndex == 0) groups.push_back(group);
			}
			std::sort(groups.begin(), groups.end());
			groups.erase(std::unique(groups.begin(), groups.end()), groups.end());
			const uint64_t workPerPixel = 1 + groups.size() * 12;
			if (!NormalWork(context, uint64_t(source->Width) * source->Height, workPerPixel)) return false;
			std::vector<NormalLightControl> lights;
			lights.reserve(groups.size());
			for (const size_t group : groups) {
				NormalLightControl control;
				if (!ReadNormalLight(context, group, *source, control)) return false;
				lights.push_back(control);
			}
			const float heightScale = float(context.Scalar("height", 1));
			if (height && !std::isfinite(heightScale))
				return context.Fail(
					Status::UnsupportedExecution, "normal height exceeds finite shader uniforms", "height"
				);
			// processData re-verifies both targets with surface_verify's default RGBA8 format.
			Image *lightOnly =
				context.NewImage("light_only", source->Width, source->Height, SurfaceFormat::RGBA8Unorm);
			Image *output =
				context.NewImage("surface_out", source->Width, source->Height, SurfaceFormat::RGBA8Unorm);
			if (!lightOnly || !output) return false;
			const auto ambient = NormalColour(context, "ambient", {0, 0, 0, 255});
			for (uint32_t y = 0; y < source->Height; y++)
				for (uint32_t x = 0; x < source->Width; x++) {
					if (!NormalStore(context, *lightOnly, x, y, {0, 0, 0, 1})) return false;
					const float u = float((x + .5) / source->Width), v = float((y + .5) / source->Height);
					for (const auto &light : lights) {
						std::array<float, 4> diffuse{};
						if (!NormalLightPixel(context, light, normal, height, heightScale, u, v, diffuse))
							return false;
						const auto accumulated = NormalFloatPixel(*lightOnly, u, v);
						for (size_t channel = 0; channel < 3; channel++)
							diffuse[channel] += accumulated[channel];
						if (!NormalStore(context, *lightOnly, x, y, diffuse)) return false;
					}
					const auto base = NormalFloatPixel(*source, u, v),
							   lighting = NormalFloatPixel(*lightOnly, u, v);
					std::array<float, 4> result{};
					for (size_t channel = 0; channel < 4; channel++)
						result[channel] = base[channel] * ambient[channel] + lighting[channel];
					if (context.Boolean("keep_alpha", true)) result[3] = base[3];
					if (!NormalStore(context, *output, x, y, result)) return false;
				}
			return context.FailureCode == Status::Ok;
		}

	}
	std::span<const ExecutorEntry> SourceNormalMapExecutors() {
		static constexpr ExecutorEntry executors[]{
			{"pc.normal_blend", NormalBlend, true},
			{"pc.normal_to_height", NormalHeight, true},
			{"pc.normal_light", NormalLight, true}
		};
		return executors;
	}
}
