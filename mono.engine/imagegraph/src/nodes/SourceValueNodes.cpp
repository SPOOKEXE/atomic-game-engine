#include "../ValueOps.hpp"
#include "Families.hpp"
#include "SourceOklch.hpp"

#include <array>
#include <cmath>
#include <numbers>
#include <span>

namespace engine::imagegraph::detail {
	namespace {
		bool BooleanValue(NodeContext &context) {
			const Value *value = context.Find("value");
			if (!value || !std::holds_alternative<bool>(*value))
				return context.Fail(Status::TypeMismatch, "Boolean requires a boolean value", "value");
			context.SetValue("boolean", *value);
			return context.FailureCode == Status::Ok;
		}

		// grug keep source make_color_rgba half-even rounding separate from image quantization.
		uint8_t SourceColourByte(double value) {
			value = std::clamp(value, 0.0, 255.0);
			const double lower = std::floor(value);
			const double fraction = value - lower;
			const bool roundUp = fraction > .5 || (fraction == .5 && std::fmod(lower, 2.0) != 0);
			return static_cast<uint8_t>(lower + roundUp);
		}

		bool RgbColour(NodeContext &context) {
			const double scale = context.Boolean("normalized", true) ? 255.0 : 1.0;
			std::array<double, 4> components{
				context.Scalar("red", 1),
				context.Scalar("green", 1),
				context.Scalar("blue", 1),
				context.Scalar("alpha", 1)
			};
			for (double component : components)
				if (!std::isfinite(component))
					return context.Fail(Status::InvalidValue, "RGB components must be finite", "color");
			context.SetValue(
				"color",
				Colour{
					SourceColourByte(components[0] * scale),
					SourceColourByte(components[1] * scale),
					SourceColourByte(components[2] * scale),
					SourceColourByte(components[3] * scale)
				}
			);
			return context.FailureCode == Status::Ok;
		}

		bool HsvColour(NodeContext &context) {
			const double scale = context.Boolean("normalized", true) ? 255.0 : 1.0;
			std::array<double, 4> components{
				context.Scalar("hue", 1),
				context.Scalar("saturation", 1),
				context.Scalar("value", 1),
				context.Scalar("alpha", 1)
			};
			for (double &component : components) {
				if (!std::isfinite(component))
					return context.Fail(Status::InvalidValue, "HSV components must be finite", "color");
				// grug preserve pinned source clamp before scaling, including non-normalized controls.
				component = std::clamp(component, 0.0, 1.0) * scale;
			}
			context.SetValue(
				"color", MakeHsvColour(components[0], components[1], components[2], components[3])
			);
			return context.FailureCode == Status::Ok;
		}

		bool ColourData(NodeContext &context) {
			const auto *value = context.Find("color");
			const auto *colour = value ? std::get_if<Colour>(value) : nullptr;
			if (!colour) return context.Fail(Status::TypeMismatch, "Color Data requires a colour", "color");
			const auto components = ColorData(*colour, context.Boolean("normalize", true));
			constexpr std::array ports{
				"red", "green", "blue", "hue", "saturation", "value", "brightness", "alpha"
			};
			for (size_t index = 0; index < ports.size(); ++index)
				context.SetValue(ports[index], components[index]);
			return context.FailureCode == Status::Ok;
		}

		bool BlendColour(NodeContext &context) {
			const auto *firstValue = context.Find("color_0");
			const auto *secondValue = context.Find("color_1");
			const auto *first = firstValue ? std::get_if<Colour>(firstValue) : nullptr;
			const auto *second = secondValue ? std::get_if<Colour>(secondValue) : nullptr;
			if (!first || !second)
				return context.Fail(Status::TypeMismatch, "Blend Color requires two colours", "color_0");
			const double intensity = context.Scalar("intensity", 1);
			if (!std::isfinite(intensity))
				return context.Fail(Status::InvalidValue, "blend intensity must be finite", "intensity");
			const int64_t mode = context.Integer("blend_mode", 0);
			const std::array<double, 3> a{first->Red / 255.0, first->Green / 255.0, first->Blue / 255.0};
			const std::array<double, 3> b{second->Red / 255.0, second->Green / 255.0, second->Blue / 255.0};
			const bool light = .299 * b[0] + .587 * b[1] + .114 * b[2] > .5;
			std::array<double, 3> blended{};
			if (mode >= 25 && mode <= 27) {
				const auto hsv0 = ColorData(*first, true);
				const auto hsv1 = ColorData(*second, true);
				// grug preserve source's normalized HSV passed to the byte-scale constructor.
				const auto colour = MakeHsvColour(
					mode == 25 ? hsv1[3] : hsv0[3],
					mode == 26 ? hsv1[4] : hsv0[4],
					mode == 27 ? hsv1[5] : hsv0[5],
					255
				);
				blended = {colour.Red / 255.0, colour.Green / 255.0, colour.Blue / 255.0};
			} else {
				for (size_t index = 0; index < 3; ++index) {
					const double x = a[index], y = b[index];
					switch (mode) {
					case 0:
					case 1:
						blended[index] = y;
						break;
					case 3:
						blended[index] = x * y;
						break;
					case 4:
						blended[index] = y == 0 ? 0 : std::max(0.0, 1 - (1 - x) / y);
						break;
					case 5:
						blended[index] = std::max(0.0, x + y - 1);
						break;
					case 6:
						blended[index] = std::min(x, y);
						break;
					case 8:
						blended[index] = std::min(1.0, x + y);
						break;
					case 9:
						blended[index] = 1 - (1 - x) * (1 - y);
						break;
					case 10:
						blended[index] = y == 1 ? 1 : std::min(1.0, x / (1 - y));
						break;
					case 11:
						blended[index] = std::max(x, y);
						break;
					case 13:
						blended[index] = light ? 1 - 2 * (1 - x) * (1 - y) : 2 * x * y;
						break;
					case 14:
						blended[index] = light ? 1 - (1 - x) * (1 - (y - .5)) : x * (y + .5);
						break;
					case 15:
						blended[index] = light ? 1 - (1 - x) * (1 - 2 * (y - .5)) : 2 * x * y;
						break;
					case 16:
						blended[index] = light ? 1 - (1 - x) * (2 * (y - .5)) : x / (1 - 2 * y);
						break;
					case 17:
						blended[index] = light ? x + 2 * (y - .5) : x + 2 * y - 1;
						break;
					case 18:
						blended[index] = light ? std::max(x, 2 * (y - .5)) : std::min(x, 2 * y);
						break;
					case 20:
						blended[index] = std::abs(x - y);
						break;
					case 21:
						blended[index] = x + y - 2 * x * y;
						break;
					case 22:
						blended[index] = std::max(0.0, x - y);
						break;
					case 23:
						blended[index] = y == 0 ? 1 : std::min(1.0, x / y);
						break;
					default:
						break;
					}
				}
			}
			std::array<uint8_t, 3> channels{};
			for (size_t index = 0; index < 3; ++index) {
				const double mixed = a[index] + (blended[index] - a[index]) * intensity;
				if (std::isnan(mixed))
					return context.Fail(
						Status::UnsupportedExecution,
						"source vivid-light NaN requires an independently verified numeric conversion",
						"blend_mode"
					);
				channels[index] = SourceColourByte(std::clamp(mixed, 0.0, 1.0) * 255);
			}
			context.SetValue("result", Colour{channels[0], channels[1], channels[2], 255});
			return context.FailureCode == Status::Ok;
		}

		bool OklchColour(NodeContext &context) {
			const double lightness = context.Scalar("lightness", .5);
			const double chroma = context.Scalar("chroma", .2);
			const double hue = context.Scalar("hue", 0);
			const double alpha = context.Scalar("alpha", 1);
			if (!std::isfinite(lightness) || !std::isfinite(chroma) || !std::isfinite(hue) ||
				!std::isfinite(alpha))
				return context.Fail(Status::InvalidValue, "OKLCH components must be finite", "color");
			const double radians = hue * (std::numbers::pi / 180.0);
			auto rgb = source_oklch::LinearToRgb(
				source_oklch::SourceOklabToLinear(
					{lightness, chroma * std::cos(radians), chroma * std::sin(radians)}
				)
			);
			const bool inRange =
				std::all_of(rgb.begin(), rgb.end(), [](double c) { return c >= 0 && c <= 1; });
			if (!inRange) {
				const auto mode = context.Integer("gamut_clipping", 0);
				if (mode == 0)
					rgb = source_oklch::ClipChroma(rgb);
				else if (mode == 1)
					rgb = source_oklch::ClipGrey(rgb);
				else if (mode == 2)
					rgb = source_oklch::ClipAdaptiveGrey(rgb);
				else if (mode == 3)
					for (double &component : rgb)
						component = std::clamp(component, 0.0, 1.0);
				else
					return context.Fail(
						Status::InvalidValue, "OKLCH gamut mode is invalid", "gamut_clipping"
					);
				for (double &component : rgb)
					if (std::isnan(component)) component = 0;
			}
			for (double component : rgb)
				if (!std::isfinite(component))
					return context.Fail(
						Status::InvalidValue, "OKLCH raw RGB exceeds finite range", "raw_rgb"
					);
			context.SetValue("raw_rgb", Vector3{rgb[0], rgb[1], rgb[2]});
			context.SetValue(
				"color",
				Colour{
					SourceColourByte(rgb[0] * 255),
					SourceColourByte(rgb[1] * 255),
					SourceColourByte(rgb[2] * 255),
					SourceColourByte(alpha * 255)
				}
			);
			return context.FailureCode == Status::Ok;
		}

		constexpr ExecutorEntry SOURCE_VALUE_EXECUTORS[]{
			{"pc.boolean", BooleanValue},
			{"pc.color_rgb", RgbColour},
			{"pc.color_hsv", HsvColour},
			{"pc.color_data", ColourData},
			{"pc.color_math", BlendColour},
			{"pc.color_oklch", OklchColour}
		};
	}

	std::span<const ExecutorEntry> SourceValueExecutors() {
		return SOURCE_VALUE_EXECUTORS;
	}
}
