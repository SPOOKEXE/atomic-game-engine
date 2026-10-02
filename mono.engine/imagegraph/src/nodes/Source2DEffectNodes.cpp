#include "Curve.hpp"
#include "Families.hpp"
#include "Sampler.hpp"
#include "Source2DGenerator.hpp"

#include <array>
#include <cmath>

namespace engine::imagegraph::detail {
	namespace {
		double MeanAlpha(const Rgba &colour) {
			return (colour[0] + colour[1] + colour[2]) / 3.0 * colour[3];
		}
	}

	bool Curvature(NodeContext &context) {
		const auto sampler = ReadSampler(context);
		const bool filtered = Filtered(sampler);
		return RunPixelProcessor(context, [&](const Image &image, uint32_t, uint32_t, double u, double v) {
			const double radius = MappedScalar(context, "radius", u, v, filtered);
			const double intensity = MappedScalar(context, "intensity", u, v, filtered);
			const double tx = radius / image.Width, ty = radius / image.Height;
			const auto sample = [&](double x, double y) {
				return MeanAlpha(
					SampleTextureSimple(image, u + x * tx, v + y * ty, sampler.Oversample, filtered)
				);
			};
			const double centre = sample(0, 0), denominator = radius * radius;
			std::array<double, 4> derivatives{
				(sample(1, 0) - 2 * centre + sample(-1, 0)) / denominator,
				(sample(0, 1) - 2 * centre + sample(0, -1)) / denominator,
				(sample(1, 1) - 2 * centre + sample(-1, -1)) / (2 * denominator),
				(sample(-1, 1) - 2 * centre + sample(1, -1)) / (2 * denominator)
			};
			if (context.Boolean("absolute"))
				for (double &derivative : derivatives)
					derivative = std::abs(derivative);
			double curvature = 0;
			for (const double derivative : derivatives)
				curvature += derivative;
			const double grey = 0.5 + 0.25 * curvature * intensity;
			return Rgba{grey, grey, grey, 1};
		});
	}

	bool Emboss(NodeContext &context) {
		bool failed = false;
		if (CopyWhenInactive(context, failed)) return !failed;
		const Image *source = context.Input("surface_in"), *heightMap = context.Input("heightmap");
		if (!source) return context.Fail(Status::InvalidValue, "Surface In is required", "surface_in");
		const int64_t height = context.Integer("height", 1),
					  directions = context.Boolean("high_res") ? 128 : 2;
		if (height < 1) return context.Fail(Status::InvalidValue, "Emboss height must be positive", "height");
		constexpr uint64_t MAXIMUM_SAMPLES = 64'000'000;
		if (uint64_t(height) > MAXIMUM_SAMPLES / directions / source->Width / source->Height)
			return context.Fail(Status::LimitExceeded, "Emboss exceeds sample work budget", "height");
		const auto sampler = ReadSampler(context);
		const bool filtered = Filtered(sampler);
		const double direction = context.Scalar("direction", 135) * std::acos(-1.0) / 180.0;
		const Colour tint = context.Get<Colour>("color", {255, 255, 255, 255});
		const Rgba colour{tint.Red / 255.0, tint.Green / 255.0, tint.Blue / 255.0, tint.Alpha / 255.0};
		return RunPixelProcessor(
			context, [&](const Image &image, uint32_t x, uint32_t y, double u, double v) {
				const Rgba original = ReadPixel(image, x, y);
				Rgba result = original;
				const Image &heightSource = heightMap ? *heightMap : image;
				const double brightness = MeanAlpha(Texture(heightSource, u, v, filtered));
				double light = 0, totalWeight = 0;
				for (int64_t angleIndex = 0; angleIndex < directions; ++angleIndex) {
					const double angle = double(angleIndex) / directions * 2 * std::acos(-1.0);
					const double lightDirection = std::cos(angle);
					const double offsetX = std::cos(angle + direction) / image.Width;
					const double offsetY = -std::sin(angle + direction) / image.Height;
					for (int64_t index = 1; index <= height; ++index) {
						const double weight = 1.0 - double(index - 1) / height;
						const double sampled = MeanAlpha(SampleTextureSimple(
							heightSource,
							u + offsetX * index,
							v + offsetY * index,
							sampler.Oversample,
							filtered
						));
						totalWeight += weight;
						if (sampled < brightness) {
							const double strength = weight * std::abs(sampled - brightness);
							light += context.Boolean("normalize", true) ? lightDirection * strength
																		: (lightDirection > 0	? strength
																		   : lightDirection < 0 ? -strength
																								: 0.0);
							break;
						}
					}
				}
				const double effect =
					1.0 + light / totalWeight * (MappedScalar(context, "intensity", u, v, filtered) + 1.0);
				if (effect > 1) {
					const double brighten = (effect - 1) * context.Scalar("brighten", 1);
					for (double &channel : result)
						channel *= 1 + brighten;
					for (size_t channel = 0; channel < 3; ++channel)
						result[channel] +=
							(1 - (1 - result[channel]) * colour[channel] - result[channel]) * brighten;
				} else if (effect < 1) {
					const double darken = (1 - effect) * context.Scalar("darken", 1);
					for (double &channel : result)
						channel *= 1 - darken;
					for (size_t channel = 0; channel < 3; ++channel)
						result[channel] += (result[channel] * colour[channel] - result[channel]) * darken;
				}
				result[3] = original[3];
				return result;
			}
		);
	}
	bool Vignette(NodeContext &context) {
		return RunPixelProcessor(
			context, [&](const Image &image, uint32_t x, uint32_t y, double u, double v) {
				const Vector2 center = source2d::PixelPosition(context, "center", image.Width, image.Height);
				const double roundness = MappedScalar(context, "roundness", u, v) / 2;
				const double distance = (u - 0.5) * (u - 0.5) + (v - 0.5) * (v - 0.5);
				const double angle = std::atan2(v - 0.5, u - 0.5);
				const double shapedU = u + (center.X + std::cos(angle) * distance - u) * roundness;
				const double shapedV = v + (center.Y + std::sin(angle) * distance - v) * roundness;
				const double exposure = MappedScalar(context, "exposure", u, v);
				const double vignette = std::clamp(
					std::pow(shapedU * (1 - shapedV) * shapedV * (1 - shapedU) * exposure, 0.25 + roundness),
					0.0,
					1.0
				);
				double strength = 1 - (1 - vignette) * MappedScalar(context, "strength", u, v);
				if (context.Boolean("strength_curved"))
					if (const auto *curve = std::get_if<Curve>(context.Find("strength_curve")))
						strength = EvalShaderCurve(*curve, strength);
				const Rgba source = ReadPixel(image, x, y),
						   tint = source2d::InputColour(context, "color", {255, 255, 255, 255});
				const double inverse = strength < 0.001 ? 10000 : 1 / strength,
							 light = context.Scalar("lighten");
				Rgba result = source;
				for (size_t channel = 0; channel < 3; ++channel) {
					const double darkened =
						source[channel] * strength * (tint[channel] + (1 - tint[channel]) * strength);
					const double lightened =
						source[channel] * inverse * (1 - tint[channel] + tint[channel] * inverse);
					result[channel] = darkened + (lightened - darkened) * light;
				}
				return result;
			}
		);
	}

	bool SymmetricNearest(NodeContext &context) {
		bool failed = false;
		if (CopyWhenInactive(context, failed)) return !failed;
		const Image *source = context.Input("surface_in");
		if (!source) return context.Fail(Status::InvalidValue, "Surface In is required", "surface_in");
		const SamplerSettings sampler = ReadSampler(context);
		const bool filtered = Filtered(sampler);
		const Vector2 radiusRange = context.Boolean("radius_mapped") && context.Input("radius_map")
										? context.Vec2("radius_map_range")
										: Vector2{context.Scalar("radius"), context.Scalar("radius")};
		const double radiusBound = std::floor(std::max(radiusRange.X, radiusRange.Y));
		if (!std::isfinite(radiusBound) || radiusBound < 0)
			return context.Fail(Status::InvalidValue, "Symmetric radius must be nonnegative", "radius");
		const double sampleCount =
			(radiusBound + 1) * (radiusBound * 2 + 1) * 2 * source->Width * source->Height;
		if (sampleCount > 64'000'000)
			return context.Fail(
				Status::LimitExceeded, "Symmetric nearest exceeds sample work budget", "radius"
			);
		const int64_t bound = static_cast<int64_t>(radiusBound);
		return RunPixelProcessor(context, [&](const Image &image, uint32_t, uint32_t, double u, double v) {
			const int64_t radius = static_cast<int64_t>(MappedScalar(context, "radius", u, v, filtered));
			double alpha = 1;
			const Vector2 uv = source2d::GeneratorUv(context, u, v, alpha, filtered);
			const auto sample = [&](int64_t x, int64_t y) {
				return SampleTextureSimple(
					image,
					uv.X + double(x) / image.Width,
					uv.Y + double(y) / image.Height,
					sampler.Oversample,
					filtered
				);
			};
			const Rgba center = sample(0, 0);
			Rgba mean{};
			uint64_t count = 0;
			for (int64_t y = 0; y <= bound && y <= radius; ++y)
				for (int64_t x = -bound; x <= bound && x <= radius; ++x) {
					if (x < -radius) continue;
					const Rgba first = sample(x, y), second = sample(-x, -y);
					for (size_t channel = 0; channel < 3; ++channel)
						mean[channel] += std::abs(center[channel] - first[channel]) <
												 std::abs(center[channel] - second[channel])
											 ? first[channel]
											 : second[channel];
					mean[3] += 1;
					++count;
				}
			const double intensity = MappedScalar(context, "intensity", u, v, filtered);
			Rgba result{};
			for (size_t channel = 0; channel < 4; ++channel)
				result[channel] = center[channel] + (mean[channel] / count - center[channel]) * intensity;
			result[3] *= alpha;
			return result;
		});
	}

	bool LinearBrush(NodeContext &context) {
		bool failed = false;
		if (CopyWhenInactive(context, failed)) return !failed;
		const Image *source = context.Input("surface_in");
		if (!source) return context.Fail(Status::InvalidValue, "Surface In is required", "surface_in");
		const int64_t iterations = context.Integer("iteration", 10);
		if (iterations < 1)
			return context.Fail(Status::InvalidValue, "Brush iteration must be positive", "iteration");
		if (uint64_t(iterations) > 64'000'000 / 4 / source->Width / source->Height)
			return context.Fail(Status::LimitExceeded, "Brush exceeds sample work budget", "iteration");
		const auto *curve = std::get_if<Curve>(context.Find("attenuation_curve"));
		const bool curved = context.Boolean("attenuation_curved") && curve;
		return RunPixelProcessor(context, [&](const Image &image, uint32_t, uint32_t, double u, double v) {
			const double length = MappedScalar(context, "length", u, v),
						 attenuation = MappedScalar(context, "attenuation", u, v),
						 circulation = MappedScalar(context, "circulation", u, v);
			Vector2 position{u * image.Width, v * image.Height};
			const auto sample = [&](Vector2 point) {
				return SampleNearest(image, point.X / image.Width, point.Y / image.Height);
			};
			const auto magnitude = [&](Vector2 point) {
				const Rgba colour = sample(point);
				double squared = 0;
				for (double channel : colour)
					squared += channel * channel;
				return std::sqrt(squared);
			};
			double weight = 1, accumulated = 0;
			Rgba result{};
			for (int64_t i = 0; i < iterations; ++i) {
				const double origin = magnitude(position);
				const Vector2 gradient{
					(magnitude({position.X + length, position.Y}) - origin) / length + 0.001,
					(magnitude({position.X, position.Y + length}) - origin) / length + 0.001
				};
				const Vector2 direction{
					gradient.X + (gradient.Y - gradient.X) * circulation,
					gradient.Y + (-gradient.X - gradient.Y) * circulation
				};
				const double size = std::hypot(direction.X, direction.Y);
				position.X += 2 * direction.X / size;
				position.Y += 2 * direction.Y / size;
				if (!std::isfinite(position.X) || !std::isfinite(position.Y)) {
					context.Fail(Status::InvalidValue, "Brush gradient is not finite", "length");
					return Rgba{};
				}
				const double contribution =
					weight * (curved ? EvalShaderCurve(*curve, double(i) / iterations) : 1);
				const Rgba colour = sample(position);
				for (size_t channel = 0; channel < 4; ++channel)
					result[channel] += colour[channel] * contribution;
				accumulated += contribution;
				weight *= attenuation;
			}
			for (size_t channel = 0; channel < 3; ++channel)
				result[channel] /= accumulated;
			return result;
		});
	}

}
