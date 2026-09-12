#include "Compositor.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>
#include <span>
#include <string_view>

namespace engine::render {
	namespace {
		float Number(const graph::Node &node, const char *name, float fallback, float low, float high) {
			const float value = node.Number(core::Name(name), fallback);
			return std::isfinite(value) ? std::clamp(value, low, high) : fallback;
		}

		float Choice(
			const graph::Node &node,
			const char *name,
			std::span<const std::string_view> options,
			std::string_view fallback
		) {
			const std::string *authored = node.Parameter(core::Name(name));
			const std::string_view selected = authored == nullptr ? fallback : std::string_view(*authored);
			const auto found = std::find(options.begin(), options.end(), selected);
			const auto fallbackFound = std::find(options.begin(), options.end(), fallback);
			return static_cast<float>(
				found != options.end() ? found - options.begin() : fallbackFound - options.begin()
			);
		}
	}

	CompositorParameters CompositorParametersFor(const graph::Node &node) {
		CompositorParameters out{};
		if (node.Kind == core::Name("exposure-grade")) {
			out[0] = glm::vec4{
				Number(node, "exposure", 0.0f, -16.0f, 16.0f),
				Number(node, "contrast", 1.0f, 0.0f, 4.0f),
				Number(node, "pivot", 0.18f, 0.0f, 4.0f),
				Number(node, "gamma", 1.0f, 0.01f, 8.0f),
			};
		} else if (node.Kind == core::Name("hsv")) {
			out[0] = glm::vec4{
				Number(node, "hue", 0.0f, -360.0f, 360.0f) / 360.0f,
				Number(node, "saturation", 1.0f, 0.0f, 4.0f),
				Number(node, "value", 1.0f, 0.0f, 16.0f),
				Number(node, "factor", 1.0f, 0.0f, 1.0f),
			};
		} else if (node.Kind == core::Name("mix")) {
			constexpr std::array operations{
				std::string_view("mix"),
				std::string_view("add"),
				std::string_view("multiply"),
				std::string_view("screen"),
				std::string_view("overlay"),
				std::string_view("subtract"),
				std::string_view("difference"),
				std::string_view("alpha-over"),
			};
			constexpr std::array clampModes{
				std::string_view("none"), std::string_view("zero"), std::string_view("unit")
			};
			out[0] = glm::vec4{
				Choice(node, "operation", operations, "alpha-over"),
				Number(node, "factor", 1.0f, 0.0f, 1.0f),
				Choice(node, "clamp", clampModes, "none"),
				0.0f,
			};
		} else if (node.Kind == core::Name("transform-crop")) {
			constexpr std::array extendModes{
				std::string_view("transparent"),
				std::string_view("clamp"),
				std::string_view("repeat"),
				std::string_view("mirror"),
			};
			out[0] = glm::vec4{
				Number(node, "scale-x", 1.0f, 0.01f, 100.0f),
				Number(node, "scale-y", 1.0f, 0.01f, 100.0f),
				Number(node, "translate-x", 0.0f, -10.0f, 10.0f),
				Number(node, "translate-y", 0.0f, -10.0f, 10.0f),
			};
			out[1] = glm::vec4{
				Number(node, "rotation", 0.0f, -360.0f, 360.0f) * std::numbers::pi_v<float> / 180.0f,
				Number(node, "crop-left", 0.0f, 0.0f, 1.0f),
				Number(node, "crop-top", 0.0f, 0.0f, 1.0f),
				Number(node, "crop-right", 1.0f, 0.0f, 1.0f),
			};
			out[2] = glm::vec4{
				Number(node, "crop-bottom", 1.0f, 0.0f, 1.0f),
				Choice(node, "extend", extendModes, "transparent"),
				0.0f,
				0.0f,
			};
		} else if (node.Kind == core::Name("blur")) {
			constexpr std::array kernels{std::string_view("gaussian"), std::string_view("box")};
			out[0] = glm::vec4{
				Choice(node, "kernel", kernels, "gaussian"),
				Number(node, "radius", 4.0f, 0.0f, 32.0f),
				Number(node, "sigma", 2.0f, 0.01f, 32.0f),
				Number(node, "angle", 0.0f, -360.0f, 360.0f) * std::numbers::pi_v<float> / 180.0f,
			};
		}
		return out;
	}
}
