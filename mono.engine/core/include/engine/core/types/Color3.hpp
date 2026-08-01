#pragma once

// Linear floating-point colour, normally 0..1 per channel.
//
// FromRGB takes the 0..255 sRGB bytes a person types and converts. Doing the
// conversion here rather than at the shader means one place knows the transfer
// function, and lighting maths never runs on sRGB values by accident.
//
// @tier L1 · shared

#include <cmath>
#include <cstdint>

namespace engine::core {

	// An RGB colour stored in linear light for lighting and blending.
	//
	// Channels are normally in `[0, 1]`, but construction and arithmetic do not
	// clamp them.
	struct Color3 {
		// Linear red channel.
		float R = 0.0f;
		// Linear green channel.
		float G = 0.0f;
		// Linear blue channel.
		float B = 0.0f;

		// Constructs linear black.
		constexpr Color3() = default;
		// Constructs a colour from linear channels without conversion or clamping.
		//
		// @param r The linear red channel.
		// @param g The linear green channel.
		// @param b The linear blue channel.
		constexpr Color3(float r, float g, float b) : R(r), G(g), B(b) {}

		// Converts 8-bit sRGB channels to linear light with the standard piecewise curve.
		//
		// @param r The red sRGB channel in `[0, 255]`.
		// @param g The green sRGB channel in `[0, 255]`.
		// @param b The blue sRGB channel in `[0, 255]`.
		static Color3 FromRGB(uint8_t r, uint8_t g, uint8_t b) {
			return {ToLinear(r / 255.0f), ToLinear(g / 255.0f), ToLinear(b / 255.0f)};
		}

		// Constructs a colour from linear channels without conversion or clamping.
		//
		// @param r The linear red channel.
		// @param g The linear green channel.
		// @param b The linear blue channel.
		static constexpr Color3 FromLinear(float r, float g, float b) {
			return {r, g, b};
		}

		// Scales every linear channel without clamping.
		constexpr Color3 operator*(float scalar) const {
			return {R * scalar, G * scalar, B * scalar};
		}
		// Multiplies corresponding linear channels, as used for tinting.
		constexpr Color3 operator*(const Color3 &other) const {
			return {R * other.R, G * other.G, B * other.B};
		}
		// Adds corresponding linear channels without clamping.
		constexpr Color3 operator+(const Color3 &other) const {
			return {R + other.R, G + other.G, B + other.B};
		}

		// Reports whether all linear channels are exactly equal.
		constexpr bool operator==(const Color3 &other) const {
			return R == other.R && G == other.G && B == other.B;
		}

		// Interpolates linearly in linear-light RGB without clamping `alpha` or the result.
		//
		// `alpha` 0 returns this colour, 1 returns `target`, and values outside that
		// range extrapolate.
		constexpr Color3 Lerp(const Color3 &target, float alpha) const {
			return {
				R + (target.R - R) * alpha,
				G + (target.G - G) * alpha,
				B + (target.B - B) * alpha,
			};
		}

	  private:
		static float ToLinear(float channel) {
			if (channel <= 0.04045f) {
				return channel / 12.92f;
			}
			return std::pow((channel + 0.055f) / 1.055f, 2.4f);
		}
	};
}
