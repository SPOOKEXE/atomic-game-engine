#pragma once

// Shared sh_interpret_number/sh_interpret_matrix GLSL palette and gradient profile.
#include "Gradient.hpp"

#include <type_traits>

namespace engine::imagegraph::detail {
	// GameMaker packed colour channels occupy the low 32 bits. Fractional conversion is unverified.
	template <class T> inline std::optional<Colour> InterpretPackedColour(const T &raw) {
		if constexpr (std::is_same_v<T, Colour>)
			return raw;
		else if constexpr (std::is_same_v<T, int64_t> || std::is_same_v<T, bool> ||
						   std::is_same_v<T, double> || std::is_same_v<T, EnumValue>) {
			const double number = [&] {
				if constexpr (std::is_same_v<T, EnumValue>)
					return double(raw.Value);
				else
					return double(raw);
			}();
			if (!std::isfinite(number) || std::trunc(number) != number || number < -2147483648.0 ||
				number > 4294967295.0)
				return std::nullopt;
			const uint32_t packed = uint32_t(int64_t(number));
			return Colour{
				uint8_t(packed), uint8_t(packed >> 8), uint8_t(packed >> 16), uint8_t(packed >> 24)
			};
		} else
			return std::nullopt;
	}
	inline Rgba InterpretColour(NodeContext &context, double value, bool shaderUniform = false) {
		const int64_t mode = context.Integer("mode");
		// Array-selected EButtons bypass clamping; both shaders leave their initialized zero output.
		if (mode < 0 || mode > 2) return {};
		if (mode == 1) {
			const auto *palette = std::get_if<ArrayValue>(context.Find("palette"));
			if (!palette || !palette->Nested.empty()) {
				context.Fail(
					Status::InvalidValue, "Interpret palette requires a flat colour array", "palette"
				);
				return {};
			}
			const size_t amount = std::min<size_t>(
				256, palette->Items.empty() ? palette->Elements.size() : palette->Items.size()
			);
			if (!amount) {
				context.Fail(
					Status::UnsupportedExecution,
					"Empty Interpret palette retains uncaptured shader uniforms",
					"palette"
				);
				return {};
			}
			if (!std::isfinite(value) || std::trunc(value) < 0 ||
				value > double(std::numeric_limits<int32_t>::max())) {
				context.Fail(
					Status::InvalidValue, "Palette index must fit a nonnegative shader integer", "number"
				);
				return {};
			}
			const size_t index = size_t(value) % amount;
			const ElementValue *element = palette->Items.empty()
											  ? &palette->Elements[index]
											  : std::get_if<ElementValue>(&palette->Items[index].Data);
			const auto colour =
				element ? std::visit([](const auto &raw) { return InterpretPackedColour(raw); }, *element)
						: std::optional<Colour>{};
			if (!colour) {
				context.Fail(
					Status::UnsupportedExecution,
					"Interpret palette requires a resolved packed source colour",
					"palette"
				);
				return {};
			}
			return {colour->Red / 255.0, colour->Green / 255.0, colour->Blue / 255.0, colour->Alpha / 255.0};
		}
		Vector2 range = context.Vec2("range", {0, 1});
		if (shaderUniform) range = {double(float(range.X)), double(float(range.Y))};
		if (!std::isfinite(value) || !std::isfinite(range.X) || !std::isfinite(range.Y) ||
			range.X == range.Y) {
			context.Fail(Status::InvalidValue, "Interpret range requires a finite nonzero span", "range");
			return {};
		}
		const double grey = shaderUniform
								? double((float(value) - float(range.X)) / (float(range.Y) - float(range.X)))
								: (value - range.X) / (range.Y - range.X);
		if (mode == 0) return {grey, grey, grey, 1};
		const auto *gradient = std::get_if<Gradient>(context.Find("gradient"));
		if (!gradient || !std::isfinite(grey)) {
			context.Fail(Status::InvalidValue, "Interpret gradient requires finite progress", "gradient");
			return {};
		}
		const double shift = shaderUniform ? double(float(context.Scalar("shift"))) : context.Scalar("shift");
		return GradientEval(
			ReadGradient(context, "gradient", *gradient), ShaderFract(ShaderFract(grey + shift) + 1)
		);
	}
}
