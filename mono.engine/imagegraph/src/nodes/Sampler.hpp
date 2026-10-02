#pragma once

// CPU equivalent of the source shader sampler region (texture2Dintp and sampleTexture) and of plain
// texture2D reads with GameMaker's clamp-to-edge addressing.
//
// A node's Interpolate and Oversample attributes of 0 inherit the centrally resolved group/project
// attributes, which are Pixel interpolation and Repeat XY oversampling for a fresh project.

#include "Processor.hpp"

#include <cmath>
#include <numbers>

namespace engine::imagegraph::detail {
	struct SamplerSettings {
		// 1 Pixel, 2 Bilinear, 3 Bicubic, 4 Lanczos3, 6 CleanEdge.
		int64_t Interpolation = 1;
		// 1 Empty, 2 Black, 3 Clamp, 4 Repeat XY, 6..8 repeat X, 10..12 repeat Y.
		int64_t Oversample = 4;
	};

	inline SamplerSettings ReadSampler(const NodeContext &context) {
		SamplerSettings settings;
		const int64_t interpolation = context.Integer("interpolate", 0);
		const int64_t oversample = context.Integer("oversample", 0);
		settings.Interpolation = interpolation == 0 ? context.InheritedInterpolation : interpolation;
		settings.Oversample = oversample == 0 ? context.InheritedOversample : oversample;
		return settings;
	}

	// sampler_ext adds CleanEdge (6); its edge-slicing region is not ported yet, so it is refused by name.
	inline bool SupportedSampler(NodeContext &context, const SamplerSettings &settings) {
		if (settings.Interpolation != 6) return true;
		return context.Fail(
			Status::UnsupportedExecution, "CleanEdge interpolation has no native sampler yet", "interpolate"
		);
	}

	// gpu_set_tex_filter follows shader_set_interpolation: filtering is off only for Pixel and CleanEdge.
	inline bool Filtered(const SamplerSettings &settings) {
		return settings.Interpolation != 1 && settings.Interpolation != 6;
	}

	inline Rgba Texel(const Image &image, int64_t x, int64_t y) {
		x = std::clamp<int64_t>(x, 0, static_cast<int64_t>(image.Width) - 1);
		y = std::clamp<int64_t>(y, 0, static_cast<int64_t>(image.Height) - 1);
		return ReadPixel(image, static_cast<uint32_t>(x), static_cast<uint32_t>(y));
	}

	// texture2D with clamp-to-edge addressing, nearest or bilinear.
	inline Rgba Texture(const Image &image, double u, double v, bool filtered) {
		if (!filtered) return SampleNearest(image, u, v);
		return BilinearClamp(image, u, v);
	}

	// GLSL fract and mod(x, 1.0), floor based for negative values.
	inline double Fract(double value) {
		return value - std::floor(value);
	}

	inline double Sinc(double x) {
		return x == 0.0 ? 1.0 : std::sin(x * std::numbers::pi) / (x * std::numbers::pi);
	}

	inline double LanczosWeight(double distance, double radius) {
		if (distance == 0.0) return 1.0;
		return distance * distance < radius * radius ? Sinc(distance) * Sinc(distance / radius) : 0.0;
	}

	// texture2Dintp. The bicubic and Lanczos3 branches reuse filtered reads exactly as the shader does.
	inline Rgba TextureInterpolated(const Image &image, double u, double v, const SamplerSettings &settings) {
		const bool filtered = Filtered(settings);
		if (settings.Interpolation == 3) {
			const double width = image.Width, height = image.Height;
			double x = u * width + 0.5, y = v * height + 0.5;
			const double ix = std::floor(x), iy = std::floor(y);
			const double fx = x - ix, fy = y - iy;
			x = ix + fx * fx * (3.0 - 2.0 * fx);
			y = iy + fy * fy * (3.0 - 2.0 * fy);
			return Texture(image, (x - 0.5) / width, (y - 0.5) / height, filtered);
		}
		if (settings.Interpolation == 4) {
			const double width = image.Width, height = image.Height;
			const double centerU = u - (Fract(u * width) - 0.5) / width;
			const double centerV = v - (Fract(v * height) - 0.5) / height;
			const double offsetX = (u - centerU) * width, offsetY = (v - centerV) * height;
			Rgba colour{};
			double weight = 0.0;
			for (int x = -1; x <= 1; x++) {
				for (int y = -1; y <= 1; y++) {
					const double wxa = LanczosWeight(x * 2 - 1 - offsetX, 3.0);
					const double wxb = LanczosWeight(x * 2 - offsetX, 3.0);
					const double wya = LanczosWeight(y * 2 - 1 - offsetY, 3.0);
					const double wyb = LanczosWeight(y * 2 - offsetY, 3.0);
					const double wx = wxa + wxb, wy = wya + wyb;
					const double w = wx * wy;
					const double sx = x * 2 - 0.5 + wxb / wx;
					const double sy = y * 2 - 0.5 + wyb / wy;
					const Rgba sample = Texture(image, centerU + sx / width, centerV + sy / height, filtered);
					for (size_t channel = 0; channel < 4; channel++)
						colour[channel] += w * sample[channel];
					weight += w;
				}
			}
			for (double &channel : colour)
				channel /= weight;
			return colour;
		}
		return Texture(image, u, v, filtered);
	}

	// sampleTexture without a UV map: inside [0, 1] reads normally, outside follows the Oversample mode.
	inline Rgba SampleTexture(const Image &image, double u, double v, const SamplerSettings &settings) {
		if (u >= 0.0 && v >= 0.0 && u <= 1.0 && v <= 1.0) return TextureInterpolated(image, u, v, settings);
		constexpr Rgba EMPTY{0, 0, 0, 0};
		constexpr Rgba BLACK{0, 0, 0, 1};
		switch (settings.Oversample) {
		case 2:
			return BLACK;
		case 3:
			return TextureInterpolated(image, std::clamp(u, 0.0, 1.0), std::clamp(v, 0.0, 1.0), settings);
		case 4:
			return TextureInterpolated(image, Fract(u), Fract(v), settings);
		case 6:
			return v < 0.0 || v > 1.0 ? EMPTY : TextureInterpolated(image, Fract(u), v, settings);
		case 7:
			return v < 0.0 || v > 1.0 ? BLACK : TextureInterpolated(image, Fract(u), v, settings);
		case 8:
			return TextureInterpolated(image, Fract(u), std::clamp(v, 0.0, 1.0), settings);
		case 10:
			return u < 0.0 || u > 1.0 ? EMPTY : TextureInterpolated(image, u, Fract(v), settings);
		case 11:
			return u < 0.0 || u > 1.0 ? BLACK : TextureInterpolated(image, u, Fract(v), settings);
		case 12:
			return TextureInterpolated(image, std::clamp(u, 0.0, 1.0), Fract(v), settings);
		default:
			return EMPTY;
		}
	}

	// sampleTexture from the sampler_simple region: plain texture2D inside [0, 1], with the node's current
	// texture filter, and the Oversample rule outside.
	inline Rgba
	SampleTextureSimple(const Image &image, double u, double v, int64_t oversample, bool filtered) {
		SamplerSettings settings;
		settings.Interpolation = filtered ? 2 : 1;
		settings.Oversample = oversample;
		return SampleTexture(image, u, v, settings);
	}

	// The sampler region's UV map: getUVA remaps by the map's flipped RG and returns its alpha, and
	// sampleTexture(texture, pos, mapBlend) scales the remap by mapBlend.
	struct UvMap {
		const Image *Map = nullptr;
		double Mix = 0.0;
	};

	inline UvMap ReadUvMap(const NodeContext &context) {
		return {context.Input("uv_map"), context.Scalar("uv_mix", 1.0)};
	}

	inline void
	UvRemap(const UvMap &map, double &u, double &v, double amount, bool filtered, double *alpha = nullptr) {
		if (alpha) *alpha = 1.0;
		if (!map.Map) return;
		const Rgba texel = Texture(*map.Map, u, v, filtered);
		if (alpha) *alpha = texel[3];
		const double weight = amount * map.Mix;
		u += (texel[0] - u) * weight;
		v += ((1.0 - texel[1]) - v) * weight;
	}

	inline Rgba SampleTextureUv(
		const Image &image,
		double u,
		double v,
		double mapBlend,
		const UvMap &map,
		const SamplerSettings &settings
	) {
		UvRemap(map, u, v, mapBlend, Filtered(settings));
		return SampleTexture(image, u, v, settings);
	}
}
