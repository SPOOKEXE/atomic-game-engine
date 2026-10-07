#pragma once

#include "NodeExecutors.hpp"

#include <cmath>

namespace engine::imagegraph::detail {
	inline bool SourceMappedSynthetic(const CatalogueEntry &entry, const CatalogueInput &input) {
		if (entry.Type == "pc.perlin_extra" &&
			(input.SourceKind == "ValueUnit" || input.SourceKind == "MaskAlphaOnly" ||
			 input.SourceKind == "MapToggle" || input.SourceKind == "MapRange"))
			return true;
		if (entry.Type == "pc.pytagorean_tile" &&
			(input.SourceKind == "ValueUnit" || input.SourceKind == "MaskAlphaOnly" ||
			 input.SourceKind == "MapToggle" || input.SourceKind == "MapRange" ||
			 (input.SourceKind == "Attribute" && (input.Id == "interpolate" || input.Id == "oversample"))))
			return true;
		if (entry.Type == "pc.weave" &&
			((input.SourceKind == "ValueUnit" && (input.Id == "position_unit" || input.Id == "scale_unit")) ||
			 (input.SourceKind == "MaskAlphaOnly" && input.Id == "mask_alpha_only") ||
			 (input.SourceKind == "CurveToggle" && input.Id == "shading_curved")))
			return true;
		if (entry.Type == "pc.noise_strand" &&
			((input.SourceKind == "ValueUnit" && input.Id == "position_unit") ||
			 (input.SourceKind == "MaskAlphaOnly" && input.Id == "mask_alpha_only")))
			return true;
		if (entry.Type == "pc.shard_noise" &&
			((input.SourceKind == "ValueUnit" && input.Id == "position_unit") ||
			 (input.SourceKind == "MaskAlphaOnly" && input.Id == "mask_alpha_only") ||
			 (input.SourceKind == "MapToggle" &&
			  (input.Id == "scale_mapped" || input.Id == "progress_mapped" ||
			   input.Id == "sharpness_mapped")) ||
			 (input.SourceKind == "MapRange" &&
			  (input.Id == "scale_map_range" || input.Id == "progress_map_range" ||
			   input.Id == "sharpness_map_range"))))
			return true;
		if (entry.Type == "pc.voronoi_extra" &&
			((input.SourceKind == "ValueUnit" && input.Id == "position_unit") ||
			 (input.SourceKind == "MaskAlphaOnly" && input.Id == "mask_alpha_only")))
			return true;
		if (entry.Type == "pc.perlin" &&
			((input.SourceKind == "ValueUnit" && input.Id == "position_unit") ||
			 (input.SourceKind == "MaskAlphaOnly" && input.Id == "mask_alpha_only") ||
			 (input.SourceKind == "MapToggle" &&
			  (input.Id == "scale_mapped" || input.Id == "scaling_mapped" ||
			   input.Id == "amplitude_mapped")) ||
			 (input.SourceKind == "MapRange" &&
			  (input.Id == "scale_map_range" || input.Id == "scaling_map_range" ||
			   input.Id == "amplitude_map_range"))))
			return true;
		if (entry.Type == "pc.cellular" &&
			((input.SourceKind == "ValueUnit" && input.Id == "position_unit") ||
			 (input.SourceKind == "MaskAlphaOnly" && input.Id == "mask_alpha_only") ||
			 (input.SourceKind == "MapToggle" && input.Id == "scale_mapped") ||
			 (input.SourceKind == "MapRange" && input.Id == "scale_map_range")))
			return true;
		if (entry.Type == "pc.caustic" &&
			((input.SourceKind == "ValueUnit" && (input.Id == "position_unit" || input.Id == "scale_unit")) ||
			 (input.SourceKind == "MaskAlphaOnly" && input.Id == "mask_alpha_only") ||
			 (input.SourceKind == "MapToggle" &&
			  (input.Id == "intensity_mapped" || input.Id == "progress_mapped")) ||
			 (input.SourceKind == "MapRange" &&
			  (input.Id == "intensity_map_range" || input.Id == "progress_map_range"))))
			return true;
		if ((entry.Type == "pc.jpeg" || entry.Type == "pc.pixel_sort" || entry.Type == "pc.noise") &&
			input.SourceKind == "MaskAlphaOnly" && input.Id == "mask_alpha_only")
			return true;
		if (entry.Type == "pc.displace" &&
			((input.SourceKind == "ValueUnit" &&
			  (input.Id == "position_unit" || input.Id == "mid_point_unit")) ||
			 (input.SourceKind == "MaskAlphaOnly" && input.Id == "mask_alpha_only") ||
			 (input.SourceKind == "Attribute" && (input.Id == "interpolate" || input.Id == "oversample")) ||
			 (input.SourceKind == "MapToggle" &&
			  (input.Id == "strength_mapped" || input.Id == "mid_value_mapped")) ||
			 (input.SourceKind == "MapRange" &&
			  (input.Id == "strength_map_range" || input.Id == "mid_value_map_range"))))
			return true;
		if (entry.Type == "pc.tile" &&
			((input.SourceKind == "DimensionUnit" && input.Id == "dimension_unit") ||
			 (input.SourceKind == "ValueUnit" &&
			  (input.Id == "spacing_unit" || input.Id == "posiiton_unit"))))
			return true;
		if (entry.Type == "pc.polar" &&
			((input.SourceKind == "ValueUnit" && input.Id == "center_unit") ||
			 (input.SourceKind == "MaskAlphaOnly" && input.Id == "mask_alpha_only") ||
			 (input.SourceKind == "Attribute" && (input.Id == "interpolate" || input.Id == "oversample")) ||
			 (input.SourceKind == "MapToggle" &&
			  (input.Id == "angle_mapped" || input.Id == "blend_mapped" || input.Id == "twist_mapped")) ||
			 (input.SourceKind == "MapRange" &&
			  (input.Id == "angle_map_range" || input.Id == "blend_map_range" ||
			   input.Id == "twist_map_range"))))
			return true;
		if (entry.Type == "pc.kuwahara" &&
			((input.SourceKind == "Attribute" && (input.Id == "interpolate" || input.Id == "oversample")) ||
			 (input.SourceKind == "MaskAlphaOnly" && input.Id == "mask_alpha_only") ||
			 (input.SourceKind == "MapToggle" && input.Id == "radius_mapped") ||
			 (input.SourceKind == "MapRange" && input.Id == "radius_map_range")))
			return true;
		if (entry.Type == "pc.blobify" &&
			((input.SourceKind == "Attribute" && (input.Id == "interpolate" || input.Id == "oversample")) ||
			 (input.SourceKind == "MaskAlphaOnly" && input.Id == "mask_alpha_only") ||
			 (input.SourceKind == "MapToggle" && input.Id == "radius_mapped") ||
			 (input.SourceKind == "MapRange" && input.Id == "radius_map_range")))
			return true;
		if (entry.Type == "pc.xdo_g_threshold" &&
			((input.SourceKind == "ValueUnit" && input.Id == "radius_unit") ||
			 (input.SourceKind == "MaskAlphaOnly" && input.Id == "mask_alpha_only") ||
			 (input.SourceKind == "Attribute" && (input.Id == "interpolate" || input.Id == "oversample")) ||
			 (input.SourceKind == "MapToggle" &&
			  (input.Id == "gamma_mapped" || input.Id == "epsilon_mapped" ||
			   input.Id == "smoothness_mapped")) ||
			 (input.SourceKind == "MapRange" &&
			  (input.Id == "gamma_map_range" || input.Id == "epsilon_map_range" ||
			   input.Id == "smoothness_map_range"))))
			return true;
		if (entry.Type == "pc.refract" &&
			((input.SourceKind == "Attribute" && (input.Id == "interpolate" || input.Id == "oversample")) ||
			 (input.SourceKind == "MaskAlphaOnly" && input.Id == "mask_alpha_only") ||
			 (input.SourceKind == "MapToggle" &&
			  (input.Id == "height_mapped" || input.Id == "distance_mapped" || input.Id == "ior_mapped")) ||
			 (input.SourceKind == "MapRange" &&
			  (input.Id == "height_map_range" || input.Id == "distance_map_range" ||
			   input.Id == "ior_map_range"))))
			return true;
		if (entry.Type == "pc.noise_cristal" &&
			((input.SourceKind == "ValueUnit" && input.Id == "position_unit") ||
			 (input.SourceKind == "MaskAlphaOnly" && input.Id == "mask_alpha_only")))
			return true;
		if (entry.Type == "pc.noise_bubble" && input.SourceKind == "MaskAlphaOnly" &&
			input.Id == "mask_alpha_only")
			return true;
		if (entry.Type == "pc.flow_noise" &&
			((input.SourceKind == "ValueUnit" && input.Id == "position_unit") ||
			 (input.SourceKind == "MaskAlphaOnly" && input.Id == "mask_alpha_only")))
			return true;
		if (entry.Type == "pc.herringbone_tile" &&
			((input.SourceKind == "ValueUnit" && (input.Id == "position_unit" || input.Id == "scale_unit")) ||
			 (input.SourceKind == "MapToggle" &&
			  (input.Id == "angle_mapped" || input.Id == "gap_mapped" || input.Id == "scale_mapped" ||
			   input.Id == "tile_color_mapped")) ||
			 (input.SourceKind == "MapRange" &&
			  (input.Id == "angle_map_range" || input.Id == "gap_map_range" ||
			   input.Id == "scale_map_range"))))
			return true;
		if ((entry.Type == "pc.julia_set" &&
			 ((input.SourceKind == "ValueUnit" && (input.Id == "c_unit" || input.Id == "position_unit")) ||
			  (input.SourceKind == "MaskAlphaOnly" && input.Id == "mask_alpha_only"))) ||
			(entry.Type == "pc.gabor_noise" &&
			 ((input.SourceKind == "ValueUnit" && input.Id == "position_unit") ||
			  (input.SourceKind == "MaskAlphaOnly" && input.Id == "mask_alpha_only") ||
			  (input.SourceKind == "MapToggle" &&
			   (input.Id == "density_mapped" || input.Id == "sharpness_mapped" ||
				input.Id == "phase_mapped" || input.Id == "scale_mapped")) ||
			  (input.SourceKind == "MapRange" &&
			   (input.Id == "density_map_range" || input.Id == "sharpness_map_range" ||
				input.Id == "phase_map_range" || input.Id == "scale_map_range")))))
			return true;
		if (entry.Type == "pc.mirror_polar" &&
			((input.SourceKind == "ValueUnit" &&
			  (input.Id == "position_unit" || input.Id == "center_unit")) ||
			 (input.SourceKind == "MapToggle" && input.Id == "spokes_mapped") ||
			 (input.SourceKind == "MapRange" && input.Id == "spokes_map_range")))
			return true;
		if (entry.Type == "pc.dotted" &&
			((input.SourceKind == "ValueUnit" && (input.Id == "size_unit" || input.Id == "position_unit")) ||
			 (input.SourceKind == "MaskAlphaOnly" && input.Id == "mask_alpha_only")))
			return true;
		return (entry.Type == "pc.stripe" &&
				((input.SourceKind == "ValueUnit" &&
				  (input.Id == "size_unit" || input.Id == "position_unit")) ||
				 (input.SourceKind == "MaskAlphaOnly" && input.Id == "mask_alpha_only") ||
				 (input.SourceKind == "MapToggle" &&
				  (input.Id == "size_mapped" || input.Id == "angle_mapped" || input.Id == "random_mapped" ||
				   input.Id == "strip_ratio_mapped" || input.Id == "colors_mapped")) ||
				 (input.SourceKind == "MapRange" &&
				  (input.Id == "size_map_range" || input.Id == "angle_map_range" ||
				   input.Id == "random_map_range" || input.Id == "strip_ratio_map_range")))) ||
			   (entry.Type == "pc.dotted" &&
				((input.SourceKind == "MapToggle" &&
				  (input.Id == "size_mapped" || input.Id == "angle_mapped" ||
				   input.Id == "dot_size_mapped")) ||
				 (input.SourceKind == "MapRange" &&
				  (input.Id == "size_map_range" || input.Id == "angle_map_range" ||
				   input.Id == "dot_size_map_range")))) ||
			   (entry.Type == "pc.smear" &&
				((input.SourceKind == "MapToggle" &&
				  (input.Id == "strength_mapped" || input.Id == "direction_mapped")) ||
				 (input.SourceKind == "MapRange" &&
				  (input.Id == "strength_map_range" || input.Id == "direction_map_range")))) ||
			   (entry.Type == "pc.dither" &&
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
		if (context.Entry.Type == "pc.perlin_extra" &&
			(port == "scale" || port == "parameter_a" || port == "parameter_b"))
			return context.Boolean(std::string(port) + "_mapped");
		if (context.Entry.Type == "pc.pytagorean_tile" &&
			(port == "scale" || port == "rotation" || port == "gap"))
			return context.Boolean(std::string(port) + "_mapped");
		if (context.Entry.Type == "pc.shard_noise" &&
			(port == "scale" || port == "progress" || port == "sharpness"))
			return context.Boolean(std::string(port) + "_mapped");
		if (context.Entry.Type == "pc.perlin" &&
			(port == "scale" || port == "scaling" || port == "amplitude"))
			return context.Boolean(
				port == "scale"		? "scale_mapped"
				: port == "scaling" ? "scaling_mapped"
									: "amplitude_mapped"
			);
		if (context.Entry.Type == "pc.cellular" && port == "scale") return context.Boolean("scale_mapped");
		if (context.Entry.Type == "pc.caustic" && (port == "intensity" || port == "progress"))
			return context.Boolean(port == "intensity" ? "intensity_mapped" : "progress_mapped");
		if (context.Entry.Type == "pc.displace" && (port == "strength" || port == "mid_value"))
			return context.Boolean(std::string(port) + "_mapped");
		if (context.Entry.Type == "pc.polar" && (port == "angle" || port == "blend" || port == "twist"))
			return context.Boolean(std::string(port) + "_mapped");
		if (context.Entry.Type == "pc.kuwahara" && port == "radius") return context.Boolean("radius_mapped");
		if (context.Entry.Type == "pc.blobify" && port == "radius") return context.Boolean("radius_mapped");
		if (context.Entry.Type == "pc.xdo_g_threshold" &&
			(port == "gamma" || port == "epsilon" || port == "smoothness"))
			return context.Boolean(std::string(port) + "_mapped");
		if (context.Entry.Type == "pc.refract" && (port == "height" || port == "distance" || port == "ior"))
			return context.Boolean(std::string(port) + "_mapped");
		if (context.Entry.Type == "pc.herringbone_tile" &&
			(port == "scale" || port == "angle" || port == "gap"))
			return context.Boolean(std::string(port) + "_mapped");
		if (context.Entry.Type == "pc.gabor_noise" &&
			(port == "density" || port == "sharpness" || port == "phase" || port == "scale"))
			return context.Boolean(std::string(port) + "_mapped");
		if (context.Entry.Type == "pc.mirror_polar" && port == "spokes")
			return context.Boolean("spokes_mapped");
		if (context.Entry.Type == "pc.stripe" &&
			(port == "size" || port == "angle" || port == "random" || port == "strip_ratio"))
			return context.Boolean(std::string(port) + "_mapped");
		if (context.Entry.Type == "pc.dotted" && (port == "size" || port == "angle" || port == "dot_size"))
			return context.Boolean(std::string(port) + "_mapped");
		if (context.Entry.Type == "pc.smear" && (port == "strength" || port == "direction"))
			return context.Boolean(port == "strength" ? "strength_mapped" : "direction_mapped");
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
		if (context.Entry.Type == "pc.perlin_extra" &&
			(port == "scale" || port == "parameter_a" || port == "parameter_b")) {
			if (SourceRangeMapped(context, port) && !context.IsLinked(port) &&
				context.IsCatalogueDefault(port).value_or(false))
				return context.Find(std::string(port) + "_map_range");
			return value;
		}
		if (context.Entry.Type == "pc.pytagorean_tile" &&
			(port == "scale" || port == "rotation" || port == "gap")) {
			if (SourceRangeMapped(context, port) && !context.IsLinked(port) &&
				context.IsCatalogueDefault(port).value_or(false))
				return context.Find(std::string(port) + "_map_range");
			return value;
		}
		if (context.Entry.Type == "pc.shard_noise" &&
			(port == "scale" || port == "progress" || port == "sharpness")) {
			if (SourceRangeMapped(context, port) && !context.IsLinked(port) &&
				context.IsCatalogueDefault(port).value_or(false))
				return context.Find(std::string(port) + "_map_range");
			return value;
		}
		if (context.Entry.Type == "pc.perlin" &&
			(port == "scale" || port == "scaling" || port == "amplitude")) {
			if (SourceRangeMapped(context, port) && !context.IsLinked(port) &&
				context.IsCatalogueDefault(port).value_or(false))
				return context.Find(
					port == "scale"		? "scale_map_range"
					: port == "scaling" ? "scaling_map_range"
										: "amplitude_map_range"
				);
			return value;
		}
		if (context.Entry.Type == "pc.cellular" && port == "scale") {
			if (!context.IsLinked(port) && context.IsCatalogueDefault(port).value_or(false))
				return context.Find("scale_map_range");
			return value;
		}
		if (context.Entry.Type == "pc.caustic" && (port == "intensity" || port == "progress")) {
			if (!context.IsLinked(port) && context.IsCatalogueDefault(port).value_or(false))
				return context.Find(port == "intensity" ? "intensity_map_range" : "progress_map_range");
			return value;
		}
		if (context.Entry.Type == "pc.displace" && (port == "strength" || port == "mid_value")) {
			if (!context.IsLinked(port) && context.IsCatalogueDefault(port).value_or(false))
				return context.Find(std::string(port) + "_map_range");
			return value;
		}
		if (context.Entry.Type == "pc.polar" && (port == "angle" || port == "blend" || port == "twist")) {
			if (!context.IsLinked(port) && context.IsCatalogueDefault(port).value_or(false))
				return context.Find(std::string(port) + "_map_range");
			return value;
		}
		if (context.Entry.Type == "pc.kuwahara" && port == "radius") {
			if (!context.IsLinked(port) && context.IsCatalogueDefault(port).value_or(false))
				return context.Find("radius_map_range");
			return value;
		}
		if (context.Entry.Type == "pc.blobify" && port == "radius") {
			if (!context.IsLinked(port) && context.IsCatalogueDefault(port).value_or(false))
				return context.Find("radius_map_range");
			return value;
		}

		if (context.Entry.Type == "pc.xdo_g_threshold" &&
			(port == "gamma" || port == "epsilon" || port == "smoothness")) {
			if (!context.IsLinked(port) && context.IsCatalogueDefault(port).value_or(false))
				return context.Find(std::string(port) + "_map_range");
			return value;
		}
		if (context.Entry.Type == "pc.refract" && (port == "height" || port == "distance" || port == "ior")) {
			if (!context.IsLinked(port) && context.IsCatalogueDefault(port).value_or(false))
				return context.Find(std::string(port) + "_map_range");
			return value;
		}
		if (context.Entry.Type == "pc.herringbone_tile" &&
			(port == "scale" || port == "angle" || port == "gap")) {
			if (!context.IsLinked(port) && context.IsCatalogueDefault(port).value_or(false))
				return context.Find(std::string(port) + "_map_range");
			return value;
		}
		if (context.Entry.Type == "pc.gabor_noise" &&
			(port == "density" || port == "sharpness" || port == "phase" || port == "scale")) {
			if (!context.IsLinked(port) && context.IsCatalogueDefault(port).value_or(false))
				return context.Find(std::string(port) + "_map_range");
			return value;
		}
		if (context.Entry.Type == "pc.mirror_polar" && port == "spokes") {
			if (!context.IsLinked(port) && context.IsCatalogueDefault(port).value_or(false))
				return context.Find("spokes_map_range");
			return value;
		}
		if (context.Entry.Type == "pc.stripe" &&
			(port == "size" || port == "angle" || port == "random" || port == "strip_ratio")) {
			if (!context.IsLinked(port) && context.IsCatalogueDefault(port).value_or(false))
				return context.Find(std::string(port) + "_map_range");
			return value;
		}
		if (context.Entry.Type == "pc.dotted" && (port == "size" || port == "angle" || port == "dot_size")) {
			if (!context.IsLinked(port) && context.IsCatalogueDefault(port).value_or(false))
				return context.Find(
					port == "size" ? "size_map_range"
								   : (port == "angle" ? "angle_map_range" : "dot_size_map_range")
				);
			return value;
		}
		const bool simplex =
			context.Entry.Type == "pc.noise_simplex" && (port == "iteration" || port == "scale");
		const bool smearDefault = context.Entry.Type == "pc.smear" && !context.IsLinked(port) &&
								  context.IsCatalogueDefault(port).value_or(false);
		if (smearDefault)
			return context.Find(port == "strength" ? "strength_map_range" : "direction_map_range");
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
		if (context.Entry.Type == "pc.smear")
			return context.Find(port == "strength" ? "strength_map_range" : "direction_map_range");
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
		if (!value && context.Entry.Type == "pc.perlin_extra") {
			range = port == "scale" ? Vector2{4, 4} : port == "parameter_b" ? Vector2{0, 1} : Vector2{};
			return true;
		}
		if (!value && context.Entry.Type == "pc.pytagorean_tile") {
			range = port == "scale" ? Vector2{.25, .25} : port == "gap" ? Vector2{.25, .25} : Vector2{};
			return true;
		}
		if (!value && context.Entry.Type == "pc.shard_noise") {
			range = port == "scale" ? Vector2{4, 4} : port == "progress" ? Vector2{0, 0} : Vector2{0, 1};
			return true;
		}
		if (!value && context.Entry.Type == "pc.perlin") {
			range = port == "scale" ? Vector2{4, 4} : port == "scaling" ? Vector2{0, 2} : Vector2{0, .5};
			return true;
		}
		if (!value && context.Entry.Type == "pc.cellular" && port == "scale") {
			range = {0, 4};
			return true;
		}
		if (!value && context.Entry.Type == "pc.caustic") {
			range = port == "progress" ? Vector2{0, 0} : Vector2{0, 1};
			return true;
		}
		if (!value && context.Entry.Type == "pc.kuwahara" && port == "radius") {
			range = {0, 2};
			return true;
		}
		if (!value && context.Entry.Type == "pc.blobify" && port == "radius") {
			range = {0, 3};
			return true;
		}
		if (!value && context.Entry.Type == "pc.mirror_polar" && port == "spokes") {
			range = {0, 4};
			return true;
		}
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
		if (context.Entry.Type == "pc.perlin_extra" || context.Entry.Type == "pc.pytagorean_tile" ||
			context.Entry.Type == "pc.shard_noise" || context.Entry.Type == "pc.perlin" ||
			context.Entry.Type == "pc.cellular" || context.Entry.Type == "pc.caustic" ||
			context.Entry.Type == "pc.displace" || context.Entry.Type == "pc.polar" ||
			context.Entry.Type == "pc.blobify" || context.Entry.Type == "pc.kuwahara" ||
			context.Entry.Type == "pc.xdo_g_threshold" || context.Entry.Type == "pc.refract" ||
			context.Entry.Type == "pc.herringbone_tile" || context.Entry.Type == "pc.gabor_noise" ||
			context.Entry.Type == "pc.mirror_polar" || context.Entry.Type == "pc.stripe" ||
			context.Entry.Type == "pc.dotted" || (context.Entry.Type == "pc.dither" && port == "contrast") ||
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
