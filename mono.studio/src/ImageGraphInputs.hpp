#pragma once

#include "ImageGraphHlslInputs.hpp"

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
	inline std::optional<engine::imagegraph::ValueType>
	SourceHlslArgumentType(const engine::imagegraph::Node &node, std::string_view port) {
		using namespace engine::imagegraph;
		constexpr std::string_view prefix = "argument_value_";
		if (node.Type != "pc.hlsl" || !port.starts_with(prefix)) return {};
		const std::string selector = "argument_type_" + std::string(port.substr(prefix.size()));
		const Value *value = nullptr;
		for (const auto &held : node.Values)
			if (held.Port == selector) {
				value = &held.Data;
				break;
			}
		if (!value)
			for (const auto &held : node.DynamicInputs)
				if (held.Id == selector && held.Default) {
					value = &*held.Default;
					break;
				}
		const auto kind = value ? HlslArgumentKind(*value) : std::optional<int64_t>{0};
		return kind ? HlslArgumentType(*kind) : std::nullopt;
	}
	inline std::optional<engine::imagegraph::ValueType>
	SourceArgumentType(const engine::imagegraph::Node &node, std::string_view port) {
		if (node.Type == "pc.hlsl") return SourceHlslArgumentType(node, port);
		return SourceLuaArgumentType(node, port);
	}
	inline bool HlslRawArgumentValue(
		const engine::imagegraph::Node &node, std::string_view port, const engine::imagegraph::Value &value
	) {
		using namespace engine::imagegraph;
		const auto type = SourceHlslArgumentType(node, port);
		if (!type || *type == ValueType::Array) return false;
		if (const auto *number = std::get_if<double>(&value)) return std::isfinite(*number);
		if (std::holds_alternative<int64_t>(value)) return true;
		return (*type == ValueType::Colour && std::holds_alternative<Colour>(value)) ||
			   (*type == ValueType::Image && std::holds_alternative<SurfaceValue>(value));
	}
}
