#pragma once
#include <engine/imagegraph/Document.hpp>

#include <charconv>
#include <cmath>
namespace engine::imagegraph::detail {
	// Source refreshInputType mutates each argument socket from its paired enum
	// control.
	inline std::optional<ValueType>
	SourceArgumentType(const Node &node, std::string_view port, const Value *selectorOverride = nullptr) {
		if (node.Type != "pc.lua_compute" && node.Type != "pc.lua_surface" && node.Type != "pc.hlsl")
			return std::nullopt;
		constexpr std::string_view prefix = "argument_value_";
		if (!port.starts_with(prefix)) return std::nullopt;
		const auto suffix = port.substr(prefix.size());
		size_t group = 0;
		const auto parsed = std::from_chars(suffix.data(), suffix.data() + suffix.size(), group);
		if (parsed.ec != std::errc{} || parsed.ptr != suffix.data() + suffix.size()) return std::nullopt;
		const std::string selector = "argument_type_" + std::string(suffix);
		const Value *control = selectorOverride;
		if (!control)
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
		if (!std::isfinite(mode) || mode < 0 || mode > (node.Type == "pc.hlsl" ? 8 : 3) ||
			std::floor(mode) != mode)
			return ValueType(255);
		if (node.Type == "pc.hlsl") {
			constexpr std::array hlsl{
				ValueType::Scalar,
				ValueType::Integer,
				ValueType::Array,
				ValueType::Array,
				ValueType::Array,
				ValueType::Array,
				ValueType::Array,
				ValueType::Image,
				ValueType::Colour
			};
			return hlsl[size_t(mode)];
		}
		constexpr std::array types{ValueType::Scalar, ValueType::Text, ValueType::Image, ValueType::Struct};
		return types[size_t(mode)];
	}
	inline std::optional<ValueType> SourceLuaArgumentType(const Node &node, std::string_view port) {
		return node.Type == "pc.hlsl" ? std::nullopt : SourceArgumentType(node, port);
	}
	inline bool SourceHlslArgumentValue(
		const Node &node,
		std::string_view port,
		const Value &value,
		const Value *selectorOverride = nullptr,
		bool processorRows = false
	) {
		if (node.Type != "pc.hlsl") return false;
		const auto type = SourceArgumentType(node, port, selectorOverride);
		if (!type || *type == ValueType(255)) return false;
		if (*type == ValueType::Scalar || *type == ValueType::Integer) {
			if (const auto *scalar = std::get_if<double>(&value)) return std::isfinite(*scalar);
			if (std::holds_alternative<int64_t>(value)) return true;
			const auto *array = processorRows ? std::get_if<ArrayValue>(&value) : nullptr;
			if (!array || !array->Nested.empty() || !array->Items.empty() ||
				(array->ElementType != ValueType::Scalar && array->ElementType != ValueType::Integer))
				return false;
			for (const auto &leaf : array->Elements) {
				if (const auto *scalar = std::get_if<double>(&leaf)) {
					if (!std::isfinite(*scalar)) return false;
				} else if (!std::holds_alternative<int64_t>(leaf))
					return false;
			}
			return true;
		}
		const bool numeric =
			std::holds_alternative<int64_t>(value) ||
			(std::holds_alternative<double>(value) && std::isfinite(std::get<double>(value)));
		if (*type == ValueType::Colour) return std::holds_alternative<Colour>(value) || numeric;
		if (*type == ValueType::Image) return std::holds_alternative<SurfaceValue>(value) || numeric;
		const std::string selector =
			"argument_type_" + std::string(port.substr(std::string_view("argument_value_").size()));
		const Value *control = selectorOverride;
		if (!control)
			for (const auto &item : node.Values)
				if (item.Port == selector) control = &item.Data;
		if (!control)
			for (const auto &item : node.DynamicInputs)
				if (item.Id == selector && item.Default) control = &*item.Default;
		if (!control) return false;
		const auto mode = std::visit(
			[](const auto &item) -> size_t {
				using T = std::decay_t<decltype(item)>;
				if constexpr (std::is_same_v<T, EnumValue>)
					return size_t(item.Value);
				else if constexpr (std::is_same_v<T, double> || std::is_same_v<T, int64_t>)
					return size_t(item);
				else
					return 0;
			},
			*control
		);
		constexpr std::array<size_t, 7> lengths{0, 0, 2, 3, 4, 9, 16};
		if (mode >= lengths.size()) return false;
		if (const auto *tuple = std::get_if<Vector2>(&value))
			return lengths[mode] == 2 && std::isfinite(tuple->X) && std::isfinite(tuple->Y);
		if (const auto *tuple = std::get_if<Vector3>(&value))
			return lengths[mode] == 3 && std::isfinite(tuple->X) && std::isfinite(tuple->Y) &&
				   std::isfinite(tuple->Z);
		if (const auto *tuple = std::get_if<Vector4>(&value))
			return lengths[mode] == 4 && std::isfinite(tuple->X) && std::isfinite(tuple->Y) &&
				   std::isfinite(tuple->Z) && std::isfinite(tuple->W);
		const auto *array = std::get_if<ArrayValue>(&value);
		if (!array || !array->Items.empty() ||
			(array->ElementType != ValueType::Scalar && array->ElementType != ValueType::Integer))
			return false;
		const auto validRow = [&](const std::vector<ElementValue> &row) {
			if (row.size() != lengths[mode]) return false;
			for (const auto &item : row) {
				if (const auto *scalar = std::get_if<double>(&item)) {
					if (!std::isfinite(*scalar)) return false;
				} else if (!std::holds_alternative<int64_t>(item))
					return false;
			}
			return true;
		};
		if (array->Nested.empty()) return validRow(array->Elements);
		if (!array->Elements.empty()) return false;
		for (const auto &row : array->Nested)
			if (!validRow(row)) return false;
		return true;
	}

} // namespace engine::imagegraph::detail
