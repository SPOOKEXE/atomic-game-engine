#pragma once

#include "NodeExecutors.hpp"

#include <cmath>

namespace engine::imagegraph::detail {
	inline bool SourceMappedSynthetic(const CatalogueEntry &entry, const CatalogueInput &input) {
		return (entry.Type == "pc.bevel" &&
				((input.SourceKind == "MapToggle" && input.Id == "height_mapped") ||
				 (input.SourceKind == "MapRange" && input.Id == "height_map_range"))) ||
			   (entry.Type == "pc.3_d_material" &&
				((input.SourceKind == "MapToggle" &&
				  (input.Id == "metalic_mapped" || input.Id == "roughness_mapped")) ||
				 (input.SourceKind == "MapRange" &&
				  (input.Id == "metalic_map_range" || input.Id == "roughness_map_range"))));
	}
	inline bool SourceRangeMapped(const NodeContext &context, std::string_view port) {
		if (context.Entry.Type == "pc.bevel" && port == "height") return context.Boolean("height_mapped");
		if (context.Entry.Type != "pc.3_d_material" || (port != "metalic" && port != "roughness"))
			return false;
		return context.Boolean(port == "metalic" ? "metalic_mapped" : "roughness_mapped");
	}
	// Source mapped numeric values gain array_depth1. Native static range fields
	// project the same source slot.
	inline const Value *SourceMappedRange(const NodeContext &context, std::string_view port) {
		const Value *value = context.Find(port);
		if (value && (context.IsLinked(port) || std::holds_alternative<ArrayValue>(*value) ||
					  std::holds_alternative<Vector2>(*value)))
			return value;
		if (context.Entry.Type == "pc.bevel" && port == "height") return context.Find("height_map_range");
		return context.Find(port == "metalic" ? "metalic_map_range" : "roughness_map_range");
	}
	inline bool ReadSourceMappedRange(NodeContext &context, std::string_view port, Vector2 &range) {
		const Value *value = SourceMappedRange(context, port);
		if (!value) {
			range = port == "height" ? Vector2{0, 4} : (port == "metalic" ? Vector2{} : Vector2{0, 1});
			return true;
		}
		if (context.Entry.Type == "pc.bevel" && port == "height") {
			if (const auto *integer = std::get_if<int64_t>(value)) {
				range = {double(*integer), double(*integer)};
				return true;
			}
			if (const auto *scalar = std::get_if<double>(value)) {
				range = {*scalar, *scalar};
				return std::isfinite(*scalar) ||
					   context.Fail(Status::InvalidValue, "mapped numeric range must be finite", port);
			}
		}
		if (const auto *tuple = std::get_if<Vector2>(value))
			range = *tuple;
		else if (const auto *array = std::get_if<ArrayValue>(value);
				 array && array->Nested.empty() && array->Elements.size() == 2) {
			const auto number = [](const ElementValue &element) -> std::optional<double> {
				if (const auto *scalar = std::get_if<double>(&element)) return *scalar;
				if (const auto *integer = std::get_if<int64_t>(&element)) return double(*integer);
				return std::nullopt;
			};
			const auto x = number(array->Elements[0]), y = number(array->Elements[1]);
			if (!x || !y)
				return context.Fail(
					Status::InvalidValue, "mapped numeric range needs two numeric endpoints", port
				);
			range = {*x, *y};
		} else
			return context.Fail(
				Status::InvalidValue, "mapped numeric range needs two numeric endpoints", port
			);
		return (std::isfinite(range.X) && std::isfinite(range.Y)) ||
			   context.Fail(Status::InvalidValue, "mapped numeric range must be finite", port);
	}
} // namespace engine::imagegraph::detail
