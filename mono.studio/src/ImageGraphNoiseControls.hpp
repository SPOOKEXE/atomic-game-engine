#pragma once

#include <engine/imagegraph/NoiseField.hpp>

#include <imgui.h>
#include <iterator>
#include <optional>
#include <string_view>

namespace studio::detail {
	inline std::optional<engine::imagegraph::Value>
	ImageGraphNoisePositionDefault(const engine::imagegraph::Node &node) {
		using namespace engine::imagegraph;
		const auto port = NoiseNodePort(node, "position", PortDirection::Input);
		if (!port) return std::nullopt;
		if (port->Type == ValueType::Vector2) return Value{Vector2{}};
		if (port->Type == ValueType::Vector3) return Value{Vector3{}};
		if (port->Type == ValueType::Scalar) return Value{0.0};
		return std::nullopt;
	}

	inline bool IsImageGraphNoiseSelector(std::string_view nodeType, std::string_view property) {
		return (property == "output_type" &&
				(nodeType == "value.noise_field" || nodeType == "value.sample_noise")) ||
			   (nodeType == "value.noise_field" && (property == "mode" || property == "dimension"));
	}

	inline bool
	ImageGraphNoisePropertyVisible(const engine::imagegraph::Node &node, std::string_view property) {
		if (node.Type != "value.noise_field" || property != "position") return true;
		for (const auto &value : node.Values)
			if (value.Port == "mode")
				if (const auto *mode = std::get_if<engine::imagegraph::EnumValue>(&value.Data))
					return mode->Value != 0;
		return false;
	}

	inline std::optional<bool> DrawImageGraphNoiseChoice(
		std::string_view nodeType, std::string_view property, engine::imagegraph::EnumValue &value
	) {
		static constexpr const char *modes[]{"Generator", "Computed"};
		static constexpr const char *dimensions[]{"1D", "2D", "3D"};
		static constexpr const char *outputs[]{"Scalar", "Vector2", "Vector3"};
		const char *const *choices = nullptr;
		size_t count = 0;
		if (!IsImageGraphNoiseSelector(nodeType, property)) return std::nullopt;
		if (property == "mode") {
			choices = modes;
			count = std::size(modes);
		} else if (property == "dimension") {
			choices = dimensions;
			count = std::size(dimensions);
		} else {
			choices = outputs;
			count = std::size(outputs);
		}

		const int64_t minimum = property == "mode" ? 0 : 1;
		const int64_t maximum = minimum + static_cast<int64_t>(count) - 1;
		const char *preview = value.Value >= minimum && value.Value <= maximum
								  ? choices[static_cast<size_t>(value.Value - minimum)]
								  : "Unknown";
		if (!ImGui::BeginCombo("##value", preview)) return false;
		bool changed = false;
		for (size_t index = 0; index < count; ++index) {
			const auto selected = minimum + static_cast<int64_t>(index);
			const bool active = value.Value == selected;
			if (ImGui::Selectable(choices[index], active)) {
				value.Value = selected;
				changed = true;
			}
			if (active) ImGui::SetItemDefaultFocus();
		}
		ImGui::EndCombo();
		return changed;
	}
}
