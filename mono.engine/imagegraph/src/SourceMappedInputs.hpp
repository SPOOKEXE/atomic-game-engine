#pragma once

#include "NodeExecutors.hpp"

#include <cmath>

namespace engine::imagegraph::detail {
	inline bool SourceMappedSynthetic(const CatalogueEntry &entry, const CatalogueInput &input) {
		return (entry.Type == "pc.dither" &&
				((input.SourceKind == "MapToggle" && input.Id == "contrast_mapped") ||
				 (input.SourceKind == "MapRange" && input.Id == "contrast_map_range"))) ||
			   (entry.Type == "pc.ambient_occlusion" &&
				((input.SourceKind == "MapToggle" &&
				  (input.Id == "height_mapped" || input.Id == "intensity_mapped")) ||
				 (input.SourceKind == "MapRange" &&
				  (input.Id == "height_map_range" || input.Id == "intensity_map_range")))) ||
			   (entry.Type == "pc.gradient" &&
				((input.SourceKind == "MapToggle" &&
				  (input.Id == "angle_mapped" || input.Id == "radius_mapped" || input.Id == "shift_mapped" ||
				   input.Id == "scale_mapped")) ||
				 (input.SourceKind == "MapRange" &&
				  (input.Id == "angle_map_range" || input.Id == "radius_map_range" ||
				   input.Id == "shift_map_range" || input.Id == "scale_map_range")))) ||
			   (entry.Type == "pc.erode" &&
				((input.SourceKind == "MapToggle" && input.Id == "width_mapped") ||
				 (input.SourceKind == "MapRange" && input.Id == "width_map_range"))) ||
			   (entry.Type == "pc.bevel" &&
				((input.SourceKind == "MapToggle" && input.Id == "height_mapped") ||
				 (input.SourceKind == "MapRange" && input.Id == "height_map_range"))) ||
			   (entry.Type == "pc.3_d_material" &&
				((input.SourceKind == "MapToggle" &&
				  (input.Id == "metalic_mapped" || input.Id == "roughness_mapped")) ||
				 (input.SourceKind == "MapRange" &&
				  (input.Id == "metalic_map_range" || input.Id == "roughness_map_range")))) ||
			   (entry.Type == "pc.noise_simplex" &&
				((input.SourceKind == "MapToggle" &&
				  (input.Id == "iteration_mapped" || input.Id == "scale_mapped")) ||
				 (input.SourceKind == "MapRange" &&
				  (input.Id == "iteration_map_range" || input.Id == "scale_map_range"))));
	}
	inline bool SourceRangeMapped(const NodeContext &context, std::string_view port) {
		if (context.Entry.Type == "pc.dither" && port == "contrast")
			return context.Boolean("contrast_mapped");
		if (context.Entry.Type == "pc.ambient_occlusion" && (port == "height" || port == "intensity"))
			return context.Boolean(port == "height" ? "height_mapped" : "intensity_mapped");
		if (context.Entry.Type == "pc.gradient" &&
			(port == "angle" || port == "radius" || port == "shift" || port == "scale"))
			return context.Boolean(std::string(port) + "_mapped");
		if (context.Entry.Type == "pc.erode" && port == "width") return context.Boolean("width_mapped");
		if (context.Entry.Type == "pc.bevel" && port == "height") return context.Boolean("height_mapped");
		if (context.Entry.Type == "pc.noise_simplex" && (port == "iteration" || port == "scale"))
			return context.Boolean(port == "iteration" ? "iteration_mapped" : "scale_mapped");
		if (context.Entry.Type != "pc.3_d_material" || (port != "metalic" && port != "roughness"))
			return false;
		return context.Boolean(port == "metalic" ? "metalic_mapped" : "roughness_mapped");
	}
	// Source mapped numeric values gain array_depth1. Native static range fields
	// project the same source slot.
	inline const Value *SourceMappedRange(const NodeContext &context, std::string_view port) {
		const Value *value = context.Find(port);
		const bool simplex =
			context.Entry.Type == "pc.noise_simplex" && (port == "iteration" || port == "scale");
		const bool drawGradient = context.Entry.Type == "pc.gradient" &&
								  (port == "angle" || port == "radius" || port == "shift" || port == "scale");
		if (drawGradient && !context.IsLinked(port) && context.IsCatalogueDefault(port).value_or(false))
			return context.Find(
				port == "angle"
					? "angle_map_range"
					: (port == "radius" ? "radius_map_range"
										: (port == "shift" ? "shift_map_range" : "scale_map_range"))
			);
		if (drawGradient && value) return value;
		const bool simplexDefault =
			simplex && !context.IsLinked(port) && context.IsCatalogueDefault(port).value_or(false);
		if (simplexDefault)
			return context.Find(port == "iteration" ? "iteration_map_range" : "scale_map_range");
		if (value && (context.IsLinked(port) || std::holds_alternative<ArrayValue>(*value) ||
					  std::holds_alternative<Vector2>(*value)))
			return value;
		if (value && simplex && port == "iteration" && !context.IsLinked(port) &&
			!context.IsCatalogueDefault(port).value_or(false) &&
			(std::holds_alternative<int64_t>(*value) || std::holds_alternative<double>(*value)))
			return value;
		if (context.Entry.Type == "pc.dither" && port == "contrast")
			return context.Find("contrast_map_range");
		if (context.Entry.Type == "pc.ambient_occlusion")
			return context.Find(port == "height" ? "height_map_range" : "intensity_map_range");
		if (context.Entry.Type == "pc.erode" && port == "width") return context.Find("width_map_range");
		if (context.Entry.Type == "pc.bevel" && port == "height") return context.Find("height_map_range");
		if (simplex) return context.Find(port == "iteration" ? "iteration_map_range" : "scale_map_range");
		return context.Find(port == "metalic" ? "metalic_map_range" : "roughness_map_range");
	}
	inline bool ReadSourceMappedRange(NodeContext &context, std::string_view port, Vector2 &range) {
		const Value *value = SourceMappedRange(context, port);
		if (!value && context.Entry.Type == "pc.gradient") {
			range = port == "radius" ? Vector2{0, .5} : (port == "scale" ? Vector2{0, 1} : Vector2{});
			return true;
		}
		if (!value && context.Entry.Type == "pc.noise_simplex") {
			range = port == "iteration" ? Vector2{0, 1} : Vector2{0.25, 0.25};
			return true;
		}
		if (!value) {
			range = port == "width"	   ? Vector2{0, 1}
					: port == "height" ? Vector2{0, 4}
									   : (port == "metalic" ? Vector2{} : Vector2{0, 1});
			return true;
		}
		if ((context.Entry.Type == "pc.dither" && port == "contrast") ||
			(context.Entry.Type == "pc.gradient" &&
			 (port == "angle" || port == "radius" || port == "shift" || port == "scale")) ||
			context.Entry.Type == "pc.ambient_occlusion" ||
			(context.Entry.Type == "pc.bevel" && port == "height") ||
			(context.Entry.Type == "pc.erode" && port == "width") ||
			(context.Entry.Type == "pc.noise_simplex" && port == "iteration")) {
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
