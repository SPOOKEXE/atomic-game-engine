#pragma once

#include <engine/imagegraph/Document.hpp>

#include <charconv>
#include <cmath>

namespace engine::imagegraph::detail {
	inline bool PuppetControlPort(const Node &node, std::string_view port, ValueType type) {
		constexpr std::string_view prefix = "control_point_";
		if (node.Type != "pc.mesh_warp" || type != ValueType::Struct || !port.starts_with(prefix))
			return false;
		const auto suffix = port.substr(prefix.size());
		uint32_t index = 0;
		const auto parsed = std::from_chars(suffix.data(), suffix.data() + suffix.size(), index);
		return !suffix.empty() && parsed.ec == std::errc{} && parsed.ptr == suffix.data() + suffix.size() &&
			   index < Limits::MaximumDynamicInputsPerNode;
	}

	// Source Puppet stores seven numeric controls in a Struct widget's animator.
	inline bool
	PuppetControlValue(const Node &node, std::string_view port, ValueType type, const Value &value) {
		if (!PuppetControlPort(node, port, type)) return false;
		const auto *array = std::get_if<ArrayValue>(&value);
		if (!array || array->ElementType != ValueType::Scalar || array->Elements.size() != 7 ||
			!array->Nested.empty() || !array->Items.empty())
			return false;
		for (const auto &element : array->Elements) {
			const auto *number = std::get_if<double>(&element);
			if (!number || !std::isfinite(*number)) return false;
		}
		return true;
	}
}
