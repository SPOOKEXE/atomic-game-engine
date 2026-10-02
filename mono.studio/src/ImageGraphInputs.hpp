#pragma once

#include <engine/imagegraph/Document.hpp>

#include <cmath>

namespace studio::detail {
	inline std::optional<engine::imagegraph::ValueType>
	SourceLuaArgumentType(const engine::imagegraph::Node &node, std::string_view port) {
		using namespace engine::imagegraph;
		constexpr std::string_view prefix = "argument_value_";
		if (!node.Type.starts_with("pc.lua_") || !port.starts_with(prefix)) return std::nullopt;
		const std::string selector = "argument_type_" + std::string(port.substr(prefix.size()));
		const Value *value = nullptr;
		for (const auto &input : node.Values)
			if (input.Port == selector) {
				value = &input.Data;
				break;
			}
		if (!value)
			for (const auto &input : node.DynamicInputs)
				if (input.Id == selector && input.Default) {
					value = &*input.Default;
					break;
				}
		int64_t selected = 0;
		if (value) {
			if (const auto *choice = std::get_if<EnumValue>(value))
				selected = choice->Value;
			else if (const auto *integer = std::get_if<int64_t>(value))
				selected = *integer;
			else if (const auto *scalar = std::get_if<double>(value)) {
				if (*scalar < 0 || *scalar > 3 || !std::isfinite(*scalar) || std::floor(*scalar) != *scalar)
					return std::nullopt;
				selected = int64_t(*scalar);
			} else
				return std::nullopt;
		}
		switch (selected) {
		case 0:
			return ValueType::Scalar;
		case 1:
			return ValueType::Text;
		case 2:
			return ValueType::Image;
		case 3:
			return ValueType::Struct;
		default:
			return std::nullopt;
		}
	}
}
