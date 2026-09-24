#pragma once

// Pure value operations from the pinned Pixel Composer node source.
// Graph validation and socket resolution belong to the document evaluator.

#include <engine/imagegraph/Document.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace engine::imagegraph::detail {
	// Mode order follows node_math.gml at b69eca232217360cf1502ef0223523d818606652.
	inline std::optional<double>
	Math(double a, double b, double amount, Vector2 from, Vector2 to, int64_t mode, bool degrees) {
		constexpr double PI = 3.14159265358979323846;
		const double angle = degrees ? a * PI / 180.0 : a;
		double result = 0.0;
		switch (mode) {
		case 0:
			result = a + b;
			break;
		case 1:
			result = a - b;
			break;
		case 2:
			result = a * b;
			break;
		case 3:
			result = b == 0.0 ? 0.0 : a / b;
			break;
		case 4:
			result = std::pow(a, b);
			break;
		case 5:
			result = b <= 0.0 ? 0.0 : std::pow(a, 1.0 / b);
			break;
		case 6:
			result = std::sin(angle) * b;
			break;
		case 7:
			result = std::cos(angle) * b;
			break;
		case 8:
			result = std::tan(angle) * b;
			break;
		case 9:
			result = b == 0.0 ? 0.0 : std::fmod(a, b);
			break;
		case 10:
			result = std::floor(a);
			break;
		case 11:
			result = std::ceil(a);
			break;
		case 12:
			result = std::floor(a + 0.5);
			break;
		case 13:
			result = a + (b - a) * amount;
			break;
		case 14:
			result = std::abs(a);
			break;
		case 15:
			result = std::clamp(a, std::min(b, amount), std::max(b, amount));
			break;
		case 16:
			result = b == 0.0 ? a : std::floor(a / b + 0.5) * b;
			break;
		case 17:
			result = a - std::floor(a);
			break;
		case 18:
			if (from.X == from.Y) return std::nullopt;
			result = to.X + (to.Y - to.X) * (a - from.X) / (from.Y - from.X);
			break;
		case 19:
			if (b <= 0.0 || b == 1.0 || a <= 0.0) return std::nullopt;
			result = std::log(a) / std::log(b);
			break;
		case 20:
			result = std::max(a, b);
			break;
		case 21:
			result = std::min(a, b);
			break;
		default:
			return std::nullopt;
		}
		return std::isfinite(result) ? std::optional<double>{result} : std::nullopt;
	}

	// Mode order follows node_compare.gml.
	inline std::optional<bool> Compare(double a, double b, int64_t mode) {
		switch (mode) {
		case 0:
			return a == b;
		case 1:
			return a != b;
		case 2:
			return a > b;
		case 3:
			return a >= b;
		case 4:
			return a < b;
		case 5:
			return a <= b;
		default:
			return std::nullopt;
		}
	}

	// Mode order follows node_logic.gml. NOT ignores the second input.
	inline std::optional<bool> Logic(bool a, bool b, int64_t mode) {
		switch (mode) {
		case 0:
			return a && b;
		case 1:
			return a || b;
		case 2:
			return !a;
		case 3:
			return !(a && b);
		case 4:
			return !(a || b);
		case 5:
			return a != b;
		default:
			return std::nullopt;
		}
	}

	inline uint8_t QuantizeChannel(double channel) {
		return static_cast<uint8_t>(std::lround(std::clamp(channel, 0.0, 255.0)));
	}

	// node_color_rgb.gml clamps each component after optional 0..1 normalization.
	inline Colour MakeRgbColour(double red, double green, double blue, double alpha, bool normalized) {
		const double scale = normalized ? 255.0 : 1.0;
		return {
			QuantizeChannel(red * scale),
			QuantizeChannel(green * scale),
			QuantizeChannel(blue * scale),
			QuantizeChannel(alpha * scale)
		};
	}

	// node_color_data.gml uses weighted root-mean-square brightness.
	inline std::array<double, 8> ColorData(Colour colour, bool normalized) {
		const double scale = normalized ? 1.0 / 255.0 : 1.0;
		const double red = colour.Red * scale;
		const double green = colour.Green * scale;
		const double blue = colour.Blue * scale;
		const double maximum = std::max({red, green, blue});
		const double minimum = std::min({red, green, blue});
		const double chroma = maximum - minimum;
		double hue = 0.0;
		if (chroma > 0.0) {
			if (maximum == red)
				hue = std::fmod((green - blue) / chroma + 6.0, 6.0);
			else if (maximum == green)
				hue = (blue - red) / chroma + 2.0;
			else
				hue = (red - green) / chroma + 4.0;
			hue /= 6.0;
		}
		const double saturation = maximum == 0.0 ? 0.0 : chroma / maximum;
		const double brightness = std::sqrt(0.241 * red * red + 0.691 * green * green + 0.068 * blue * blue);
		const double hsvScale = normalized ? 1.0 : 255.0;
		return {
			red, green, blue, hue * hsvScale, saturation * hsvScale, maximum, brightness, colour.Alpha * scale
		};
	}

	// Inputs use the 0..255 hue, saturation and value scale of make_color_hsva.
	inline Colour MakeHsvColour(double hue, double saturation, double value, double alpha) {
		const double h = std::fmod(std::fmod(hue, 255.0) + 255.0, 255.0) / 255.0 * 6.0;
		const double s = std::clamp(saturation / 255.0, 0.0, 1.0);
		const double v = std::clamp(value / 255.0, 0.0, 1.0);
		const double chroma = v * s;
		const double x = chroma * (1.0 - std::abs(std::fmod(h, 2.0) - 1.0));
		const double offset = v - chroma;
		std::array<double, 3> rgb{};
		if (h < 1.0)
			rgb = {chroma, x, 0.0};
		else if (h < 2.0)
			rgb = {x, chroma, 0.0};
		else if (h < 3.0)
			rgb = {0.0, chroma, x};
		else if (h < 4.0)
			rgb = {0.0, x, chroma};
		else if (h < 5.0)
			rgb = {x, 0.0, chroma};
		else
			rgb = {chroma, 0.0, x};
		return {
			QuantizeChannel((rgb[0] + offset) * 255.0),
			QuantizeChannel((rgb[1] + offset) * 255.0),
			QuantizeChannel((rgb[2] + offset) * 255.0),
			QuantizeChannel(alpha)
		};
	}

	// RGB mode of node_color_mix.gml. Other source modes use distinct colour spaces.
	inline Colour MixRgbColour(Colour from, Colour to, double amount) {
		const auto mix = [amount](uint8_t a, uint8_t b) {
			return QuantizeChannel(static_cast<double>(a) + (static_cast<double>(b) - a) * amount);
		};
		return {
			mix(from.Red, to.Red),
			mix(from.Green, to.Green),
			mix(from.Blue, to.Blue),
			mix(from.Alpha, to.Alpha)
		};
	}

	// The source HSV mix interpolates channel values directly, without hue wrap.
	inline Colour MixHsvColour(Colour from, Colour to, double amount) {
		const auto first = ColorData(from, false);
		const auto second = ColorData(to, false);
		const auto mix = [amount](double a, double b) {
			return std::clamp(a + (b - a) * amount, 0.0, 255.0);
		};
		return MakeHsvColour(
			mix(first[3], second[3]),
			mix(first[4], second[4]),
			mix(first[5], second[5]),
			mix(from.Alpha, to.Alpha)
		);
	}

	// Pixel Composer's color_function.gml uses a cube-root matrix colour space
	// named OKLAB. Preserve its coefficients and 2.2 transfer exponent.
	inline Colour MixOklabColour(Colour from, Colour to, double amount) {
		const auto forward = [](Colour colour) {
			const double red = std::pow(colour.Red / 255.0, 2.2);
			const double green = std::pow(colour.Green / 255.0, 2.2);
			const double blue = std::pow(colour.Blue / 255.0, 2.2);
			return std::array<double, 3>{
				std::cbrt(0.4121656120 * red + 0.2118591070 * green + 0.0883097947 * blue),
				std::cbrt(0.5362752080 * red + 0.6807189584 * green + 0.2818474174 * blue),
				std::cbrt(0.0514575653 * red + 0.1074065790 * green + 0.6302613616 * blue)
			};
		};
		const auto first = forward(from);
		const auto second = forward(to);
		std::array<double, 3> channels{};
		for (size_t index = 0; index < channels.size(); index++)
			channels[index] = std::pow(first[index] + (second[index] - first[index]) * amount, 3.0);
		const double red =
			4.0767245293 * channels[0] - 1.2681437731 * channels[1] - 0.0041119885 * channels[2];
		const double green =
			-3.3072168827 * channels[0] + 2.6093323231 * channels[1] - 0.7034763098 * channels[2];
		const double blue =
			0.2307590544 * channels[0] - 0.3411344290 * channels[1] + 1.7068625689 * channels[2];
		return {
			QuantizeChannel(std::pow(std::max(0.0, red), 1.0 / 2.2) * 255.0),
			QuantizeChannel(std::pow(std::max(0.0, green), 1.0 / 2.2) * 255.0),
			QuantizeChannel(std::pow(std::max(0.0, blue), 1.0 / 2.2) * 255.0),
			QuantizeChannel(from.Alpha + (to.Alpha - from.Alpha) * amount)
		};
	}

	inline double VectorMagnitude(Vector2 value) {
		return std::hypot(value.X, value.Y);
	}

	// node_vector_normalize.gml returns a zero vector for zero length.
	inline Vector2 Normalize(Vector2 value) {
		const double magnitude = VectorMagnitude(value);
		if (magnitude == 0.0) return {};
		return {value.X / magnitude, value.Y / magnitude};
	}

	inline double Dot(Vector2 a, Vector2 b) {
		return a.X * b.X + a.Y * b.Y;
	}

	inline double Cross(Vector2 a, Vector2 b) {
		return a.X * b.Y - a.Y * b.X;
	}

	// GameMaker's point_direction measures positive angles clockwise on a Y-down canvas.
	inline double VectorDirection(Vector2 value, bool radians) {
		constexpr double PI = 3.14159265358979323846;
		double degrees = std::atan2(-value.Y, value.X) * 180.0 / PI;
		if (degrees < 0.0) degrees += 360.0;
		return radians ? degrees * PI / 180.0 : degrees;
	}
}
