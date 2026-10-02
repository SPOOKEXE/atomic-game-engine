#pragma once
#include <engine/imagegraph/Document.hpp>

#include <charconv>
#include <cmath>
namespace engine::imagegraph::detail {
	// Source refreshInputType mutates each argument socket from its paired enum control.
	inline std::optional<ValueType> SourceLuaArgumentType(const Node &node, std::string_view port) {
		if (node.Type != "pc.lua_compute" && node.Type != "pc.lua_surface") return std::nullopt;
		constexpr std::string_view prefix = "argument_value_";
		if (!port.starts_with(prefix)) return std::nullopt;
		const auto suffix = port.substr(prefix.size());
		size_t group = 0;
		const auto parsed = std::from_chars(suffix.data(), suffix.data() + suffix.size(), group);
		if (parsed.ec != std::errc{} || parsed.ptr != suffix.data() + suffix.size()) return std::nullopt;
		const std::string selector = "argument_type_" + std::string(suffix);
		const Value *control = nullptr;
		for (const auto &value : node.Values)
			if (value.Port == selector) {
				control = &value.Data;
				break;
			}
		if (!control)
			for (const auto &input : node.DynamicInputs)
				if (input.Id == selector && input.Default) {
					control = &*input.Default;
					break;
				}
		double mode = 0;
		if (control) {
			if (const auto *v = std::get_if<EnumValue>(control))
				mode = double(v->Value);
			else if (const auto *v = std::get_if<int64_t>(control))
				mode = double(*v);
			else if (const auto *v = std::get_if<double>(control))
				mode = *v;
			else
				return ValueType(255);
		}
		if (!std::isfinite(mode) || mode < 0 || mode > 3 || std::floor(mode) != mode) return ValueType(255);
		constexpr std::array types{ValueType::Scalar, ValueType::Text, ValueType::Image, ValueType::Struct};
		return types[size_t(mode)];
	}
}
