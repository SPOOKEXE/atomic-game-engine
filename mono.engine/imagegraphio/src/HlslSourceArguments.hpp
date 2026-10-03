#pragma once

#include <engine/imagegraph/Document.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <optional>

namespace engine::imagegraphio::detail {
	inline std::optional<imagegraph::ValueType> HlslArgumentType(int64_t mode) {
		using imagegraph::ValueType;
		constexpr std::array types{
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
		return mode >= 0 && mode < int64_t(types.size()) ? std::optional{types[size_t(mode)]} : std::nullopt;
	}
	inline size_t HlslArgumentLength(int64_t mode) {
		constexpr std::array<size_t, 9> lengths{0, 0, 2, 3, 4, 9, 16, 0, 0};
		return mode >= 0 && mode < int64_t(lengths.size()) ? lengths[size_t(mode)] : 0;
	}
	inline bool HlslArgumentValue(int64_t mode, const imagegraph::Value &value) {
		using namespace imagegraph;
		const auto numeric = [](const auto &item) {
			return std::holds_alternative<int64_t>(item) ||
				   (std::holds_alternative<double>(item) && std::isfinite(std::get<double>(item)));
		};
		if (mode == 0 || mode == 1 || mode == 7 || mode == 8)
			return numeric(value) || (mode == 8 && std::holds_alternative<Colour>(value));
		const auto *array = std::get_if<ArrayValue>(&value);
		const size_t width = HlslArgumentLength(mode);
		if (!width || !array || !array->Items.empty() ||
			(array->ElementType != ValueType::Scalar && array->ElementType != ValueType::Integer))
			return false;
		const auto row = [&](const auto &items) {
			return items.size() == width && std::all_of(items.begin(), items.end(), numeric);
		};
		if (!array->Nested.empty())
			return array->Elements.empty() && std::all_of(array->Nested.begin(), array->Nested.end(), row);
		return row(array->Elements);
	}
}
