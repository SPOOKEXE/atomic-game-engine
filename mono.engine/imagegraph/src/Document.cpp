#include "ArrayOps.hpp"
#include "PaletteOps.hpp"
#include "PixelOps.hpp"
#include "PixelOpsBasicFilters.hpp"
#include "PixelOpsBlend.hpp"
#include "PixelOpsBlur.hpp"
#include "PixelOpsChannelAssembly.hpp"
#include "PixelOpsChecker.hpp"
#include "PixelOpsColorAdjust.hpp"
#include "PixelOpsConversion.hpp"
#include "PixelOpsCurveColor.hpp"
#include "PixelOpsGenerate.hpp"
#include "PixelOpsGradient.hpp"
#include "PixelOpsNoise.hpp"
#include "PixelOpsShapeRender.hpp"
#include "PixelOpsSpatialWarp.hpp"
#include "PixelOpsTile.hpp"
#include "PixelOpsVignette.hpp"
#include "PixelOpsWarp.hpp"
#include "Timeline.hpp"
#include "TimelineSchedule.hpp"
#include "ValueNodeEval.hpp"
#include "ValueNodeSchemas.hpp"

#include <engine/imagegraph/AudioCapture.hpp>
#include <engine/imagegraph/Document.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <iomanip>
#include <limits>
#include <locale>
#include <map>
#include <queue>
#include <set>
#include <sstream>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace engine::imagegraph {
	namespace {
		constexpr std::array<PortSchema, 3> SOLID_PORTS = {{
			{"foreground", ValueType::Image, PortDirection::Input},
			{"mask", ValueType::Image, PortDirection::Input},
			{"image", ValueType::Image, PortDirection::Output},
		}};
		constexpr std::array<PortSchema, 2> PASS_PORTS = {{
			{"image", ValueType::Image, PortDirection::Input},
			{"image", ValueType::Image, PortDirection::Output},
		}};
		constexpr std::array<PropertySchema, 6> SOLID_PROPERTIES = {{
			{"width", ValueType::Integer},
			{"height", ValueType::Integer},
			{"colour", ValueType::Colour},
			{"use_mask_dimension", ValueType::Boolean},
			{"empty", ValueType::Boolean},
			{"mask_alpha_only", ValueType::Boolean},
		}};
		const NodeSchema SOLID_SCHEMA{"image.solid", SOLID_PORTS, SOLID_PROPERTIES};
		const std::array<PortSchema, 5> CHECKER_PORTS = {{
			{"uv_map", ValueType::Image, PortDirection::Input},
			{"mask", ValueType::Image, PortDirection::Input},
			{"size_map", ValueType::Image, PortDirection::Input},
			{"angle_map", ValueType::Image, PortDirection::Input},
			{"image", ValueType::Image, PortDirection::Output},
		}};
		const std::array<PropertySchema, 11> CHECKER_PROPERTIES = {{
			{"width", ValueType::Integer},
			{"height", ValueType::Integer},
			{"size", ValueType::Scalar},
			{"aspect", ValueType::Scalar},
			{"angle", ValueType::Scalar},
			{"position", ValueType::Vector2},
			{"diagonal", ValueType::Boolean},
			{"type", ValueType::Integer},
			{"color_1", ValueType::Colour},
			{"color_2", ValueType::Colour},
			{"uv_mix", ValueType::Scalar},
		}};
		const NodeSchema CHECKER_SCHEMA{"image.checker", CHECKER_PORTS, CHECKER_PROPERTIES};
		const std::array<PortSchema, 5> MONOCHROME_PORTS = {{
			{"image", ValueType::Image, PortDirection::Input},
			{"mask", ValueType::Image, PortDirection::Input},
			{"brightness_map", ValueType::Image, PortDirection::Input},
			{"contrast_map", ValueType::Image, PortDirection::Input},
			{"image", ValueType::Image, PortDirection::Output},
		}};
		const std::array<PropertySchema, 6> MONOCHROME_PROPERTIES = {{
			{"brightness", ValueType::Scalar},
			{"contrast", ValueType::Scalar},
			{"mix", ValueType::Scalar},
			{"channel", ValueType::Integer},
			{"invert_mask", ValueType::Boolean},
			{"mask_feather", ValueType::Scalar},
		}};
		const NodeSchema GREYSCALE_SCHEMA{"image.greyscale", MONOCHROME_PORTS, MONOCHROME_PROPERTIES};
		const NodeSchema BW_SCHEMA{"image.bw", MONOCHROME_PORTS, MONOCHROME_PROPERTIES};
		const std::array<PortSchema, 2> GREY_ALPHA_PORTS = {{
			{"image", ValueType::Image, PortDirection::Input},
			{"image", ValueType::Image, PortDirection::Output},
		}};
		const std::array<PropertySchema, 4> GREY_ALPHA_PROPERTIES = {{
			{"curve", ValueType::Curve},
			{"invert", ValueType::Boolean},
			{"replace_color", ValueType::Boolean},
			{"color", ValueType::Colour},
		}};
		const NodeSchema GREY_ALPHA_SCHEMA{"image.grey_alpha", GREY_ALPHA_PORTS, GREY_ALPHA_PROPERTIES};
		const std::array<PortSchema, 5> RGB_EXTRACT_PORTS = {{
			{"image", ValueType::Image, PortDirection::Input},
			{"red", ValueType::Image, PortDirection::Output},
			{"green", ValueType::Image, PortDirection::Output},
			{"blue", ValueType::Image, PortDirection::Output},
			{"alpha", ValueType::Image, PortDirection::Output},
		}};
		const std::array<PropertySchema, 3> RGB_EXTRACT_PROPERTIES = {{
			{"output_type", ValueType::Integer},
			{"keep_alpha", ValueType::Boolean},
			{"output_array", ValueType::Boolean},
		}};
		const NodeSchema RGB_EXTRACT_SCHEMA{"image.rgb_extract", RGB_EXTRACT_PORTS, RGB_EXTRACT_PROPERTIES};
		const std::array<PortSchema, 5> HSV_EXTRACT_PORTS = {{
			{"image", ValueType::Image, PortDirection::Input},
			{"hue", ValueType::Image, PortDirection::Output},
			{"saturation", ValueType::Image, PortDirection::Output},
			{"value", ValueType::Image, PortDirection::Output},
			{"alpha", ValueType::Image, PortDirection::Output},
		}};
		const std::array<PropertySchema, 2> HSV_EXTRACT_PROPERTIES = {{
			{"color_space", ValueType::Integer},
			{"output_array", ValueType::Boolean},
		}};
		const NodeSchema HSV_EXTRACT_SCHEMA{"image.hsv_extract", HSV_EXTRACT_PORTS, HSV_EXTRACT_PROPERTIES};
		const std::array<PortSchema, 7> RGB_COMBINE_PORTS = {{
			{"red", ValueType::Image, PortDirection::Input},
			{"green", ValueType::Image, PortDirection::Input},
			{"blue", ValueType::Image, PortDirection::Input},
			{"alpha", ValueType::Image, PortDirection::Input},
			{"rgba_array", ValueType::Array, PortDirection::Input},
			{"base_map", ValueType::Image, PortDirection::Input},
			{"image", ValueType::Image, PortDirection::Output},
		}};
		const std::array<PropertySchema, 3> RGB_COMBINE_PROPERTIES = {{
			{"sampling_type", ValueType::Integer},
			{"base_value", ValueType::Scalar},
			{"array_input", ValueType::Boolean},
		}};
		const NodeSchema RGB_COMBINE_SCHEMA{"image.combine_rgb", RGB_COMBINE_PORTS, RGB_COMBINE_PROPERTIES};
		const std::array<PortSchema, 6> HSV_COMBINE_PORTS = {{
			{"hue", ValueType::Image, PortDirection::Input},
			{"saturation", ValueType::Image, PortDirection::Input},
			{"value", ValueType::Image, PortDirection::Input},
			{"alpha", ValueType::Image, PortDirection::Input},
			{"hsv_array", ValueType::Array, PortDirection::Input},
			{"image", ValueType::Image, PortDirection::Output},
		}};
		const std::array<PropertySchema, 2> HSV_COMBINE_PROPERTIES = {{
			{"color_space", ValueType::Integer},
			{"array_input", ValueType::Boolean},
		}};
		const NodeSchema HSV_COMBINE_SCHEMA{"image.combine_hsv", HSV_COMBINE_PORTS, HSV_COMBINE_PROPERTIES};
		const std::array<PortSchema, 6> OVERRIDE_CHANNEL_PORTS = {{
			{"image", ValueType::Image, PortDirection::Input},
			{"red", ValueType::Image, PortDirection::Input},
			{"green", ValueType::Image, PortDirection::Input},
			{"blue", ValueType::Image, PortDirection::Input},
			{"alpha", ValueType::Image, PortDirection::Input},
			{"image", ValueType::Image, PortDirection::Output},
		}};
		const std::array<PropertySchema, 1> OVERRIDE_CHANNEL_PROPERTIES = {{
			{"sampling_type", ValueType::Integer},
		}};
		const NodeSchema OVERRIDE_CHANNEL_SCHEMA{
			"image.override_channel", OVERRIDE_CHANNEL_PORTS, OVERRIDE_CHANNEL_PROPERTIES
		};
		const std::array<PortSchema, 3> MULTIPLY_ALPHA_PORTS = {{
			{"image", ValueType::Image, PortDirection::Input},
			{"mask", ValueType::Image, PortDirection::Input},
			{"image", ValueType::Image, PortDirection::Output},
		}};
		const std::array<PropertySchema, 8> MULTIPLY_ALPHA_PROPERTIES = {{
			{"threshold", ValueType::Scalar},
			{"bg_color", ValueType::Colour},
			{"mix", ValueType::Scalar},
			{"channel", ValueType::Integer},
			{"invert_mask", ValueType::Boolean},
			{"mask_feather", ValueType::Scalar},
			{"active", ValueType::Boolean},
			{"oversample", ValueType::Integer},
		}};
		const NodeSchema MULTIPLY_ALPHA_SCHEMA{
			"image.multiply_alpha", MULTIPLY_ALPHA_PORTS, MULTIPLY_ALPHA_PROPERTIES
		};
		const std::array<PortSchema, 2> GAMMA_MAP_PORTS = {{
			{"image", ValueType::Image, PortDirection::Input},
			{"image", ValueType::Image, PortDirection::Output},
		}};
		const std::array<PropertySchema, 2> GAMMA_MAP_PROPERTIES = {{
			{"invert", ValueType::Boolean},
			{"active", ValueType::Boolean},
		}};
		const NodeSchema GAMMA_MAP_SCHEMA{"image.gamma_map", GAMMA_MAP_PORTS, GAMMA_MAP_PROPERTIES};
		const std::array<PortSchema, 4> MIRROR_PORTS = {{
			{"image", ValueType::Image, PortDirection::Input},
			{"mask", ValueType::Image, PortDirection::Input},
			{"image", ValueType::Image, PortDirection::Output},
			{"mirror_mask", ValueType::Image, PortDirection::Output},
		}};
		const std::array<PropertySchema, 9> MIRROR_PROPERTIES = {{
			{"position", ValueType::Vector2},
			{"angle", ValueType::Scalar},
			{"flip", ValueType::Boolean},
			{"both_side", ValueType::Boolean},
			{"mix", ValueType::Scalar},
			{"channel", ValueType::Integer},
			{"invert_mask", ValueType::Boolean},
			{"mask_feather", ValueType::Scalar},
			{"active", ValueType::Boolean},
		}};
		const NodeSchema MIRROR_SCHEMA{"image.mirror", MIRROR_PORTS, MIRROR_PROPERTIES};
		const std::array<PortSchema, 4> BARREL_PORTS = {{
			{"image", ValueType::Image, PortDirection::Input},
			{"mask", ValueType::Image, PortDirection::Input},
			{"intensity_map", ValueType::Image, PortDirection::Input},
			{"image", ValueType::Image, PortDirection::Output},
		}};
		const std::array<PropertySchema, 11> BARREL_PROPERTIES = {{
			{"center", ValueType::Vector2},
			{"intensity", ValueType::Scalar},
			{"scale", ValueType::Vector2},
			{"distance_method", ValueType::Integer},
			{"mix", ValueType::Scalar},
			{"channel", ValueType::Integer},
			{"invert_mask", ValueType::Boolean},
			{"mask_feather", ValueType::Scalar},
			{"active", ValueType::Boolean},
			{"oversample", ValueType::Integer},
			{"interpolation", ValueType::Integer},
		}};
		const NodeSchema BARREL_SCHEMA{"image.barrel_distort", BARREL_PORTS, BARREL_PROPERTIES};
		const std::array<PortSchema, 8> CHROMATIC_PORTS = {{
			{"image", ValueType::Image, PortDirection::Input},
			{"uv_map", ValueType::Image, PortDirection::Input},
			{"mask", ValueType::Image, PortDirection::Input},
			{"strength_map", ValueType::Image, PortDirection::Input},
			{"intensity_map", ValueType::Image, PortDirection::Input},
			{"shift_map", ValueType::Image, PortDirection::Input},
			{"scale_map", ValueType::Image, PortDirection::Input},
			{"image", ValueType::Image, PortDirection::Output},
		}};
		const std::array<PropertySchema, 16> CHROMATIC_PROPERTIES = {{
			{"type", ValueType::Integer},
			{"center", ValueType::Vector2},
			{"strength", ValueType::Scalar},
			{"intensity", ValueType::Scalar},
			{"iterations", ValueType::Integer},
			{"resolution", ValueType::Integer},
			{"shift", ValueType::Scalar},
			{"scale", ValueType::Scalar},
			{"uv_mix", ValueType::Scalar},
			{"mix", ValueType::Scalar},
			{"invert_mask", ValueType::Boolean},
			{"mask_feather", ValueType::Scalar},
			{"active", ValueType::Boolean},
			{"oversample", ValueType::Integer},
			{"interpolation", ValueType::Integer},
			{"strength_curve", ValueType::Curve},
		}};
		const NodeSchema CHROMATIC_SCHEMA{
			"image.chromatic_aberration", CHROMATIC_PORTS, CHROMATIC_PROPERTIES
		};
		const std::array<PortSchema, 5> SPHERIZE_PORTS = {{
			{"image", ValueType::Image, PortDirection::Input},
			{"mask", ValueType::Image, PortDirection::Input},
			{"strength_map", ValueType::Image, PortDirection::Input},
			{"radius_map", ValueType::Image, PortDirection::Input},
			{"image", ValueType::Image, PortDirection::Output},
		}};
		const std::array<PropertySchema, 16> SPHERIZE_PROPERTIES = {{
			{"center", ValueType::Vector2},
			{"position", ValueType::Vector2},
			{"rotation", ValueType::Scalar},
			{"strength", ValueType::Scalar},
			{"radius", ValueType::Scalar},
			{"normalize", ValueType::Boolean},
			{"trim", ValueType::Scalar},
			{"texture_offset", ValueType::Vector2},
			{"texture_scale", ValueType::Vector2},
			{"mix", ValueType::Scalar},
			{"channel", ValueType::Integer},
			{"invert_mask", ValueType::Boolean},
			{"mask_feather", ValueType::Scalar},
			{"active", ValueType::Boolean},
			{"oversample", ValueType::Integer},
			{"interpolation", ValueType::Integer},
		}};
		const NodeSchema SPHERIZE_SCHEMA{"image.spherize", SPHERIZE_PORTS, SPHERIZE_PROPERTIES};
		const std::array<PortSchema, 6> DILATE_PORTS = {{
			{"image", ValueType::Image, PortDirection::Input},
			{"uv_map", ValueType::Image, PortDirection::Input},
			{"mask", ValueType::Image, PortDirection::Input},
			{"strength_map", ValueType::Image, PortDirection::Input},
			{"radius_map", ValueType::Image, PortDirection::Input},
			{"image", ValueType::Image, PortDirection::Output},
		}};
		const std::array<PropertySchema, 12> DILATE_PROPERTIES = {{
			{"center", ValueType::Vector2},
			{"strength", ValueType::Scalar},
			{"radius", ValueType::Scalar},
			{"uv_mix", ValueType::Scalar},
			{"mix", ValueType::Scalar},
			{"channel", ValueType::Integer},
			{"invert_mask", ValueType::Boolean},
			{"mask_feather", ValueType::Scalar},
			{"active", ValueType::Boolean},
			{"oversample", ValueType::Integer},
			{"interpolation", ValueType::Integer},
			{"strength_curve", ValueType::Curve},
		}};
		const NodeSchema DILATE_SCHEMA{"image.dilate", DILATE_PORTS, DILATE_PROPERTIES};
		constexpr std::array<PortSchema, 8> GRADIENT_PORTS = {{
			{"uv_map", ValueType::Image, PortDirection::Input},
			{"angle_map", ValueType::Image, PortDirection::Input},
			{"radius_map", ValueType::Image, PortDirection::Input},
			{"shift_map", ValueType::Image, PortDirection::Input},
			{"scale_map", ValueType::Image, PortDirection::Input},
			{"angle_value", ValueType::Scalar, PortDirection::Input},
			{"mask", ValueType::Image, PortDirection::Input},
			{"image", ValueType::Image, PortDirection::Output},
		}};
		constexpr std::array<PropertySchema, 23> GRADIENT_PROPERTIES = {{
			{"width", ValueType::Integer},		   {"height", ValueType::Integer},
			{"gradient", ValueType::Gradient},	   {"type", ValueType::Integer},
			{"angle", ValueType::Scalar},		   {"angle_max", ValueType::Scalar},
			{"radius", ValueType::Scalar},		   {"radius_max", ValueType::Scalar},
			{"center", ValueType::Vector2},		   {"shape", ValueType::Vector2},
			{"uniform_ratio", ValueType::Boolean}, {"shift", ValueType::Scalar},
			{"shift_max", ValueType::Scalar},	   {"scale", ValueType::Scalar},
			{"scale_max", ValueType::Scalar},	   {"loop", ValueType::Integer},
			{"uv_mix", ValueType::Scalar},		   {"progress_remap", ValueType::Curve},
			{"inverse_axis", ValueType::Scalar},   {"inverse_curve", ValueType::Curve},
			{"level_in", ValueType::Vector2},	   {"level_out", ValueType::Vector2},
			{"curve", ValueType::Curve},
		}};
		const NodeSchema GRADIENT_SCHEMA{"image.gradient", GRADIENT_PORTS, GRADIENT_PROPERTIES};
		constexpr std::array<PortSchema, 3> SIMPLEX_PORTS = {{
			{"uv_map", ValueType::Image, PortDirection::Input},
			{"mask", ValueType::Image, PortDirection::Input},
			{"image", ValueType::Image, PortDirection::Output},
		}};
		constexpr std::array<PropertySchema, 18> SIMPLEX_PROPERTIES = {{
			{"width", ValueType::Integer},
			{"height", ValueType::Integer},
			{"seed", ValueType::Scalar},
			{"iterations", ValueType::Integer},
			{"tile", ValueType::Boolean},
			{"position", ValueType::Vector2},
			{"rotation", ValueType::Scalar},
			{"scale", ValueType::Vector2},
			{"iteration_scaling", ValueType::Scalar},
			{"iteration_amplitude", ValueType::Scalar},
			{"level_in", ValueType::Vector2},
			{"level_out", ValueType::Vector2},
			{"color_mode", ValueType::Integer},
			{"color_range_r", ValueType::Vector2},
			{"color_range_g", ValueType::Vector2},
			{"color_range_b", ValueType::Vector2},
			{"uv_mix", ValueType::Scalar},
			{"mapped", ValueType::Boolean},
		}};
		const NodeSchema SIMPLEX_SCHEMA{"image.noise_simplex", SIMPLEX_PORTS, SIMPLEX_PROPERTIES};
		constexpr std::array<PortSchema, 3> TILE_PORTS = {{
			{"image", ValueType::Image, PortDirection::Input},
			{"uv_map", ValueType::Image, PortDirection::Input},
			{"image", ValueType::Image, PortDirection::Output},
		}};
		constexpr std::array<PropertySchema, 12> TILE_PROPERTIES = {{
			{"scaling_type", ValueType::Integer},
			{"width", ValueType::Integer},
			{"height", ValueType::Integer},
			{"amount", ValueType::Vector2},
			{"spacing", ValueType::Vector2},
			{"position", ValueType::Vector2},
			{"rotation", ValueType::Scalar},
			{"scale", ValueType::Vector2},
			{"shift_axis", ValueType::Integer},
			{"shift", ValueType::Scalar},
			{"pattern", ValueType::Integer},
			{"uv_mix", ValueType::Scalar},
		}};
		const NodeSchema TILE_SCHEMA{"image.tile", TILE_PORTS, TILE_PROPERTIES};
		constexpr std::array<PortSchema, 7> SHAPE_PORTS = {
			{{"uv_map", ValueType::Image, PortDirection::Input},
			 {"mask", ValueType::Image, PortDirection::Input},
			 {"bg_surface", ValueType::Image, PortDirection::Input},
			 {"colored", ValueType::Image, PortDirection::Output},
			 {"mask", ValueType::Image, PortDirection::Output},
			 {"height", ValueType::Image, PortDirection::Output},
			 {"uv", ValueType::Image, PortDirection::Output}}
		};
		constexpr std::array<PropertySchema, 27> SHAPE_PROPERTIES = {
			{{"width", ValueType::Integer},
			 {"height", ValueType::Integer},
			 {"shape", ValueType::Text},
			 {"position_mode", ValueType::Integer},
			 {"center", ValueType::Vector2},
			 {"half_size", ValueType::Vector2},
			 {"shape_rotation", ValueType::Scalar},
			 {"shape_scale", ValueType::Scalar},
			 {"point1", ValueType::Vector2},
			 {"point2", ValueType::Vector2},
			 {"point3", ValueType::Vector2},
			 {"color", ValueType::Colour},
			 {"background", ValueType::Integer},
			 {"bg_color", ValueType::Colour},
			 {"bg_blend", ValueType::Integer},
			 {"antialias", ValueType::Boolean},
			 {"height_render", ValueType::Boolean},
			 {"opacity", ValueType::Boolean},
			 {"multiply_alpha", ValueType::Boolean},
			 {"mask_alpha_only", ValueType::Boolean},
			 {"level", ValueType::Vector2},
			 {"curve", ValueType::Curve},
			 {"uv_mix", ValueType::Scalar},
			 {"twist", ValueType::Scalar},
			 {"shear", ValueType::Vector2},
			 {"corner", ValueType::Scalar},
			 {"invert_mask", ValueType::Boolean}}
		};
		const NodeSchema SHAPE_SCHEMA{"image.shape", SHAPE_PORTS, SHAPE_PROPERTIES};
		constexpr std::array<PortSchema, 4> COLOR_ADJUST_PORTS = {
			{{"image", ValueType::Image, PortDirection::Input},
			 {"mask", ValueType::Image, PortDirection::Input},
			 {"palette", ValueType::Image, PortDirection::Input},
			 {"image", ValueType::Image, PortDirection::Output}}
		};
		constexpr std::array<PropertySchema, 16> COLOR_ADJUST_PROPERTIES = {
			{{"input_type", ValueType::Integer},
			 {"channel", ValueType::Integer},
			 {"alpha", ValueType::Scalar},
			 {"brightness", ValueType::Scalar},
			 {"contrast", ValueType::Scalar},
			 {"exposure", ValueType::Scalar},
			 {"hue", ValueType::Scalar},
			 {"saturation", ValueType::Scalar},
			 {"value", ValueType::Scalar},
			 {"blend", ValueType::Colour},
			 {"blend_mode", ValueType::Integer},
			 {"blend_amount", ValueType::Scalar},
			 {"invert_mask", ValueType::Boolean},
			 {"mask_feather", ValueType::Scalar},
			 {"mapped", ValueType::Boolean},
			 {"mix", ValueType::Scalar}}
		};
		const NodeSchema COLOR_ADJUST_SCHEMA{
			"image.color_adjust", COLOR_ADJUST_PORTS, COLOR_ADJUST_PROPERTIES
		};
		constexpr std::array<PortSchema, 4> BLUR_PORTS = {
			{{"image", ValueType::Image, PortDirection::Input},
			 {"mask", ValueType::Image, PortDirection::Input},
			 {"uv_map", ValueType::Image, PortDirection::Input},
			 {"image", ValueType::Image, PortDirection::Output}}
		};
		constexpr std::array<PropertySchema, 12> BLUR_PROPERTIES = {
			{{"size", ValueType::Scalar},
			 {"intensity", ValueType::Integer},
			 {"override_color", ValueType::Boolean},
			 {"color", ValueType::Colour},
			 {"gamma", ValueType::Boolean},
			 {"aspect", ValueType::Scalar},
			 {"direction", ValueType::Scalar},
			 {"channel", ValueType::Integer},
			 {"mix", ValueType::Scalar},
			 {"invert_mask", ValueType::Boolean},
			 {"mask_feather", ValueType::Scalar},
			 {"uv_mix", ValueType::Scalar}}
		};
		const NodeSchema BLUR_SCHEMA{"image.blur", BLUR_PORTS, BLUR_PROPERTIES};
		constexpr std::array<PortSchema, 6> VIGNETTE_PORTS = {{
			{"image", ValueType::Image, PortDirection::Input},
			{"mask", ValueType::Image, PortDirection::Input},
			{"roundness_map", ValueType::Image, PortDirection::Input},
			{"exposure_map", ValueType::Image, PortDirection::Input},
			{"strength_map", ValueType::Image, PortDirection::Input},
			{"image", ValueType::Image, PortDirection::Output},
		}};
		constexpr std::array<PropertySchema, 11> VIGNETTE_PROPERTIES = {{
			{"center", ValueType::Vector2},
			{"roundness", ValueType::Scalar},
			{"exposure", ValueType::Scalar},
			{"strength", ValueType::Scalar},
			{"exponent", ValueType::Scalar},
			{"lighten", ValueType::Scalar},
			{"color", ValueType::Colour},
			{"mix", ValueType::Scalar},
			{"invert_mask", ValueType::Boolean},
			{"mask_feather", ValueType::Scalar},
			{"strength_curve", ValueType::Curve},
		}};
		const NodeSchema VIGNETTE_SCHEMA{"image.vignette", VIGNETTE_PORTS, VIGNETTE_PROPERTIES};
		const std::array<PortSchema, 8> DISPLACE_PORTS = {{
			{"image", ValueType::Image, PortDirection::Input},
			{"displace_map", ValueType::Image, PortDirection::Input},
			{"displace_map_2", ValueType::Image, PortDirection::Input},
			{"uv_map", ValueType::Image, PortDirection::Input},
			{"mask", ValueType::Image, PortDirection::Input},
			{"strength_map", ValueType::Image, PortDirection::Input},
			{"mid_value_map", ValueType::Image, PortDirection::Input},
			{"image", ValueType::Image, PortDirection::Output},
		}};
		const std::array<PropertySchema, 24> DISPLACE_PROPERTIES = {{
			{"mode", ValueType::Integer},		 {"position", ValueType::Vector2},
			{"strength", ValueType::Scalar},	 {"mid_value", ValueType::Scalar},
			{"angle_offset", ValueType::Scalar}, {"interpolation", ValueType::Integer},
			{"oversample", ValueType::Integer},	 {"oversample_mode", ValueType::Integer},
			{"uv_mix", ValueType::Scalar},		 {"mix", ValueType::Scalar},
			{"invert_mask", ValueType::Boolean}, {"mask_feather", ValueType::Scalar},
			{"channel", ValueType::Integer},	 {"separate_axis", ValueType::Boolean},
			{"mid_point", ValueType::Vector2},	 {"iterate", ValueType::Boolean},
			{"blend_mode", ValueType::Integer},	 {"iteration", ValueType::Integer},
			{"mix_ratio", ValueType::Scalar},	 {"fade_distance", ValueType::Boolean},
			{"reposition", ValueType::Boolean},	 {"repeat", ValueType::Integer},
			{"stop_empty", ValueType::Boolean},	 {"strength_curve", ValueType::Curve},
		}};
		const NodeSchema DISPLACE_SCHEMA{"image.displace", DISPLACE_PORTS, DISPLACE_PROPERTIES};
		const std::array<PortSchema, 6> POLAR_PORTS = {{
			{"image", ValueType::Image, PortDirection::Input},
			{"mask", ValueType::Image, PortDirection::Input},
			{"angle_map", ValueType::Image, PortDirection::Input},
			{"blend_map", ValueType::Image, PortDirection::Input},
			{"twist_map", ValueType::Image, PortDirection::Input},
			{"image", ValueType::Image, PortDirection::Output},
		}};
		const std::array<PropertySchema, 14> POLAR_PROPERTIES = {{
			{"tile", ValueType::Vector2},
			{"center", ValueType::Vector2},
			{"angle", ValueType::Scalar},
			{"invert", ValueType::Boolean},
			{"swap_axis", ValueType::Boolean},
			{"blend", ValueType::Scalar},
			{"radius_mode", ValueType::Integer},
			{"range", ValueType::Vector2},
			{"twist", ValueType::Scalar},
			{"interpolation", ValueType::Integer},
			{"channel", ValueType::Integer},
			{"mix", ValueType::Scalar},
			{"invert_mask", ValueType::Boolean},
			{"mask_feather", ValueType::Scalar},
		}};
		const NodeSchema POLAR_SCHEMA{"image.polar", POLAR_PORTS, POLAR_PROPERTIES};
		const std::array<PortSchema, 3> CURVE_COLOR_PORTS = {{
			{"image", ValueType::Image, PortDirection::Input},
			{"mask", ValueType::Image, PortDirection::Input},
			{"image", ValueType::Image, PortDirection::Output},
		}};
		const std::array<PropertySchema, 9> CURVE_COLOR_PROPERTIES = {{
			{"brightness", ValueType::Curve},
			{"red", ValueType::Curve},
			{"green", ValueType::Curve},
			{"blue", ValueType::Curve},
			{"alpha", ValueType::Curve},
			{"channel", ValueType::Integer},
			{"mix", ValueType::Scalar},
			{"invert_mask", ValueType::Boolean},
			{"mask_feather", ValueType::Scalar},
		}};
		const NodeSchema CURVE_COLOR_SCHEMA{"image.curve", CURVE_COLOR_PORTS, CURVE_COLOR_PROPERTIES};
		const std::array<PortSchema, 5> COLORIZE_PORTS = {{
			{"image", ValueType::Image, PortDirection::Input},
			{"mask", ValueType::Image, PortDirection::Input},
			{"gradient_map", ValueType::Image, PortDirection::Input},
			{"shift_map", ValueType::Image, PortDirection::Input},
			{"image", ValueType::Image, PortDirection::Output},
		}};
		const std::array<PropertySchema, 10> COLORIZE_PROPERTIES = {{
			{"gradient", ValueType::Gradient},
			{"shift", ValueType::Scalar},
			{"color_range", ValueType::Vector2},
			{"overflow", ValueType::Integer},
			{"multiply_alpha", ValueType::Boolean},
			{"keep_alpha", ValueType::Boolean},
			{"channel", ValueType::Integer},
			{"mix", ValueType::Scalar},
			{"invert_mask", ValueType::Boolean},
			{"mask_feather", ValueType::Scalar},
		}};
		const NodeSchema COLORIZE_SCHEMA{"image.colorize", COLORIZE_PORTS, COLORIZE_PROPERTIES};
		constexpr std::array<PortSchema, 3> HEIGHT_BLEND_PORTS = {{
			{"background", ValueType::Image, PortDirection::Input},
			{"foreground", ValueType::Image, PortDirection::Input},
			{"image", ValueType::Image, PortDirection::Output},
		}};
		constexpr std::array<PropertySchema, 4> HEIGHT_BLEND_PROPERTIES = {{
			{"mode", ValueType::Integer},
			{"type", ValueType::Integer},
			{"factor", ValueType::Scalar},
			{"array_process", ValueType::Integer},
		}};
		const NodeSchema HEIGHT_BLEND_SCHEMA{
			"image.height_blend", HEIGHT_BLEND_PORTS, HEIGHT_BLEND_PROPERTIES
		};
		constexpr std::array<PortSchema, 4> BLEND_PORTS = {{
			{"background", ValueType::Image, PortDirection::Input},
			{"foreground", ValueType::Image, PortDirection::Input},
			{"mask", ValueType::Image, PortDirection::Input},
			{"image", ValueType::Image, PortDirection::Output},
		}};
		constexpr std::array<PropertySchema, 13> BLEND_PROPERTIES = {{
			{"swap", ValueType::Boolean},
			{"invert_mask", ValueType::Boolean},
			{"mask_feather", ValueType::Scalar},
			{"output_dimension", ValueType::Integer},
			{"constant_dimension", ValueType::Vector2},
			{"blend_mode", ValueType::Integer},
			{"opacity", ValueType::Scalar},
			{"preserve_alpha", ValueType::Boolean},
			{"fill_mode", ValueType::Integer},
			{"position", ValueType::Vector2},
			{"horizontal_align", ValueType::Integer},
			{"vertical_align", ValueType::Integer},
			{"mask_alpha_only", ValueType::Boolean},
		}};
		const NodeSchema BLEND_SCHEMA{"image.blend", BLEND_PORTS, BLEND_PROPERTIES};
		const NodeSchema PASS_SCHEMA{"image.passthrough", PASS_PORTS, {}};
		constexpr std::array<PortSchema, 3> MASKED_FILTER_PORTS = {{
			{"image", ValueType::Image, PortDirection::Input},
			{"mask", ValueType::Image, PortDirection::Input},
			{"image", ValueType::Image, PortDirection::Output},
		}};
		constexpr std::array<PropertySchema, 5> FLIP_PROPERTIES = {{
			{"axis", ValueType::Integer},
			{"channel", ValueType::Integer},
			{"mix", ValueType::Scalar},
			{"invert_mask", ValueType::Boolean},
			{"mask_feather", ValueType::Scalar},
		}};
		constexpr std::array<PropertySchema, 5> INVERT_PROPERTIES = {{
			{"include_alpha", ValueType::Boolean},
			{"channel", ValueType::Integer},
			{"mix", ValueType::Scalar},
			{"invert_mask", ValueType::Boolean},
			{"mask_feather", ValueType::Scalar},
		}};
		constexpr std::array<PropertySchema, 4> CUTOFF_PROPERTIES = {{
			{"minimum", ValueType::Scalar},
			{"mix", ValueType::Scalar},
			{"invert_mask", ValueType::Boolean},
			{"mask_feather", ValueType::Scalar},
		}};
		const NodeSchema FLIP_SCHEMA{"image.flip", MASKED_FILTER_PORTS, FLIP_PROPERTIES};
		const NodeSchema INVERT_SCHEMA{"image.invert", MASKED_FILTER_PORTS, INVERT_PROPERTIES};
		const NodeSchema CUTOFF_SCHEMA{"image.alpha_cutoff", MASKED_FILTER_PORTS, CUTOFF_PROPERTIES};
		constexpr std::array<PropertySchema, 6> OFFSET_PROPERTIES = {{
			{"x_offset", ValueType::Scalar},
			{"y_offset", ValueType::Scalar},
			{"angle", ValueType::Scalar},
			{"mix", ValueType::Scalar},
			{"invert_mask", ValueType::Boolean},
			{"mask_feather", ValueType::Scalar},
		}};
		const NodeSchema OFFSET_SCHEMA{"image.offset", MASKED_FILTER_PORTS, OFFSET_PROPERTIES};
		constexpr std::array<PropertySchema, 16> THRESHOLD_PROPERTIES = {{
			{"channel", ValueType::Integer},
			{"mix", ValueType::Scalar},
			{"invert_mask", ValueType::Boolean},
			{"mask_feather", ValueType::Scalar},
			{"brightness", ValueType::Boolean},
			{"algorithm", ValueType::Integer},
			{"brightness_threshold", ValueType::Scalar},
			{"brightness_smoothness", ValueType::Scalar},
			{"adaptive_radius", ValueType::Integer},
			{"brightness_invert", ValueType::Boolean},
			{"brightness_multiply", ValueType::Boolean},
			{"apply_to_alpha", ValueType::Integer},
			{"alpha", ValueType::Boolean},
			{"alpha_threshold", ValueType::Scalar},
			{"alpha_smoothness", ValueType::Scalar},
			{"alpha_invert", ValueType::Boolean},
		}};
		const NodeSchema THRESHOLD_SCHEMA{"image.threshold", MASKED_FILTER_PORTS, THRESHOLD_PROPERTIES};
		constexpr std::array<PropertySchema, 11> POSTERIZE_PROPERTIES = {{
			{"mix", ValueType::Scalar},
			{"invert_mask", ValueType::Boolean},
			{"mask_feather", ValueType::Scalar},
			{"use_palette", ValueType::Boolean},
			{"palette", ValueType::Array},
			{"use_global_range", ValueType::Boolean},
			{"steps", ValueType::Integer},
			{"gamma", ValueType::Scalar},
			{"posterize_alpha", ValueType::Boolean},
			{"hue_bias", ValueType::Scalar},
			{"mask_alpha_only", ValueType::Boolean},
		}};
		const NodeSchema POSTERIZE_SCHEMA{"image.posterize", MASKED_FILTER_PORTS, POSTERIZE_PROPERTIES};
		constexpr std::array<PortSchema, 1> ARRAY_PORTS = {
			{{"array", ValueType::Array, PortDirection::Output}}
		};
		constexpr std::array<PropertySchema, 1> ARRAY_PROPERTIES = {{{"spread", ValueType::Boolean}}};
		const NodeSchema ARRAY_SCHEMA{"value.array", ARRAY_PORTS, ARRAY_PROPERTIES, true};
		constexpr std::array<PortSchema, 2> ARRAY_GET_PORTS = {{
			{"array", ValueType::Array, PortDirection::Input},
			{"image", ValueType::Image, PortDirection::Output},
		}};
		constexpr std::array<PropertySchema, 2> ARRAY_GET_PROPERTIES = {{
			{"index", ValueType::Integer},
			{"overflow", ValueType::Integer},
		}};
		const NodeSchema ARRAY_GET_SCHEMA{"value.array_get", ARRAY_GET_PORTS, ARRAY_GET_PROPERTIES};
		// The schema is durable authored data only. Render owns fixed-plane GPU work.
		constexpr std::array<PortSchema, 5> TRANSFORM_IMAGE_3D_PORTS = {{
			{"surface", ValueType::Image, PortDirection::Input},
			{"back_surface", ValueType::Image, PortDirection::Input},
			{"mesh", ValueType::Mesh, PortDirection::Output},
			{"rendered", ValueType::Image, PortDirection::Output},
			{"depth", ValueType::Image, PortDirection::Output},
		}};
		constexpr std::array<PropertySchema, 9> TRANSFORM_IMAGE_3D_PROPERTIES = {{
			{"position", ValueType::Vector3},
			{"anchor", ValueType::Vector3},
			{"rotation", ValueType::Quaternion},
			{"scale", ValueType::Vector3},
			{"texture_tiling", ValueType::Vector2},
			{"projection", ValueType::Enum},
			{"fov", ValueType::Scalar},
			{"view_range", ValueType::Vector2},
			{"depth_range", ValueType::Vector2},
		}};
		const NodeSchema TRANSFORM_IMAGE_3D_SCHEMA{
			"image.transform_3d", TRANSFORM_IMAGE_3D_PORTS, TRANSFORM_IMAGE_3D_PROPERTIES
		};

		std::string_view TypeName(ValueType type) {
			switch (type) {
			case ValueType::Boolean:
				return "boolean";
			case ValueType::Integer:
				return "integer";
			case ValueType::Scalar:
				return "scalar";
			case ValueType::Text:
				return "text";
			case ValueType::Colour:
				return "colour";
			case ValueType::Vector2:
				return "vector2";
			case ValueType::Image:
				return "image";
			case ValueType::Array:
				return "array";
			case ValueType::Gradient:
				return "gradient";
			case ValueType::Area:
				return "area";
			case ValueType::Curve:
				return "curve";
			case ValueType::Vector4:
				return "vector4";
			case ValueType::Path2D:
				return "path2d";
			case ValueType::Vector3:
				return "vector3";
			case ValueType::Quaternion:
				return "quaternion";
			case ValueType::Enum:
				return "enum";
			case ValueType::Mesh:
				return "mesh";
			case ValueType::AudioBit:
				return "audiobit";
			}
			return {};
		}

		std::optional<ValueType> ParseType(std::string_view name) {
			for (ValueType type :
				 {ValueType::Boolean,
				  ValueType::Integer,
				  ValueType::Scalar,
				  ValueType::Text,
				  ValueType::Colour,
				  ValueType::Vector2,
				  ValueType::Image,
				  ValueType::Array,
				  ValueType::Gradient,
				  ValueType::Area,
				  ValueType::Curve,
				  ValueType::Vector4,
				  ValueType::Path2D,
				  ValueType::Vector3,
				  ValueType::Quaternion,
				  ValueType::Enum,
				  ValueType::Mesh,
				  ValueType::AudioBit}) {
				if (TypeName(type) == name) return type;
			}
			return std::nullopt;
		}

		std::string_view DirectionName(PortDirection direction) {
			return direction == PortDirection::Input ? "input" : "output";
		}

		void SetDiagnostic(
			Diagnostic &diagnostic,
			Status status,
			std::string message,
			std::string node = {},
			std::string port = {}
		) {
			diagnostic = {status, std::move(node), std::move(port), std::move(message)};
		}

		ValueType TypeOf(const Value &value) {
			switch (value.index()) {
			case 0:
				return ValueType::Boolean;
			case 1:
				return ValueType::Integer;
			case 2:
				return ValueType::Scalar;
			case 3:
				return ValueType::Text;
			case 4:
				return ValueType::Colour;
			case 5:
				return ValueType::Vector2;
			case 6:
				return ValueType::Array;
			case 7:
				return ValueType::Gradient;
			case 8:
				return ValueType::Area;
			case 9:
				return ValueType::Curve;
			case 10:
				return ValueType::Vector4;
			case 11:
				return ValueType::Path2D;
			case 12:
				return ValueType::Vector3;
			case 13:
				return ValueType::Quaternion;
			case 14:
				return ValueType::Enum;
			default:
				return ValueType::AudioBit;
			}
		}

		bool IsFinite(const Value &value) {
			if (const auto *scalar = std::get_if<double>(&value)) return std::isfinite(*scalar);
			if (const auto *vector = std::get_if<Vector2>(&value)) {
				return std::isfinite(vector->X) && std::isfinite(vector->Y);
			}
			if (const auto *vector = std::get_if<Vector4>(&value))
				return std::isfinite(vector->X) && std::isfinite(vector->Y) && std::isfinite(vector->Z) &&
					   std::isfinite(vector->W);
			if (const auto *vector = std::get_if<Vector3>(&value))
				return std::isfinite(vector->X) && std::isfinite(vector->Y) && std::isfinite(vector->Z);
			if (const auto *rotation = std::get_if<Quaternion>(&value))
				return std::isfinite(rotation->X) && std::isfinite(rotation->Y) &&
					   std::isfinite(rotation->Z) && std::isfinite(rotation->W);
			if (const auto *audio = std::get_if<AudioBit>(&value)) {
				if (!std::isfinite(audio->SampleRate) || audio->SampleRate <= 0.0 ||
					audio->Samples.size() > Limits::MaximumAudioSamplesPerFrame)
					return false;
				return std::all_of(audio->Samples.begin(), audio->Samples.end(), [](double sample) {
					return std::isfinite(sample);
				});
			}
			if (const auto *gradient = std::get_if<Gradient>(&value)) {
				if (gradient->Mode > 6 || gradient->Keys.empty() ||
					gradient->Keys.size() > Limits::MaximumGradientKeys)
					return false;
				double previous = -1.0;
				for (const GradientKey &key : gradient->Keys) {
					if (!std::isfinite(key.Time) || key.Time < 0.0 || key.Time > 1.0 || key.Time < previous)
						return false;
					previous = key.Time;
				}
				return true;
			}
			if (const auto *area = std::get_if<Area>(&value))
				return std::isfinite(area->CenterX) && std::isfinite(area->CenterY) &&
					   std::isfinite(area->HalfWidth) && std::isfinite(area->HalfHeight) &&
					   area->Shape <= 1 && area->Mode <= 2;
			if (const auto *curve = std::get_if<Curve>(&value)) {
				if (curve->Anchors.size() < 2 || curve->Anchors.size() > Limits::MaximumCurveAnchors)
					return false;
				for (double field : curve->Header)
					if (!std::isfinite(field)) return false;
				for (const auto &anchor : curve->Anchors)
					for (double field : anchor)
						if (!std::isfinite(field)) return false;
				return true;
			}
			if (const auto *path = std::get_if<Path2D>(&value)) {
				if (path->Anchors.size() > Limits::MaximumPathAnchors ||
					path->Weights.size() > Limits::MaximumPathWeights)
					return false;
				for (const PathAnchor &anchor : path->Anchors)
					for (double field : anchor.Controls)
						if (!std::isfinite(field)) return false;
				for (const PathWeight &weight : path->Weights)
					if (!std::isfinite(weight.Position) || !std::isfinite(weight.Weight)) return false;
				return true;
			}
			if (const auto *array = std::get_if<ArrayValue>(&value)) {
				for (const ElementValue &element : array->Elements) {
					if (!std::visit(
							[](const auto &item) -> bool {
								if constexpr (std::is_same_v<std::decay_t<decltype(item)>, double>)
									return std::isfinite(item);
								if constexpr (std::is_same_v<std::decay_t<decltype(item)>, Vector2>)
									return std::isfinite(item.X) && std::isfinite(item.Y);
								return true;
							},
							element
						))
						return false;
				}
			}
			return true;
		}

		bool IsNodeType(std::string_view type) {
			return FindSchema(type) != nullptr;
		}

		const PortSchema *FindPort(std::string_view type, std::string_view id, PortDirection side) {
			const NodeSchema *schema = FindSchema(type);
			if (!schema) return nullptr;
			for (const PortSchema &port : schema->Ports) {
				if (port.Id == id && port.Direction == side) return &port;
			}
			return nullptr;
		}

		std::optional<ValueType> FindPortType(const Node &node, std::string_view id, PortDirection side) {
			if (const PortSchema *port = FindPort(node.Type, id, side)) return port->Type;
			if (side == PortDirection::Input) {
				for (const DynamicInput &input : node.DynamicInputs) {
					if (input.Id == id) return input.Type;
				}
			}
			return std::nullopt;
		}

		bool WithinArrayBudget(const ArrayValue &array) {
			if (array.Elements.size() > Limits::MaximumArrayElements) return false;
			size_t bytes = array.Elements.size() * sizeof(ElementValue);
			if (bytes > Limits::MaximumArrayBytes) return false;
			for (const ElementValue &element : array.Elements) {
				if (const auto *textElement = std::get_if<std::string>(&element)) {
					if (textElement->size() > Limits::MaximumTextBytes ||
						textElement->size() > Limits::MaximumArrayBytes - bytes)
						return false;
					bytes += textElement->size();
				}
			}
			return true;
		}

		bool WithinValueBudget(const Value &value) {
			if (const auto *textValue = std::get_if<std::string>(&value))
				return textValue->size() <= Limits::MaximumTextBytes;
			if (const auto *array = std::get_if<ArrayValue>(&value)) return WithinArrayBudget(*array);
			if (const auto *audio = std::get_if<AudioBit>(&value))
				return audio->Samples.size() <= Limits::MaximumAudioSamplesPerFrame &&
					   audio->Samples.size() <= Limits::MaximumAudioCaptureSamples;
			return true;
		}

		bool ValidArray(const ArrayValue &array) {
			if (!WithinArrayBudget(array) || array.ElementType == ValueType::Image ||
				array.ElementType == ValueType::Array || array.ElementType >= ValueType::Gradient ||
				TypeName(array.ElementType).empty())
				return false;
			for (const ElementValue &element : array.Elements) {
				const Value value = std::visit([](const auto &item) -> Value { return item; }, element);
				if (TypeOf(value) != array.ElementType || !IsFinite(value)) return false;
			}
			return true;
		}

		const PropertySchema *FindProperty(std::string_view type, std::string_view id) {
			const NodeSchema *schema = FindSchema(type);
			if (!schema) return nullptr;
			for (const PropertySchema &property : schema->Properties) {
				if (property.Id == id) return &property;
			}
			return nullptr;
		}

		std::string ValueTag(const Value &value) {
			switch (value.index()) {
			case 0:
				return "b";
			case 1:
				return "i";
			case 2:
				return "d";
			case 3:
				return "s";
			case 4:
				return "c";
			case 5:
				return "v";
			case 6:
				return "a";
			case 7:
				return "g";
			case 8:
				return "r";
			case 9:
				return "q";
			case 10:
				return "w";
			case 11:
				return "p";
			case 12:
				return "3";
			case 13:
				return "h";
			case 14:
				return "e";
			default:
				return "u";
			}
		}

		void WriteQuoted(std::ostream &stream, std::string_view text);
		bool ReadQuoted(std::istream &stream, std::string &text);

		void WriteValue(std::ostream &stream, const Value &value) {
			stream << ValueTag(value) << ' ';
			if (const auto *boolean = std::get_if<bool>(&value)) {
				stream << (*boolean ? 1 : 0);
			} else if (const auto *integer = std::get_if<int64_t>(&value)) {
				stream << *integer;
			} else if (const auto *scalar = std::get_if<double>(&value)) {
				stream << std::setprecision(17) << *scalar;
			} else if (const auto *text = std::get_if<std::string>(&value)) {
				WriteQuoted(stream, *text);
			} else if (const auto *colour = std::get_if<Colour>(&value)) {
				stream << static_cast<unsigned>(colour->Red) << ' ' << static_cast<unsigned>(colour->Green)
					   << ' ' << static_cast<unsigned>(colour->Blue) << ' '
					   << static_cast<unsigned>(colour->Alpha);
			} else if (const auto *vector = std::get_if<Vector2>(&value)) {
				stream << std::setprecision(17) << vector->X << ' ' << vector->Y;
			} else if (const auto *array = std::get_if<ArrayValue>(&value)) {
				stream << TypeName(array->ElementType) << ' ' << array->Elements.size();
				for (const ElementValue &element : array->Elements) {
					stream << ' ';
					std::visit([&](const auto &item) { WriteValue(stream, Value{item}); }, element);
				}
			} else if (const auto *gradient = std::get_if<Gradient>(&value)) {
				stream << unsigned(gradient->Mode) << ' ' << gradient->Keys.size();
				for (const GradientKey &key : gradient->Keys)
					stream << ' ' << std::setprecision(17) << key.Time << ' ' << unsigned(key.Color.Red)
						   << ' ' << unsigned(key.Color.Green) << ' ' << unsigned(key.Color.Blue) << ' '
						   << unsigned(key.Color.Alpha);
			} else if (const auto *area = std::get_if<Area>(&value)) {
				stream << std::setprecision(17) << area->CenterX << ' ' << area->CenterY << ' '
					   << area->HalfWidth << ' ' << area->HalfHeight << ' ' << unsigned(area->Shape) << ' '
					   << unsigned(area->Mode);
			} else if (const auto *curve = std::get_if<Curve>(&value)) {
				stream << curve->Anchors.size();
				for (double field : curve->Header)
					stream << ' ' << std::setprecision(17) << field;
				for (const auto &anchor : curve->Anchors)
					for (double field : anchor)
						stream << ' ' << std::setprecision(17) << field;
			} else if (const auto *vector = std::get_if<Vector4>(&value)) {
				stream << std::setprecision(17) << vector->X << ' ' << vector->Y << ' ' << vector->Z << ' '
					   << vector->W;
			} else if (const auto *vector = std::get_if<Vector3>(&value)) {
				stream << std::setprecision(17) << vector->X << ' ' << vector->Y << ' ' << vector->Z;
			} else if (const auto *rotation = std::get_if<Quaternion>(&value)) {
				stream << std::setprecision(17) << rotation->X << ' ' << rotation->Y << ' ' << rotation->Z
					   << ' ' << rotation->W;
			} else if (const auto *choice = std::get_if<EnumValue>(&value)) {
				stream << choice->Value;
			} else if (const auto *audio = std::get_if<AudioBit>(&value)) {
				stream << std::setprecision(17) << audio->SampleRate << ' ' << audio->Samples.size();
				for (const double sample : audio->Samples)
					stream << ' ' << sample;
			} else {
				const Path2D &path = std::get<Path2D>(value);
				stream << (path.Loop ? 1 : 0) << ' ' << path.Anchors.size() << ' ' << path.Weights.size();
				for (const PathAnchor &anchor : path.Anchors) {
					for (double field : anchor.Controls)
						stream << ' ' << std::setprecision(17) << field;
					stream << ' ' << anchor.Index;
				}
				for (const PathWeight &weight : path.Weights)
					stream << ' ' << std::setprecision(17) << weight.Position << ' ' << weight.Weight;
			}
		}

		bool ReadValue(std::istream &stream, Value &value, uint32_t version, bool allowArray = true) {
			std::string tag;
			if (!(stream >> tag)) return false;
			if (tag == "b") {
				int stored = 0;
				if (!(stream >> stored) || (stored != 0 && stored != 1)) return false;
				value = stored != 0;
				return true;
			}
			if (tag == "i") {
				int64_t stored = 0;
				if (!(stream >> stored)) return false;
				value = stored;
				return true;
			}
			if (tag == "d") {
				double stored = 0.0;
				if (!(stream >> stored) || !std::isfinite(stored)) return false;
				value = stored;
				return true;
			}
			if (tag == "s") {
				std::string stored;
				if (!ReadQuoted(stream, stored)) return false;
				value = std::move(stored);
				return true;
			}
			if (tag == "c") {
				unsigned red = 0, green = 0, blue = 0, alpha = 0;
				if (!(stream >> red >> green >> blue >> alpha) || red > 255 || green > 255 || blue > 255 ||
					alpha > 255) {
					return false;
				}
				value = Colour{
					static_cast<uint8_t>(red),
					static_cast<uint8_t>(green),
					static_cast<uint8_t>(blue),
					static_cast<uint8_t>(alpha)
				};
				return true;
			}
			if (tag == "v") {
				Vector2 stored;
				if (!(stream >> stored.X >> stored.Y) || !std::isfinite(stored.X) || !std::isfinite(stored.Y))
					return false;
				value = stored;
				return true;
			}
			if (tag == "a" && allowArray) {
				std::string typeName;
				size_t count = 0;
				if (!(stream >> typeName >> count) || count > Limits::MaximumArrayElements) return false;
				const auto type = ParseType(typeName);
				if (!type || *type == ValueType::Image || *type == ValueType::Array ||
					*type >= ValueType::Gradient)
					return false;
				ArrayValue array{*type, {}};
				array.Elements.reserve(count);
				for (size_t index = 0; index < count; index++) {
					Value element;
					if (!ReadValue(stream, element, version, false) || TypeOf(element) != *type) return false;
					std::visit(
						[&](const auto &item) {
							using T = std::decay_t<decltype(item)>;
							if constexpr (std::is_constructible_v<ElementValue, T>)
								array.Elements.emplace_back(item);
						},
						element
					);
				}
				value = std::move(array);
				return true;
			}
			if (version < 3) return false;
			if (tag == "g") {
				unsigned mode = 0;
				size_t count = 0;
				if (!(stream >> mode >> count) || mode > 6 || count == 0 ||
					count > Limits::MaximumGradientKeys)
					return false;
				Gradient gradient{static_cast<uint8_t>(mode), {}};
				gradient.Keys.reserve(count);
				for (size_t index = 0; index < count; index++) {
					GradientKey key;
					unsigned red = 0, green = 0, blue = 0, alpha = 0;
					if (!(stream >> key.Time >> red >> green >> blue >> alpha) || !std::isfinite(key.Time) ||
						red > 255 || green > 255 || blue > 255 || alpha > 255)
						return false;
					key.Color = Colour{
						static_cast<uint8_t>(red),
						static_cast<uint8_t>(green),
						static_cast<uint8_t>(blue),
						static_cast<uint8_t>(alpha)
					};
					gradient.Keys.push_back(key);
				}
				value = std::move(gradient);
				return IsFinite(value);
			}
			if (tag == "r") {
				Area area;
				unsigned shape = 0, mode = 0;
				if (!(stream >> area.CenterX >> area.CenterY >> area.HalfWidth >> area.HalfHeight >> shape >>
					  mode) ||
					shape > 1 || mode > 2)
					return false;
				area.Shape = static_cast<uint8_t>(shape);
				area.Mode = static_cast<uint8_t>(mode);
				value = area;
				return IsFinite(value);
			}
			if (tag == "q") {
				size_t count = 0;
				if (!(stream >> count) || count < 2 || count > Limits::MaximumCurveAnchors) return false;
				Curve curve;
				for (double &field : curve.Header)
					if (!(stream >> field) || !std::isfinite(field)) return false;
				curve.Anchors.reserve(count);
				for (size_t index = 0; index < count; index++) {
					std::array<double, 6> anchor{};
					for (double &field : anchor)
						if (!(stream >> field) || !std::isfinite(field)) return false;
					curve.Anchors.push_back(anchor);
				}
				value = std::move(curve);
				return true;
			}
			if (tag == "w") {
				Vector4 vector;
				if (!(stream >> vector.X >> vector.Y >> vector.Z >> vector.W)) return false;
				value = vector;
				return IsFinite(value);
			}
			if (tag == "3") {
				Vector3 vector;
				if (!(stream >> vector.X >> vector.Y >> vector.Z)) return false;
				value = vector;
				return IsFinite(value);
			}
			if (tag == "h") {
				Quaternion rotation;
				if (!(stream >> rotation.X >> rotation.Y >> rotation.Z >> rotation.W)) return false;
				value = rotation;
				return IsFinite(value);
			}
			if (tag == "e") {
				EnumValue choice;
				if (!(stream >> choice.Value)) return false;
				value = choice;
				return true;
			}
			if (tag == "u") {
				AudioBit audio;
				size_t count = 0;
				if (!(stream >> audio.SampleRate >> count) || !std::isfinite(audio.SampleRate) ||
					audio.SampleRate <= 0.0 || count > Limits::MaximumAudioSamplesPerFrame)
					return false;
				audio.Samples.reserve(count);
				for (size_t index = 0; index < count; ++index) {
					double sample = 0.0;
					if (!(stream >> sample) || !std::isfinite(sample)) return false;
					audio.Samples.push_back(sample);
				}
				value = std::move(audio);
				return true;
			}
			if (tag == "p") {
				unsigned loop = 0;
				size_t anchorCount = 0, weightCount = 0;
				if (!(stream >> loop >> anchorCount >> weightCount) || loop > 1 ||
					anchorCount > Limits::MaximumPathAnchors || weightCount > Limits::MaximumPathWeights)
					return false;
				Path2D path;
				path.Loop = loop != 0;
				path.Anchors.reserve(anchorCount);
				path.Weights.reserve(weightCount);
				for (size_t index = 0; index < anchorCount; index++) {
					PathAnchor anchor;
					for (double &field : anchor.Controls)
						if (!(stream >> field) || !std::isfinite(field)) return false;
					if (!(stream >> anchor.Index)) return false;
					path.Anchors.push_back(anchor);
				}
				for (size_t index = 0; index < weightCount; index++) {
					PathWeight weight;
					if (!(stream >> weight.Position >> weight.Weight) || !std::isfinite(weight.Position) ||
						!std::isfinite(weight.Weight))
						return false;
					path.Weights.push_back(weight);
				}
				value = std::move(path);
				return true;
			}
			return false;
		}

		bool HasTrailing(std::istream &stream) {
			stream >> std::ws;
			return !stream.eof();
		}

		void WriteQuoted(std::ostream &stream, std::string_view text) {
			stream.put('"');
			for (const char character : text) {
				switch (character) {
				case '\\':
					stream << "\\\\";
					break;
				case '"':
					stream << "\\\"";
					break;
				case '\n':
					stream << "\\n";
					break;
				case '\r':
					stream << "\\r";
					break;
				case '\t':
					stream << "\\t";
					break;
				default:
					stream.put(character);
					break;
				}
			}
			stream.put('"');
		}

		bool ReadQuoted(std::istream &stream, std::string &text) {
			stream >> std::ws;
			if (stream.get() != '"') return false;
			text.clear();
			char character = 0;
			while (stream.get(character)) {
				if (character == '"') return true;
				if (character != '\\') {
					text.push_back(character);
					continue;
				}
				if (!stream.get(character)) return false;
				switch (character) {
				case '\\':
					text.push_back('\\');
					break;
				case '"':
					text.push_back('"');
					break;
				case 'n':
					text.push_back('\n');
					break;
				case 'r':
					text.push_back('\r');
					break;
				case 't':
					text.push_back('\t');
					break;
				default:
					return false;
				}
			}
			return false;
		}

		const AuthoredValue *FindValue(const Node &node, std::string_view port) {
			for (const AuthoredValue &value : node.Values) {
				if (value.Port == port) return &value;
			}
			return nullptr;
		}
	}

	const NodeSchema *FindSchema(std::string_view type) {
		if (const NodeSchema *schema = detail::FindValueNodeSchema(type)) return schema;
		if (type == SOLID_SCHEMA.Type) return &SOLID_SCHEMA;
		if (type == GRADIENT_SCHEMA.Type) return &GRADIENT_SCHEMA;
		if (type == CHECKER_SCHEMA.Type) return &CHECKER_SCHEMA;
		if (type == GREYSCALE_SCHEMA.Type) return &GREYSCALE_SCHEMA;
		if (type == BW_SCHEMA.Type) return &BW_SCHEMA;
		if (type == GREY_ALPHA_SCHEMA.Type) return &GREY_ALPHA_SCHEMA;
		if (type == RGB_EXTRACT_SCHEMA.Type) return &RGB_EXTRACT_SCHEMA;
		if (type == HSV_EXTRACT_SCHEMA.Type) return &HSV_EXTRACT_SCHEMA;
		if (type == RGB_COMBINE_SCHEMA.Type) return &RGB_COMBINE_SCHEMA;
		if (type == HSV_COMBINE_SCHEMA.Type) return &HSV_COMBINE_SCHEMA;
		if (type == OVERRIDE_CHANNEL_SCHEMA.Type) return &OVERRIDE_CHANNEL_SCHEMA;
		if (type == MULTIPLY_ALPHA_SCHEMA.Type) return &MULTIPLY_ALPHA_SCHEMA;
		if (type == GAMMA_MAP_SCHEMA.Type) return &GAMMA_MAP_SCHEMA;
		if (type == MIRROR_SCHEMA.Type) return &MIRROR_SCHEMA;
		if (type == BARREL_SCHEMA.Type) return &BARREL_SCHEMA;
		if (type == CHROMATIC_SCHEMA.Type) return &CHROMATIC_SCHEMA;
		if (type == SPHERIZE_SCHEMA.Type) return &SPHERIZE_SCHEMA;
		if (type == DILATE_SCHEMA.Type) return &DILATE_SCHEMA;
		if (type == SIMPLEX_SCHEMA.Type) return &SIMPLEX_SCHEMA;
		if (type == TILE_SCHEMA.Type) return &TILE_SCHEMA;
		if (type == SHAPE_SCHEMA.Type) return &SHAPE_SCHEMA;
		if (type == COLOR_ADJUST_SCHEMA.Type) return &COLOR_ADJUST_SCHEMA;
		if (type == BLUR_SCHEMA.Type) return &BLUR_SCHEMA;
		if (type == VIGNETTE_SCHEMA.Type) return &VIGNETTE_SCHEMA;
		if (type == DISPLACE_SCHEMA.Type) return &DISPLACE_SCHEMA;
		if (type == POLAR_SCHEMA.Type) return &POLAR_SCHEMA;
		if (type == CURVE_COLOR_SCHEMA.Type) return &CURVE_COLOR_SCHEMA;
		if (type == COLORIZE_SCHEMA.Type) return &COLORIZE_SCHEMA;
		if (type == HEIGHT_BLEND_SCHEMA.Type) return &HEIGHT_BLEND_SCHEMA;
		if (type == BLEND_SCHEMA.Type) return &BLEND_SCHEMA;
		if (type == PASS_SCHEMA.Type) return &PASS_SCHEMA;
		if (type == FLIP_SCHEMA.Type) return &FLIP_SCHEMA;
		if (type == INVERT_SCHEMA.Type) return &INVERT_SCHEMA;
		if (type == CUTOFF_SCHEMA.Type) return &CUTOFF_SCHEMA;
		if (type == OFFSET_SCHEMA.Type) return &OFFSET_SCHEMA;
		if (type == THRESHOLD_SCHEMA.Type) return &THRESHOLD_SCHEMA;
		if (type == POSTERIZE_SCHEMA.Type) return &POSTERIZE_SCHEMA;
		if (type == ARRAY_SCHEMA.Type) return &ARRAY_SCHEMA;
		if (type == ARRAY_GET_SCHEMA.Type) return &ARRAY_GET_SCHEMA;
		if (type == TRANSFORM_IMAGE_3D_SCHEMA.Type) return &TRANSFORM_IMAGE_3D_SCHEMA;
		return nullptr;
	}

	std::string Write(const Document &document) {
		std::ostringstream stream;
		stream.imbue(std::locale::classic());
		stream << "imagegraph " << document.FormatVersion << '\n';
		for (size_t nodeIndex = 0; nodeIndex < document.Nodes.size(); nodeIndex++) {
			const Node &node = document.Nodes[nodeIndex];
			stream << "node ";
			WriteQuoted(stream, node.Id);
			stream << ' ';
			WriteQuoted(stream, node.Type);
			stream << ' ';
			WriteQuoted(stream, node.GroupId);
			stream << ' ' << std::setprecision(17) << node.Position.X << ' ' << node.Position.Y << '\n';
			for (const AuthoredValue &value : node.Values) {
				stream << "value " << nodeIndex << ' ';
				WriteQuoted(stream, node.Id);
				stream << ' ';
				WriteQuoted(stream, value.Port);
				stream << ' ';
				WriteValue(stream, value.Data);
				stream << '\n';
			}
			if (document.FormatVersion >= 2) {
				for (const DynamicInput &input : node.DynamicInputs) {
					stream << "dynamic " << nodeIndex << ' ';
					WriteQuoted(stream, node.Id);
					stream << ' ';
					WriteQuoted(stream, input.Id);
					stream << ' ' << TypeName(input.Type) << ' ' << (input.Default ? 1 : 0);
					if (input.Default) {
						stream << ' ';
						WriteValue(stream, *input.Default);
					}
					stream << '\n';
				}
			}
		}
		for (const Link &link : document.Links) {
			stream << "link ";
			WriteQuoted(stream, link.FromNode);
			stream << ' ';
			WriteQuoted(stream, link.FromPort);
			stream << ' ';
			WriteQuoted(stream, link.ToNode);
			stream << ' ';
			WriteQuoted(stream, link.ToPort);
			stream << '\n';
		}
		for (const Group &group : document.Groups) {
			stream << "group ";
			WriteQuoted(stream, group.Id);
			stream << ' ';
			WriteQuoted(stream, group.Name);
			if (document.FormatVersion >= 2) {
				stream << ' ';
				WriteQuoted(stream, group.ParentId);
			}
			stream << '\n';
			if (document.FormatVersion >= 2) {
				for (const GroupPort &port : group.Ports) {
					stream << "group_port ";
					WriteQuoted(stream, group.Id);
					stream << ' ';
					WriteQuoted(stream, port.Id);
					stream << ' ';
					WriteQuoted(stream, port.JunctionId);
					stream << ' ' << DirectionName(port.Direction) << '\n';
				}
			}
		}
		if (document.FormatVersion >= 2) {
			for (const Junction &junction : document.Junctions) {
				stream << "junction ";
				WriteQuoted(stream, junction.Id);
				stream << ' ';
				WriteQuoted(stream, junction.GroupId);
				stream << ' ' << TypeName(junction.Type) << ' ' << (junction.Default ? 1 : 0);
				if (junction.Default) {
					stream << ' ';
					WriteValue(stream, *junction.Default);
				}
				stream << '\n';
			}
		}
		for (const Output &output : document.Outputs) {
			stream << "output ";
			WriteQuoted(stream, output.Id);
			stream << ' ';
			WriteQuoted(stream, output.NodeId);
			stream << ' ';
			WriteQuoted(stream, output.Port);
			stream << '\n';
		}
		for (const Keyframe &keyframe : document.Keyframes) {
			stream << "keyframe ";
			WriteQuoted(stream, keyframe.NodeId);
			stream << ' ';
			WriteQuoted(stream, keyframe.Port);
			stream << ' ' << keyframe.Tick << ' ';
			WriteQuoted(stream, keyframe.Interpolation);
			stream << ' ';
			WriteValue(stream, keyframe.Data);
			stream << '\n';
			if (document.FormatVersion >= 4 && keyframe.Ease) {
				stream << "key_ease ";
				WriteQuoted(stream, keyframe.NodeId);
				stream << ' ';
				WriteQuoted(stream, keyframe.Port);
				stream << ' ' << keyframe.Tick << ' ';
				WriteQuoted(stream, keyframe.Ease->InType);
				stream << ' ';
				WriteQuoted(stream, keyframe.Ease->OutType);
				stream << ' ' << std::setprecision(17) << keyframe.Ease->In.X << ' ' << keyframe.Ease->In.Y
					   << ' ' << keyframe.Ease->Out.X << ' ' << keyframe.Ease->Out.Y << '\n';
			}
			if (document.FormatVersion >= 6 && keyframe.SineDriver) {
				stream << "key_driver ";
				WriteQuoted(stream, keyframe.NodeId);
				stream << ' ';
				WriteQuoted(stream, keyframe.Port);
				stream << ' ' << keyframe.Tick << ' ';
				WriteQuoted(stream, "sine");
				stream << ' ' << std::setprecision(17) << keyframe.SineDriver->Frequency << ' '
					   << keyframe.SineDriver->Amplitude << ' ' << keyframe.SineDriver->Phase << ' '
					   << keyframe.SineDriver->Smooth << '\n';
			}
		}
		if (document.FormatVersion >= 4) {
			if (document.Timeline) {
				stream << "timeline " << document.Timeline->Frames << ' ' << document.Timeline->First << ' '
					   << document.Timeline->Last << ' ';
				WriteQuoted(stream, document.Timeline->Playback);
				if (document.FormatVersion >= 5)
					stream << ' ' << std::setprecision(17) << document.Timeline->FramesPerSecond;
				stream << '\n';
			}
			for (const AnimationTrack &track : document.Tracks) {
				stream << "track ";
				WriteQuoted(stream, track.NodeId);
				stream << ' ';
				WriteQuoted(stream, track.Port);
				stream << ' ';
				WriteQuoted(stream, track.End);
				stream << ' ' << track.LoopRange << '\n';
			}
		}
		return stream.str();
	}

	Status Read(const std::string &text, Document &document, Diagnostic &diagnostic) {
		if (text.size() > Limits::MaximumDocumentBytes) {
			SetDiagnostic(diagnostic, Status::LimitExceeded, "document text exceeds the byte limit");
			return diagnostic.Code;
		}
		std::istringstream input(text);
		input.imbue(std::locale::classic());
		std::string line;
		if (!std::getline(input, line)) {
			SetDiagnostic(diagnostic, Status::Malformed, "missing imagegraph header");
			return diagnostic.Code;
		}
		std::istringstream header(line);
		std::string marker;
		Document parsed;
		if (!(header >> marker >> parsed.FormatVersion) || marker != "imagegraph" || HasTrailing(header)) {
			SetDiagnostic(diagnostic, Status::Malformed, "invalid imagegraph header");
			return diagnostic.Code;
		}
		if (parsed.FormatVersion != 1 && parsed.FormatVersion != 2 && parsed.FormatVersion != 3 &&
			parsed.FormatVersion != 4 && parsed.FormatVersion != 5 && parsed.FormatVersion != 6) {
			SetDiagnostic(diagnostic, Status::UnsupportedVersion, "unsupported imagegraph document version");
			return diagnostic.Code;
		}

		size_t lineNumber = 1;
		while (std::getline(input, line)) {
			lineNumber++;
			if (line.empty()) continue;
			std::istringstream row(line);
			row.imbue(std::locale::classic());
			if (!(row >> marker)) continue;
			if (marker == "node") {
				Node node;
				if (!ReadQuoted(row, node.Id) || !ReadQuoted(row, node.Type) ||
					!ReadQuoted(row, node.GroupId) || !(row >> node.Position.X >> node.Position.Y) ||
					!std::isfinite(node.Position.X) || !std::isfinite(node.Position.Y) || HasTrailing(row))
					goto malformed;
				if (parsed.Nodes.size() == Limits::MaximumNodes) {
					SetDiagnostic(
						diagnostic, Status::LimitExceeded, "document exceeds the node limit", node.Id
					);
					return diagnostic.Code;
				}
				parsed.Nodes.push_back(std::move(node));
			} else if (marker == "value") {
				size_t nodeIndex = 0;
				std::string nodeId, port;
				Value value;
				if (!(row >> nodeIndex) || !ReadQuoted(row, nodeId) || !ReadQuoted(row, port) ||
					!ReadValue(row, value, parsed.FormatVersion) || HasTrailing(row))
					goto malformed;
				if (nodeIndex >= parsed.Nodes.size() || parsed.Nodes[nodeIndex].Id != nodeId) {
					SetDiagnostic(diagnostic, Status::Malformed, "value precedes its node", nodeId, port);
					return diagnostic.Code;
				}
				Node &node = parsed.Nodes[nodeIndex];
				if (node.Values.size() == Limits::MaximumPropertiesPerNode) {
					SetDiagnostic(
						diagnostic,
						Status::LimitExceeded,
						"node exceeds the authored property limit",
						nodeId,
						port
					);
					return diagnostic.Code;
				}
				if (!WithinValueBudget(value)) {
					SetDiagnostic(
						diagnostic,
						Status::LimitExceeded,
						"authored value exceeds its byte or element limit",
						nodeId,
						port
					);
					return diagnostic.Code;
				}
				node.Values.push_back({std::move(port), std::move(value)});
			} else if (marker == "dynamic" && parsed.FormatVersion >= 2) {
				size_t nodeIndex = 0;
				std::string nodeId, inputId, typeName;
				int hasDefault = 0;
				if (!(row >> nodeIndex) || !ReadQuoted(row, nodeId) || !ReadQuoted(row, inputId) ||
					!(row >> typeName >> hasDefault) || (hasDefault != 0 && hasDefault != 1))
					goto malformed;
				const auto type = ParseType(typeName);
				if (!type || (parsed.FormatVersion < 3 && *type >= ValueType::Gradient)) goto malformed;
				if (nodeIndex >= parsed.Nodes.size() || parsed.Nodes[nodeIndex].Id != nodeId) {
					SetDiagnostic(
						diagnostic, Status::Malformed, "dynamic input precedes its node", nodeId, inputId
					);
					return diagnostic.Code;
				}
				DynamicInput dynamic{std::move(inputId), *type, std::nullopt};
				if (hasDefault) {
					Value value;
					if (!ReadValue(row, value, parsed.FormatVersion)) goto malformed;
					if (!WithinValueBudget(value)) {
						SetDiagnostic(
							diagnostic,
							Status::LimitExceeded,
							"dynamic input default exceeds its limit",
							nodeId,
							dynamic.Id
						);
						return diagnostic.Code;
					}
					dynamic.Default = std::move(value);
				}
				if (HasTrailing(row)) goto malformed;
				if (parsed.Nodes[nodeIndex].DynamicInputs.size() == Limits::MaximumDynamicInputsPerNode) {
					SetDiagnostic(
						diagnostic,
						Status::LimitExceeded,
						"node exceeds the dynamic input limit",
						nodeId,
						dynamic.Id
					);
					return diagnostic.Code;
				}
				parsed.Nodes[nodeIndex].DynamicInputs.push_back(std::move(dynamic));
			} else if (marker == "link") {
				Link link;
				if (!ReadQuoted(row, link.FromNode) || !ReadQuoted(row, link.FromPort) ||
					!ReadQuoted(row, link.ToNode) || !ReadQuoted(row, link.ToPort) || HasTrailing(row))
					goto malformed;
				if (parsed.Links.size() == Limits::MaximumLinks) {
					SetDiagnostic(diagnostic, Status::LimitExceeded, "document exceeds the link limit");
					return diagnostic.Code;
				}
				parsed.Links.push_back(std::move(link));
			} else if (marker == "group") {
				Group group;
				if (!ReadQuoted(row, group.Id) || !ReadQuoted(row, group.Name) ||
					(parsed.FormatVersion >= 2 && !ReadQuoted(row, group.ParentId)) || HasTrailing(row))
					goto malformed;
				if (parsed.Groups.size() == Limits::MaximumGroups) {
					SetDiagnostic(
						diagnostic, Status::LimitExceeded, "document exceeds the group limit", group.Id
					);
					return diagnostic.Code;
				}
				parsed.Groups.push_back(std::move(group));
			} else if (marker == "group_port" && parsed.FormatVersion >= 2) {
				std::string groupId, portId, junctionId, direction;
				if (!ReadQuoted(row, groupId) || !ReadQuoted(row, portId) || !ReadQuoted(row, junctionId) ||
					!(row >> direction) || HasTrailing(row) ||
					(direction != "input" && direction != "output"))
					goto malformed;
				const auto group =
					std::find_if(parsed.Groups.begin(), parsed.Groups.end(), [&](const Group &candidate) {
						return candidate.Id == groupId;
					});
				if (group == parsed.Groups.end()) {
					SetDiagnostic(
						diagnostic, Status::Malformed, "group port precedes its group", groupId, portId
					);
					return diagnostic.Code;
				}
				if (group->Ports.size() == Limits::MaximumGroupPorts) {
					SetDiagnostic(
						diagnostic, Status::LimitExceeded, "group exceeds its port limit", groupId, portId
					);
					return diagnostic.Code;
				}
				group->Ports.push_back(
					{std::move(portId),
					 std::move(junctionId),
					 direction == "input" ? PortDirection::Input : PortDirection::Output}
				);
			} else if (marker == "junction" && parsed.FormatVersion >= 2) {
				Junction junction;
				std::string typeName;
				int hasDefault = 0;
				if (!ReadQuoted(row, junction.Id) || !ReadQuoted(row, junction.GroupId) ||
					!(row >> typeName >> hasDefault) || (hasDefault != 0 && hasDefault != 1))
					goto malformed;
				const auto type = ParseType(typeName);
				if (!type || (parsed.FormatVersion < 3 && *type >= ValueType::Gradient)) goto malformed;
				junction.Type = *type;
				if (hasDefault) {
					Value value;
					if (!ReadValue(row, value, parsed.FormatVersion)) goto malformed;
					if (!WithinValueBudget(value)) {
						SetDiagnostic(
							diagnostic,
							Status::LimitExceeded,
							"junction default exceeds its limit",
							junction.Id,
							"value"
						);
						return diagnostic.Code;
					}
					junction.Default = std::move(value);
				}
				if (HasTrailing(row)) goto malformed;
				if (parsed.Junctions.size() == Limits::MaximumJunctions) {
					SetDiagnostic(
						diagnostic, Status::LimitExceeded, "document exceeds the junction limit", junction.Id
					);
					return diagnostic.Code;
				}
				parsed.Junctions.push_back(std::move(junction));
			} else if (marker == "output") {
				Output output;
				if (!ReadQuoted(row, output.Id) || !ReadQuoted(row, output.NodeId) ||
					!ReadQuoted(row, output.Port) || HasTrailing(row))
					goto malformed;
				if (parsed.Outputs.size() == Limits::MaximumOutputs) {
					SetDiagnostic(
						diagnostic,
						Status::LimitExceeded,
						"document exceeds the output limit",
						output.NodeId,
						output.Port
					);
					return diagnostic.Code;
				}
				parsed.Outputs.push_back(std::move(output));
			} else if (marker == "keyframe") {
				Keyframe keyframe;
				if (!ReadQuoted(row, keyframe.NodeId) || !ReadQuoted(row, keyframe.Port) ||
					!(row >> keyframe.Tick) || !ReadQuoted(row, keyframe.Interpolation) ||
					!ReadValue(row, keyframe.Data, parsed.FormatVersion) || HasTrailing(row))
					goto malformed;
				if (!WithinValueBudget(keyframe.Data)) {
					SetDiagnostic(
						diagnostic,
						Status::LimitExceeded,
						"keyframe value exceeds its limit",
						keyframe.NodeId,
						keyframe.Port
					);
					return diagnostic.Code;
				}
				if (parsed.Keyframes.size() == Limits::MaximumKeyframes) {
					SetDiagnostic(
						diagnostic,
						Status::LimitExceeded,
						"document exceeds the keyframe limit",
						keyframe.NodeId,
						keyframe.Port
					);
					return diagnostic.Code;
				}
				parsed.Keyframes.push_back(std::move(keyframe));
			} else if (marker == "key_ease" && parsed.FormatVersion >= 4) {
				std::string nodeId, port;
				uint64_t tick = 0;
				KeyframeEase ease;
				if (!ReadQuoted(row, nodeId) || !ReadQuoted(row, port) || !(row >> tick) ||
					!ReadQuoted(row, ease.InType) || !ReadQuoted(row, ease.OutType) ||
					!(row >> ease.In.X >> ease.In.Y >> ease.Out.X >> ease.Out.Y) || HasTrailing(row))
					goto malformed;
				if (parsed.Keyframes.empty() || parsed.Keyframes.back().NodeId != nodeId ||
					parsed.Keyframes.back().Port != port || parsed.Keyframes.back().Tick != tick ||
					parsed.Keyframes.back().Ease)
					goto malformed;
				if (!std::isfinite(ease.In.X) || !std::isfinite(ease.In.Y) || !std::isfinite(ease.Out.X) ||
					!std::isfinite(ease.Out.Y))
					goto malformed;
				parsed.Keyframes.back().Ease = std::move(ease);
			} else if (marker == "key_driver" && parsed.FormatVersion >= 6) {
				std::string nodeId, port, type;
				uint64_t tick = 0;
				KeyframeSineDriver driver;
				if (!ReadQuoted(row, nodeId) || !ReadQuoted(row, port) || !(row >> tick) ||
					!ReadQuoted(row, type) ||
					!(row >> driver.Frequency >> driver.Amplitude >> driver.Phase >> driver.Smooth) ||
					HasTrailing(row) || type != "sine" || !std::isfinite(driver.Frequency) ||
					!std::isfinite(driver.Amplitude) || !std::isfinite(driver.Phase) ||
					!std::isfinite(driver.Smooth))
					goto malformed;
				if (parsed.Keyframes.empty() || parsed.Keyframes.back().NodeId != nodeId ||
					parsed.Keyframes.back().Port != port || parsed.Keyframes.back().Tick != tick ||
					parsed.Keyframes.back().SineDriver)
					goto malformed;
				parsed.Keyframes.back().SineDriver = driver;
			} else if (marker == "timeline" && parsed.FormatVersion >= 4) {
				TimelineSettings timeline;
				if (parsed.Timeline || !(row >> timeline.Frames >> timeline.First >> timeline.Last) ||
					!ReadQuoted(row, timeline.Playback))
					goto malformed;
				if (parsed.FormatVersion >= 5 && !(row >> timeline.FramesPerSecond)) goto malformed;
				if (HasTrailing(row) || !std::isfinite(timeline.FramesPerSecond) ||
					timeline.FramesPerSecond <= 0 || !std::isfinite(1.0 / timeline.FramesPerSecond) ||
					1.0 / timeline.FramesPerSecond <= 0)
					goto malformed;
				parsed.Timeline = std::move(timeline);
			} else if (marker == "track" && parsed.FormatVersion >= 4) {
				AnimationTrack track;
				if (!ReadQuoted(row, track.NodeId) || !ReadQuoted(row, track.Port) ||
					!ReadQuoted(row, track.End) || !(row >> track.LoopRange) || HasTrailing(row))
					goto malformed;
				if (parsed.Tracks.size() == Limits::MaximumTracks) {
					SetDiagnostic(
						diagnostic,
						Status::LimitExceeded,
						"document exceeds the track limit",
						track.NodeId,
						track.Port
					);
					return diagnostic.Code;
				}
				parsed.Tracks.push_back(std::move(track));
			} else {
				goto malformed;
			}
		}
		document = std::move(parsed);
		diagnostic = {};
		return Status::Ok;

	malformed:
		SetDiagnostic(
			diagnostic, Status::Malformed, "malformed imagegraph record at line " + std::to_string(lineNumber)
		);
		return diagnostic.Code;
	}

	Status Migrate(Document &document, Diagnostic &diagnostic) {
		if (document.FormatVersion != 1 && document.FormatVersion != 2 && document.FormatVersion != 3 &&
			document.FormatVersion != 4 && document.FormatVersion != 5 && document.FormatVersion != 6) {
			SetDiagnostic(diagnostic, Status::UnsupportedVersion, "unsupported imagegraph document version");
			return diagnostic.Code;
		}
		if (document.FormatVersion == 1) {
			if (!document.Junctions.empty()) {
				SetDiagnostic(diagnostic, Status::InvalidValue, "legacy document contains v2 junctions");
				return diagnostic.Code;
			}
			for (const Node &node : document.Nodes) {
				if (!node.DynamicInputs.empty()) {
					SetDiagnostic(
						diagnostic,
						Status::InvalidValue,
						"legacy document contains v2 dynamic inputs",
						node.Id
					);
					return diagnostic.Code;
				}
			}
			for (const Group &group : document.Groups) {
				if (!group.ParentId.empty() || !group.Ports.empty()) {
					SetDiagnostic(
						diagnostic, Status::InvalidGroup, "legacy document contains a nested group", group.Id
					);
					return diagnostic.Code;
				}
			}
			document.FormatVersion = 2;
		}
		if (document.FormatVersion == 2) document.FormatVersion = 3;
		if (document.FormatVersion == 3) document.FormatVersion = 4;
		if (document.FormatVersion == 4) {
			if (document.Timeline) document.Timeline->FramesPerSecond = 30.0;
			document.FormatVersion = 5;
		}
		if (document.FormatVersion == 5) document.FormatVersion = 6;
		diagnostic = {};
		return Status::Ok;
	}

	Status Compile(const Document &document, Plan &plan, Diagnostic &diagnostic) {
		if (document.FormatVersion != 1 && document.FormatVersion != 2 && document.FormatVersion != 3 &&
			document.FormatVersion != 4 && document.FormatVersion != 5 && document.FormatVersion != 6) {
			SetDiagnostic(diagnostic, Status::UnsupportedVersion, "unsupported imagegraph document version");
			return diagnostic.Code;
		}
		if (document.Nodes.size() > Limits::MaximumNodes || document.Links.size() > Limits::MaximumLinks ||
			document.Groups.size() > Limits::MaximumGroups ||
			document.Junctions.size() > Limits::MaximumJunctions ||
			document.Outputs.size() > Limits::MaximumOutputs ||
			document.Keyframes.size() > Limits::MaximumKeyframes ||
			document.Tracks.size() > Limits::MaximumTracks) {
			SetDiagnostic(diagnostic, Status::LimitExceeded, "document exceeds a graph count limit");
			return diagnostic.Code;
		}
		if (document.FormatVersion < 4) {
			if (document.Timeline || !document.Tracks.empty() ||
				std::any_of(document.Keyframes.begin(), document.Keyframes.end(), [](const Keyframe &key) {
					return key.Ease.has_value() || key.SineDriver.has_value();
				})) {
				SetDiagnostic(diagnostic, Status::InvalidValue, "timeline metadata needs imagegraph v4");
				return diagnostic.Code;
			}
		}
		if (document.FormatVersion < 6 && std::any_of(
											  document.Keyframes.begin(),
											  document.Keyframes.end(),
											  [](const Keyframe &key) { return key.SineDriver.has_value(); }
										  )) {
			SetDiagnostic(diagnostic, Status::UnsupportedVersion, "sine keyframe driver needs imagegraph v6");
			return diagnostic.Code;
		}
		if (document.Timeline) {
			const TimelineSettings &timeline = *document.Timeline;
			const double frameSeconds = 1.0 / timeline.FramesPerSecond;
			if (!std::isfinite(timeline.FramesPerSecond) || timeline.FramesPerSecond <= 0 ||
				!std::isfinite(frameSeconds) || frameSeconds <= 0 ||
				(document.FormatVersion < 5 && timeline.FramesPerSecond != 30.0)) {
				SetDiagnostic(
					diagnostic,
					Status::InvalidValue,
					"timeline frame rate must be finite and positive",
					{},
					"timeline"
				);
				return diagnostic.Code;
			}
			if (timeline.Frames == 0 || timeline.Frames > Limits::MaximumTick + 1 ||
				timeline.First > timeline.Last || timeline.Last >= timeline.Frames) {
				SetDiagnostic(
					diagnostic,
					Status::InvalidValue,
					"timeline range must fit its bounded frame count",
					{},
					"timeline"
				);
				return diagnostic.Code;
			}
			if (timeline.Playback != "loop" && timeline.Playback != "stop" &&
				timeline.Playback != "pingpong") {
				SetDiagnostic(
					diagnostic,
					Status::InvalidValue,
					"timeline playback mode is not registered",
					{},
					"timeline"
				);
				return diagnostic.Code;
			}
		}
		if (document.FormatVersion < 3) {
			for (const Node &node : document.Nodes) {
				for (const AuthoredValue &authored : node.Values)
					if (TypeOf(authored.Data) >= ValueType::Gradient) {
						SetDiagnostic(
							diagnostic,
							Status::UnsupportedVersion,
							"structured value needs imagegraph v3",
							node.Id,
							authored.Port
						);
						return diagnostic.Code;
					}
				for (const DynamicInput &input : node.DynamicInputs)
					if (input.Type >= ValueType::Gradient ||
						(input.Default && TypeOf(*input.Default) >= ValueType::Gradient)) {
						SetDiagnostic(
							diagnostic,
							Status::UnsupportedVersion,
							"structured input needs imagegraph v3",
							node.Id,
							input.Id
						);
						return diagnostic.Code;
					}
			}
			for (const Junction &junction : document.Junctions)
				if (junction.Type >= ValueType::Gradient ||
					(junction.Default && TypeOf(*junction.Default) >= ValueType::Gradient)) {
					SetDiagnostic(
						diagnostic,
						Status::UnsupportedVersion,
						"structured junction needs imagegraph v3",
						junction.Id,
						"value"
					);
					return diagnostic.Code;
				}
			for (const Keyframe &keyframe : document.Keyframes)
				if (TypeOf(keyframe.Data) >= ValueType::Gradient) {
					SetDiagnostic(
						diagnostic,
						Status::UnsupportedVersion,
						"structured keyframe needs imagegraph v3",
						keyframe.NodeId,
						keyframe.Port
					);
					return diagnostic.Code;
				}
		}
		if (document.FormatVersion < 6) {
			const auto requiresTransform3dTypes = [](const Value &value) {
				const ValueType type = TypeOf(value);
				return type == ValueType::Vector3 || type == ValueType::Quaternion || type == ValueType::Enum;
			};
			for (const Node &node : document.Nodes)
				for (const AuthoredValue &authored : node.Values)
					if (requiresTransform3dTypes(authored.Data)) {
						SetDiagnostic(
							diagnostic,
							Status::UnsupportedVersion,
							"3D transform value needs imagegraph v6",
							node.Id,
							authored.Port
						);
						return diagnostic.Code;
					}
		}

		std::unordered_map<std::string, size_t> nodeIndices;
		std::unordered_map<std::string, size_t> junctionIndices;
		std::unordered_set<std::string> groupIds;
		std::unordered_set<std::string> outputIds;
		for (size_t index = 0; index < document.Nodes.size(); index++) {
			const Node &node = document.Nodes[index];
			if (node.Id.empty() || node.Type.empty() || !std::isfinite(node.Position.X) ||
				!std::isfinite(node.Position.Y)) {
				SetDiagnostic(
					diagnostic, Status::InvalidValue, "node identity, type or position is invalid", node.Id
				);
				return diagnostic.Code;
			}
			if (node.Id.size() > Limits::MaximumTextBytes || node.Type.size() > Limits::MaximumTextBytes ||
				node.GroupId.size() > Limits::MaximumTextBytes) {
				SetDiagnostic(diagnostic, Status::LimitExceeded, "node text exceeds the byte limit", node.Id);
				return diagnostic.Code;
			}
			if (!nodeIndices.emplace(node.Id, index).second) {
				SetDiagnostic(diagnostic, Status::DuplicateId, "duplicate node id", node.Id);
				return diagnostic.Code;
			}
			if (!IsNodeType(node.Type)) {
				SetDiagnostic(diagnostic, Status::UnknownNode, "node type is not registered", node.Id);
				return diagnostic.Code;
			}
			if (node.Values.size() > Limits::MaximumPropertiesPerNode) {
				SetDiagnostic(
					diagnostic, Status::LimitExceeded, "node exceeds the authored property limit", node.Id
				);
				return diagnostic.Code;
			}
			if (node.DynamicInputs.size() > Limits::MaximumDynamicInputsPerNode) {
				SetDiagnostic(
					diagnostic, Status::LimitExceeded, "node exceeds the dynamic input limit", node.Id
				);
				return diagnostic.Code;
			}
			if (!node.DynamicInputs.empty() &&
				(document.FormatVersion < 2 || !FindSchema(node.Type)->DynamicInputs)) {
				SetDiagnostic(
					diagnostic,
					Status::InvalidValue,
					"node type does not declare dynamic inputs",
					node.Id,
					node.DynamicInputs.front().Id
				);
				return diagnostic.Code;
			}
			std::unordered_set<std::string> portIds;
			for (const PortSchema &port : FindSchema(node.Type)->Ports)
				portIds.insert(std::string(port.Id));
			for (const DynamicInput &input : node.DynamicInputs) {
				if (TypeName(input.Type).empty()) {
					SetDiagnostic(
						diagnostic, Status::InvalidValue, "dynamic input type is invalid", node.Id, input.Id
					);
					return diagnostic.Code;
				}
				if (input.Id.empty() || !portIds.insert(input.Id).second) {
					SetDiagnostic(
						diagnostic,
						Status::DuplicateId,
						"dynamic input id is empty or duplicated",
						node.Id,
						input.Id
					);
					return diagnostic.Code;
				}
				if (input.Id.size() > Limits::MaximumTextBytes) {
					SetDiagnostic(
						diagnostic,
						Status::LimitExceeded,
						"dynamic input id exceeds the byte limit",
						node.Id,
						input.Id
					);
					return diagnostic.Code;
				}
				if (input.Default) {
					if (!WithinValueBudget(*input.Default)) {
						SetDiagnostic(
							diagnostic,
							Status::LimitExceeded,
							"dynamic input default exceeds its limit",
							node.Id,
							input.Id
						);
						return diagnostic.Code;
					}
					if (TypeOf(*input.Default) != input.Type) {
						SetDiagnostic(
							diagnostic,
							Status::TypeMismatch,
							"dynamic input default has the wrong type",
							node.Id,
							input.Id
						);
						return diagnostic.Code;
					}
					const auto *array = std::get_if<ArrayValue>(&*input.Default);
					if (!IsFinite(*input.Default) || (array && !ValidArray(*array))) {
						SetDiagnostic(
							diagnostic,
							Status::InvalidValue,
							"dynamic input default is invalid",
							node.Id,
							input.Id
						);
						return diagnostic.Code;
					}
				}
			}
			std::unordered_set<std::string> valueIds;
			for (const AuthoredValue &value : node.Values) {
				if (value.Port.size() > Limits::MaximumTextBytes) {
					SetDiagnostic(
						diagnostic, Status::LimitExceeded, "property name exceeds the byte limit", node.Id
					);
					return diagnostic.Code;
				}
				if (!WithinValueBudget(value.Data)) {
					SetDiagnostic(
						diagnostic,
						Status::LimitExceeded,
						"authored value exceeds its limit",
						node.Id,
						value.Port
					);
					return diagnostic.Code;
				}
				const PropertySchema *property = FindProperty(node.Type, value.Port);
				if (!property) {
					SetDiagnostic(
						diagnostic, Status::UnknownPort, "unknown authored property", node.Id, value.Port
					);
					return diagnostic.Code;
				}
				if (!valueIds.insert(value.Port).second) {
					SetDiagnostic(
						diagnostic, Status::DuplicateId, "duplicate authored property", node.Id, value.Port
					);
					return diagnostic.Code;
				}
				if (TypeOf(value.Data) != property->Type) {
					SetDiagnostic(
						diagnostic,
						Status::TypeMismatch,
						"authored property has the wrong value type",
						node.Id,
						value.Port
					);
					return diagnostic.Code;
				}
				if (!IsFinite(value.Data)) {
					SetDiagnostic(
						diagnostic,
						Status::InvalidValue,
						"authored numeric value must be finite",
						node.Id,
						value.Port
					);
					return diagnostic.Code;
				}
				if (const auto *array = std::get_if<ArrayValue>(&value.Data); array && !ValidArray(*array)) {
					SetDiagnostic(
						diagnostic, Status::InvalidValue, "authored array is invalid", node.Id, value.Port
					);
					return diagnostic.Code;
				}
			}
			if (node.Type == "image.posterize") {
				const AuthoredValue *paletteValue = FindValue(node, "palette");
				const AuthoredValue *usePaletteValue = FindValue(node, "use_palette");
				const bool usePalette = usePaletteValue ? std::get<bool>(usePaletteValue->Data) : true;
				if (paletteValue) {
					const auto *palette = std::get_if<ArrayValue>(&paletteValue->Data);
					if (palette == nullptr || palette->ElementType != ValueType::Colour) {
						SetDiagnostic(
							diagnostic,
							Status::TypeMismatch,
							"posterize palette must be an array of colours",
							node.Id,
							"palette"
						);
						return diagnostic.Code;
					}
					if (palette->Elements.empty()) {
						SetDiagnostic(
							diagnostic,
							Status::InvalidValue,
							"posterize palette must contain at least one colour",
							node.Id,
							"palette"
						);
						return diagnostic.Code;
					}
					if (palette->Elements.size() > Limits::MaximumPaletteEntries) {
						SetDiagnostic(
							diagnostic,
							Status::LimitExceeded,
							"posterize palette exceeds the colour limit",
							node.Id,
							"palette"
						);
						return diagnostic.Code;
					}
				}
				if (usePalette && paletteValue == nullptr) {
					SetDiagnostic(
						diagnostic,
						Status::InvalidValue,
						"palette posterize requires an authored colour palette",
						node.Id,
						"palette"
					);
					return diagnostic.Code;
				}
			}
			if (node.Type == "image.solid" && (!valueIds.contains("width") || !valueIds.contains("height") ||
											   !valueIds.contains("colour"))) {
				SetDiagnostic(
					diagnostic, Status::InvalidValue, "solid node requires width, height and colour", node.Id
				);
				return diagnostic.Code;
			}
			if (node.Type == "image.solid") {
				const auto *width = std::get_if<int64_t>(&FindValue(node, "width")->Data);
				const auto *height = std::get_if<int64_t>(&FindValue(node, "height")->Data);
				if (!width || !height || *width < 1 || *height < 1 || *width > Limits::MaximumDimension ||
					*height > Limits::MaximumDimension) {
					SetDiagnostic(
						diagnostic,
						Status::LimitExceeded,
						"solid dimensions are outside the supported range",
						node.Id
					);
					return diagnostic.Code;
				}
				const uint64_t bytes = static_cast<uint64_t>(*width) * static_cast<uint64_t>(*height) * 4;
				if (bytes > Limits::MaximumOutputBytes) {
					SetDiagnostic(
						diagnostic, Status::LimitExceeded, "solid output exceeds the byte budget", node.Id
					);
					return diagnostic.Code;
				}
			}
			if (node.Type == "image.checker") {
				const AuthoredValue *widthValue = FindValue(node, "width");
				const AuthoredValue *heightValue = FindValue(node, "height");
				if (!widthValue || !heightValue) {
					SetDiagnostic(diagnostic, Status::InvalidValue, "checker requires dimensions", node.Id);
					return diagnostic.Code;
				}
				const int64_t width = std::get<int64_t>(widthValue->Data);
				const int64_t height = std::get<int64_t>(heightValue->Data);
				if (width < 1 || height < 1 || width > Limits::MaximumDimension ||
					height > Limits::MaximumDimension ||
					uint64_t(width) * uint64_t(height) * 4 > Limits::MaximumOutputBytes) {
					SetDiagnostic(
						diagnostic, Status::LimitExceeded, "checker dimensions exceed native limits", node.Id
					);
					return diagnostic.Code;
				}
			}
			if (node.Type == "image.gradient") {
				const AuthoredValue *widthValue = FindValue(node, "width");
				const AuthoredValue *heightValue = FindValue(node, "height");
				const AuthoredValue *gradientValue = FindValue(node, "gradient");
				if (!widthValue || !heightValue || !gradientValue) {
					SetDiagnostic(
						diagnostic,
						Status::InvalidValue,
						"gradient requires width, height and gradient",
						node.Id
					);
					return diagnostic.Code;
				}
				const int64_t width = std::get<int64_t>(widthValue->Data);
				const int64_t height = std::get<int64_t>(heightValue->Data);
				if (width < 1 || height < 1 || width > Limits::MaximumDimension ||
					height > Limits::MaximumDimension ||
					static_cast<uint64_t>(width) * height * 4 > Limits::MaximumOutputBytes) {
					SetDiagnostic(
						diagnostic, Status::LimitExceeded, "gradient dimensions exceed native limits", node.Id
					);
					return diagnostic.Code;
				}
				const auto integer = [&](std::string_view port, int64_t fallback) {
					const AuthoredValue *value = FindValue(node, port);
					return value ? std::get<int64_t>(value->Data) : fallback;
				};
				if (integer("type", 0) < 0 || integer("type", 0) > 3 || integer("loop", 0) < 0 ||
					integer("loop", 0) > 2) {
					SetDiagnostic(
						diagnostic,
						Status::InvalidValue,
						"gradient type or loop is outside its range",
						node.Id
					);
					return diagnostic.Code;
				}
			}
			if (node.Type == "image.noise_simplex") {
				const AuthoredValue *widthValue = FindValue(node, "width");
				const AuthoredValue *heightValue = FindValue(node, "height");
				if (!widthValue || !heightValue) {
					SetDiagnostic(
						diagnostic, Status::InvalidValue, "simplex requires width and height", node.Id
					);
					return diagnostic.Code;
				}
				const int64_t width = std::get<int64_t>(widthValue->Data);
				const int64_t height = std::get<int64_t>(heightValue->Data);
				if (width < 1 || height < 1 || width > Limits::MaximumDimension ||
					height > Limits::MaximumDimension ||
					static_cast<uint64_t>(width) * height * 4 > Limits::MaximumOutputBytes) {
					SetDiagnostic(
						diagnostic, Status::LimitExceeded, "simplex dimensions exceed native limits", node.Id
					);
					return diagnostic.Code;
				}
				const AuthoredValue *iterationsValue = FindValue(node, "iterations");
				const int64_t iterations = iterationsValue ? std::get<int64_t>(iterationsValue->Data) : 1;
				const AuthoredValue *modeValue = FindValue(node, "color_mode");
				const int64_t mode = modeValue ? std::get<int64_t>(modeValue->Data) : 0;
				if (iterations < 1 || iterations > 16 || mode < 0 || mode > 2) {
					SetDiagnostic(
						diagnostic,
						Status::InvalidValue,
						"simplex mode or iterations are outside range",
						node.Id
					);
					return diagnostic.Code;
				}
			}
			if (node.Type == "image.tile") {
				const auto integer = [&](std::string_view port, int64_t fallback) {
					const AuthoredValue *value = FindValue(node, port);
					return value ? std::get<int64_t>(value->Data) : fallback;
				};
				if (integer("scaling_type", 0) < 0 || integer("scaling_type", 0) > 1 ||
					integer("shift_axis", 0) < 0 || integer("shift_axis", 0) > 1 ||
					integer("pattern", 0) < 0 || integer("pattern", 0) > 2) {
					SetDiagnostic(
						diagnostic, Status::InvalidValue, "tile mode is outside its range", node.Id
					);
					return diagnostic.Code;
				}
				if (integer("scaling_type", 0) == 0 &&
					(!FindValue(node, "width") || !FindValue(node, "height"))) {
					SetDiagnostic(
						diagnostic, Status::InvalidValue, "fixed tile dimensions are missing", node.Id
					);
					return diagnostic.Code;
				}
			}
			if (node.Type == "image.height_blend") {
				const AuthoredValue *modeValue = FindValue(node, "mode");
				const AuthoredValue *typeValue = FindValue(node, "type");
				const AuthoredValue *factorValue = FindValue(node, "factor");
				const int64_t mode = modeValue ? std::get<int64_t>(modeValue->Data) : 0;
				const int64_t type = typeValue ? std::get<int64_t>(typeValue->Data) : 1;
				const double factor = factorValue ? std::get<double>(factorValue->Data) : 0.5;
				if (mode < 0 || mode > 1 || type < 0 || type > 5 || factor < 0.0 || factor > 1.0) {
					SetDiagnostic(
						diagnostic,
						Status::InvalidValue,
						"height blend mode, type or factor is outside its range",
						node.Id
					);
					return diagnostic.Code;
				}
			}
			if (node.Type == "image.blend") {
				const auto integerValue = [&](std::string_view port, int64_t fallback) {
					const AuthoredValue *value = FindValue(node, port);
					return value ? std::get<int64_t>(value->Data) : fallback;
				};
				const auto scalarValue = [&](std::string_view port, double fallback) {
					const AuthoredValue *value = FindValue(node, port);
					return value ? std::get<double>(value->Data) : fallback;
				};
				const int64_t mode = integerValue("blend_mode", 0);
				const int64_t dimension = integerValue("output_dimension", 0);
				const int64_t fill = integerValue("fill_mode", 0);
				const int64_t horizontal = integerValue("horizontal_align", 0);
				const int64_t vertical = integerValue("vertical_align", 0);
				const double opacity = scalarValue("opacity", 1.0);
				const double feather = scalarValue("mask_feather", 1.0);
				if (!detail::SupportedBlendMode(mode) || dimension < 0 || dimension > 4 || fill < 0 ||
					fill > 2 || horizontal < 0 || horizontal > 2 || vertical < 0 || vertical > 2 ||
					opacity < 0.0 || opacity > 1.0 || feather < 1.0 || feather > 16.0) {
					SetDiagnostic(
						diagnostic, Status::InvalidValue, "blend control is outside its range", node.Id
					);
					return diagnostic.Code;
				}
				const AuthoredValue *positionValue = FindValue(node, "position");
				const Vector2 position =
					positionValue ? std::get<Vector2>(positionValue->Data) : Vector2{0.5, 0.5};
				if (std::abs(position.X) > Limits::MaximumDimension ||
					std::abs(position.Y) > Limits::MaximumDimension) {
					SetDiagnostic(
						diagnostic,
						Status::LimitExceeded,
						"blend position exceeds native range",
						node.Id,
						"position"
					);
					return diagnostic.Code;
				}
				if (dimension == 4) {
					const AuthoredValue *constantValue = FindValue(node, "constant_dimension");
					if (!constantValue) {
						SetDiagnostic(
							diagnostic,
							Status::InvalidValue,
							"constant output dimension requires an authored size",
							node.Id,
							"constant_dimension"
						);
						return diagnostic.Code;
					}
					const Vector2 size = std::get<Vector2>(constantValue->Data);
					if (size.X < 1.0 || size.Y < 1.0 || size.X > Limits::MaximumDimension ||
						size.Y > Limits::MaximumDimension || std::trunc(size.X) != size.X ||
						std::trunc(size.Y) != size.Y) {
						SetDiagnostic(
							diagnostic,
							Status::LimitExceeded,
							"constant output dimension exceeds native bounds",
							node.Id,
							"constant_dimension"
						);
						return diagnostic.Code;
					}
				}
			}
			if (node.Type == "image.flip") {
				const AuthoredValue *axisValue = FindValue(node, "axis");
				const auto *axis = axisValue ? std::get_if<int64_t>(&axisValue->Data) : nullptr;
				if (!axis || *axis < 1 || *axis > 3) {
					SetDiagnostic(
						diagnostic, Status::InvalidValue, "flip axis must be 1, 2 or 3", node.Id, "axis"
					);
					return diagnostic.Code;
				}
			}
			if (node.Type == "image.invert" && !valueIds.contains("include_alpha")) {
				SetDiagnostic(
					diagnostic,
					Status::InvalidValue,
					"invert requires include_alpha",
					node.Id,
					"include_alpha"
				);
				return diagnostic.Code;
			}
			if (node.Type == "image.invert" || node.Type == "image.flip") {
				if (const AuthoredValue *channelValue = FindValue(node, "channel")) {
					const int64_t channel = std::get<int64_t>(channelValue->Data);
					if (channel < 0 || channel > 15) {
						SetDiagnostic(
							diagnostic,
							Status::InvalidValue,
							"channel bits must be in [0, 15]",
							node.Id,
							"channel"
						);
						return diagnostic.Code;
					}
				}
			}
			if (node.Type == "image.invert" || node.Type == "image.alpha_cutoff" ||
				node.Type == "image.flip") {
				if (const AuthoredValue *mixValue = FindValue(node, "mix")) {
					const double mix = std::get<double>(mixValue->Data);
					if (mix < 0.0 || mix > 1.0) {
						SetDiagnostic(
							diagnostic, Status::InvalidValue, "mix must be in [0, 1]", node.Id, "mix"
						);
						return diagnostic.Code;
					}
				}
				if (const AuthoredValue *featherValue = FindValue(node, "mask_feather")) {
					const double feather = std::get<double>(featherValue->Data);
					if (feather < 0.0 || feather > 32.0) {
						SetDiagnostic(
							diagnostic,
							Status::InvalidValue,
							"mask_feather must be in [0, 32]",
							node.Id,
							"mask_feather"
						);
						return diagnostic.Code;
					}
				}
			}
			if (node.Type == "image.alpha_cutoff") {
				const AuthoredValue *minimumValue = FindValue(node, "minimum");
				const auto *minimum = minimumValue ? std::get_if<double>(&minimumValue->Data) : nullptr;
				if (!minimum || *minimum < 0.0 || *minimum > 1.0) {
					SetDiagnostic(
						diagnostic, Status::InvalidValue, "minimum must be in [0, 1]", node.Id, "minimum"
					);
					return diagnostic.Code;
				}
			}
			if (node.Type == "image.offset" || node.Type == "image.threshold" ||
				node.Type == "image.posterize") {
				const auto scalar = [&](std::string_view port, double fallback) {
					const AuthoredValue *value = FindValue(node, port);
					return value ? std::get<double>(value->Data) : fallback;
				};
				const double mix = scalar("mix", 1.0);
				const double feather = scalar("mask_feather", 0.0);
				if (mix < 0.0 || mix > 1.0 || feather < 0.0 || feather > 32.0) {
					SetDiagnostic(
						diagnostic,
						Status::InvalidValue,
						"filter mix or mask feather is outside its range",
						node.Id
					);
					return diagnostic.Code;
				}
				if (node.Type == "image.offset" &&
					(std::abs(scalar("x_offset", 0.0)) > 1.0 || std::abs(scalar("y_offset", 0.0)) > 1.0 ||
					 std::abs(scalar("angle", 0.0)) > 360000.0)) {
					SetDiagnostic(
						diagnostic, Status::InvalidValue, "offset control exceeds native range", node.Id
					);
					return diagnostic.Code;
				}
				if (node.Type == "image.threshold") {
					const auto integer = [&](std::string_view port, int64_t fallback) {
						const AuthoredValue *value = FindValue(node, port);
						return value ? std::get<int64_t>(value->Data) : fallback;
					};
					if (integer("channel", 15) < 0 || integer("channel", 15) > 15 ||
						integer("algorithm", 0) < 0 || integer("algorithm", 0) > 1 ||
						integer("adaptive_radius", 4) < 0 || integer("adaptive_radius", 4) > 32 ||
						integer("apply_to_alpha", 0) < 0 || integer("apply_to_alpha", 0) > 2 ||
						scalar("brightness_threshold", 0.5) < 0.0 ||
						scalar("brightness_threshold", 0.5) > 1.0 ||
						scalar("brightness_smoothness", 0.0) < 0.0 ||
						scalar("brightness_smoothness", 0.0) > 1.0 || scalar("alpha_threshold", 0.5) < 0.0 ||
						scalar("alpha_threshold", 0.5) > 1.0 || scalar("alpha_smoothness", 0.0) < 0.0 ||
						scalar("alpha_smoothness", 0.0) > 1.0) {
						SetDiagnostic(
							diagnostic,
							Status::InvalidValue,
							"threshold control is outside its range",
							node.Id
						);
						return diagnostic.Code;
					}
				}
				if (node.Type == "image.posterize") {
					const AuthoredValue *stepsValue = FindValue(node, "steps");
					const int64_t steps = stepsValue ? std::get<int64_t>(stepsValue->Data) : 4;
					if (steps < 2 || steps > 16 || scalar("gamma", 1.0) < 0.0 || scalar("gamma", 1.0) > 2.0) {
						SetDiagnostic(
							diagnostic,
							Status::InvalidValue,
							"posterize control is outside its range",
							node.Id
						);
						return diagnostic.Code;
					}
				}
			}
			if (node.Type == "value.array_get") {
				if (const AuthoredValue *overflowValue = FindValue(node, "overflow")) {
					const int64_t overflow = std::get<int64_t>(overflowValue->Data);
					if (overflow < 0 || overflow > 2) {
						SetDiagnostic(
							diagnostic,
							Status::InvalidValue,
							"array overflow mode must be 0, 1 or 2",
							node.Id,
							"overflow"
						);
						return diagnostic.Code;
					}
				}
			}
			if (node.Type == "image.height_blend") {
				if (const AuthoredValue *processValue = FindValue(node, "array_process")) {
					const int64_t process = std::get<int64_t>(processValue->Data);
					if (process < 0 || process > 3) {
						SetDiagnostic(
							diagnostic,
							Status::InvalidValue,
							"array process mode must be 0, 1, 2 or 3",
							node.Id,
							"array_process"
						);
						return diagnostic.Code;
					}
				}
			}
		}

		for (const Group &group : document.Groups) {
			if (group.Id.size() > Limits::MaximumTextBytes || group.Name.size() > Limits::MaximumTextBytes ||
				group.ParentId.size() > Limits::MaximumTextBytes) {
				SetDiagnostic(
					diagnostic, Status::LimitExceeded, "group text exceeds the byte limit", group.Id
				);
				return diagnostic.Code;
			}
			if (group.Id.empty() || !groupIds.insert(group.Id).second) {
				SetDiagnostic(diagnostic, Status::DuplicateId, "group id is empty or duplicated", group.Id);
				return diagnostic.Code;
			}
			if (document.FormatVersion == 1 && !group.ParentId.empty()) {
				SetDiagnostic(
					diagnostic, Status::InvalidGroup, "legacy document contains a nested group", group.Id
				);
				return diagnostic.Code;
			}
			if (group.Ports.size() > Limits::MaximumGroupPorts) {
				SetDiagnostic(diagnostic, Status::LimitExceeded, "group exceeds its port limit", group.Id);
				return diagnostic.Code;
			}
			if (document.FormatVersion == 1 && !group.Ports.empty()) {
				SetDiagnostic(
					diagnostic, Status::InvalidGroup, "legacy document contains group ports", group.Id
				);
				return diagnostic.Code;
			}
		}
		for (const Group &group : document.Groups) {
			if (group.ParentId.empty()) continue;
			if (group.ParentId == group.Id || !groupIds.contains(group.ParentId)) {
				SetDiagnostic(
					diagnostic,
					Status::InvalidGroup,
					"group parent is missing or self-referential",
					group.Id,
					group.ParentId
				);
				return diagnostic.Code;
			}
			std::string parent = group.ParentId;
			for (size_t depth = 0; !parent.empty(); depth++) {
				if (depth >= document.Groups.size()) {
					SetDiagnostic(diagnostic, Status::Cycle, "group hierarchy contains a cycle", group.Id);
					return diagnostic.Code;
				}
				const auto ancestor =
					std::find_if(document.Groups.begin(), document.Groups.end(), [&](const Group &candidate) {
						return candidate.Id == parent;
					});
				parent = ancestor->ParentId;
			}
		}
		for (size_t index = 0; index < document.Junctions.size(); index++) {
			const Junction &junction = document.Junctions[index];
			if (document.FormatVersion == 1) {
				SetDiagnostic(
					diagnostic, Status::InvalidValue, "legacy document contains v2 junctions", junction.Id
				);
				return diagnostic.Code;
			}
			if (junction.Id.size() > Limits::MaximumTextBytes ||
				junction.GroupId.size() > Limits::MaximumTextBytes) {
				SetDiagnostic(
					diagnostic, Status::LimitExceeded, "junction text exceeds the byte limit", junction.Id
				);
				return diagnostic.Code;
			}
			if (junction.Id.empty() || nodeIndices.contains(junction.Id) ||
				!junctionIndices.emplace(junction.Id, index).second) {
				SetDiagnostic(
					diagnostic, Status::DuplicateId, "junction id is empty or duplicated", junction.Id
				);
				return diagnostic.Code;
			}
			if (TypeName(junction.Type).empty()) {
				SetDiagnostic(
					diagnostic, Status::InvalidValue, "junction type is invalid", junction.Id, "value"
				);
				return diagnostic.Code;
			}
			if (!junction.GroupId.empty() && !groupIds.contains(junction.GroupId)) {
				SetDiagnostic(
					diagnostic,
					Status::InvalidGroup,
					"junction refers to an unknown group",
					junction.Id,
					junction.GroupId
				);
				return diagnostic.Code;
			}
			if (junction.Default) {
				if (!WithinValueBudget(*junction.Default)) {
					SetDiagnostic(
						diagnostic,
						Status::LimitExceeded,
						"junction default exceeds its limit",
						junction.Id,
						"value"
					);
					return diagnostic.Code;
				}
				if (TypeOf(*junction.Default) != junction.Type) {
					SetDiagnostic(
						diagnostic,
						Status::TypeMismatch,
						"junction default has the wrong type",
						junction.Id,
						"value"
					);
					return diagnostic.Code;
				}
				const auto *array = std::get_if<ArrayValue>(&*junction.Default);
				if (!IsFinite(*junction.Default) || (array && !ValidArray(*array))) {
					SetDiagnostic(
						diagnostic, Status::InvalidValue, "junction default is invalid", junction.Id, "value"
					);
					return diagnostic.Code;
				}
			}
		}
		for (const Group &group : document.Groups) {
			std::unordered_set<std::string> portIds;
			std::unordered_set<std::string> routedJunctions;
			for (const GroupPort &port : group.Ports) {
				if (port.Direction != PortDirection::Input && port.Direction != PortDirection::Output) {
					SetDiagnostic(
						diagnostic, Status::InvalidValue, "group port direction is invalid", group.Id, port.Id
					);
					return diagnostic.Code;
				}
				if (port.Id.size() > Limits::MaximumTextBytes ||
					port.JunctionId.size() > Limits::MaximumTextBytes) {
					SetDiagnostic(
						diagnostic,
						Status::LimitExceeded,
						"group port text exceeds the byte limit",
						group.Id,
						port.Id
					);
					return diagnostic.Code;
				}
				if (port.Id.empty() || !portIds.insert(port.Id).second) {
					SetDiagnostic(
						diagnostic,
						Status::DuplicateId,
						"group port id is empty or duplicated",
						group.Id,
						port.Id
					);
					return diagnostic.Code;
				}
				const auto junction = junctionIndices.find(port.JunctionId);
				if (junction == junctionIndices.end() ||
					document.Junctions[junction->second].GroupId != group.Id) {
					SetDiagnostic(
						diagnostic,
						Status::InvalidGroup,
						"group port must route through its own junction",
						group.Id,
						port.Id
					);
					return diagnostic.Code;
				}
				if (!routedJunctions.insert(port.JunctionId).second) {
					SetDiagnostic(
						diagnostic,
						Status::DuplicateId,
						"junction is exposed by more than one group port",
						group.Id,
						port.Id
					);
					return diagnostic.Code;
				}
			}
		}
		for (const Node &node : document.Nodes) {
			if (!node.GroupId.empty() && !groupIds.contains(node.GroupId)) {
				SetDiagnostic(
					diagnostic, Status::InvalidGroup, "node refers to an unknown group", node.Id, node.GroupId
				);
				return diagnostic.Code;
			}
		}
		std::set<std::tuple<std::string, std::string, uint64_t>> keyframeIds;
		for (const Keyframe &keyframe : document.Keyframes) {
			if (keyframe.Tick > Limits::MaximumTick) {
				SetDiagnostic(
					diagnostic,
					Status::LimitExceeded,
					"keyframe tick exceeds the fixed timeline limit",
					keyframe.NodeId,
					keyframe.Port
				);
				return diagnostic.Code;
			}
			if (keyframe.NodeId.size() > Limits::MaximumTextBytes ||
				keyframe.Port.size() > Limits::MaximumTextBytes ||
				keyframe.Interpolation.size() > Limits::MaximumTextBytes) {
				SetDiagnostic(
					diagnostic,
					Status::LimitExceeded,
					"keyframe text exceeds the byte limit",
					keyframe.NodeId,
					keyframe.Port
				);
				return diagnostic.Code;
			}
			if (!WithinValueBudget(keyframe.Data)) {
				SetDiagnostic(
					diagnostic,
					Status::LimitExceeded,
					"keyframe value exceeds its limit",
					keyframe.NodeId,
					keyframe.Port
				);
				return diagnostic.Code;
			}
			const auto node = nodeIndices.find(keyframe.NodeId);
			if (node == nodeIndices.end()) {
				SetDiagnostic(
					diagnostic,
					Status::InvalidValue,
					"keyframe refers to an unknown node",
					keyframe.NodeId,
					keyframe.Port
				);
				return diagnostic.Code;
			}
			const PropertySchema *property = FindProperty(document.Nodes[node->second].Type, keyframe.Port);
			if (!property) {
				SetDiagnostic(
					diagnostic,
					Status::UnknownPort,
					"keyframe refers to an unknown property",
					keyframe.NodeId,
					keyframe.Port
				);
				return diagnostic.Code;
			}
			if (TypeOf(keyframe.Data) != property->Type) {
				SetDiagnostic(
					diagnostic,
					Status::TypeMismatch,
					"keyframe has the wrong value type",
					keyframe.NodeId,
					keyframe.Port
				);
				return diagnostic.Code;
			}
			if (!IsFinite(keyframe.Data)) {
				SetDiagnostic(
					diagnostic,
					Status::InvalidValue,
					"keyframe value must be finite",
					keyframe.NodeId,
					keyframe.Port
				);
				return diagnostic.Code;
			}
			if (keyframe.Interpolation != "step" && keyframe.Interpolation != "linear" &&
				keyframe.Interpolation != "cubic" &&
				!(document.FormatVersion >= 4 && keyframe.Interpolation == "source")) {
				SetDiagnostic(
					diagnostic,
					Status::InvalidValue,
					"keyframe interpolation is not registered",
					keyframe.NodeId,
					keyframe.Port
				);
				return diagnostic.Code;
			}
			if ((keyframe.Interpolation == "source") != keyframe.Ease.has_value()) {
				SetDiagnostic(
					diagnostic,
					Status::InvalidValue,
					"source keyframe needs side easing",
					keyframe.NodeId,
					keyframe.Port
				);
				return diagnostic.Code;
			}
			if (keyframe.Ease) {
				const KeyframeEase &ease = *keyframe.Ease;
				const auto validSide = [](const std::string &type) {
					return type == "linear" || type == "bezier" || type == "cut";
				};
				if (!validSide(ease.InType) || !validSide(ease.OutType) || !std::isfinite(ease.In.X) ||
					!std::isfinite(ease.In.Y) || !std::isfinite(ease.Out.X) || !std::isfinite(ease.Out.Y)) {
					SetDiagnostic(
						diagnostic,
						Status::InvalidValue,
						"keyframe easing must be finite and registered",
						keyframe.NodeId,
						keyframe.Port
					);
					return diagnostic.Code;
				}
			}
			if (keyframe.SineDriver) {
				const KeyframeSineDriver &driver = *keyframe.SineDriver;
				if (!std::holds_alternative<double>(keyframe.Data)) {
					SetDiagnostic(
						diagnostic,
						Status::TypeMismatch,
						"sine keyframe driver requires a scalar value",
						keyframe.NodeId,
						keyframe.Port
					);
					return diagnostic.Code;
				}
				if (!std::isfinite(driver.Frequency) || !std::isfinite(driver.Amplitude) ||
					!std::isfinite(driver.Phase) || !std::isfinite(driver.Smooth) || driver.Smooth < 0.0 ||
					driver.Smooth > 1.0) {
					SetDiagnostic(
						diagnostic,
						Status::InvalidValue,
						"sine keyframe driver parameters must be finite and smooth must be in [0, 1]",
						keyframe.NodeId,
						keyframe.Port
					);
					return diagnostic.Code;
				}
			}
			if (!keyframeIds.emplace(keyframe.NodeId, keyframe.Port, keyframe.Tick).second) {
				SetDiagnostic(
					diagnostic, Status::DuplicateId, "duplicate keyframe tick", keyframe.NodeId, keyframe.Port
				);
				return diagnostic.Code;
			}
		}
		std::map<std::pair<std::string, std::string>, std::vector<const Keyframe *>> authoredTracks;
		for (const Keyframe &keyframe : document.Keyframes)
			authoredTracks[{keyframe.NodeId, keyframe.Port}].push_back(&keyframe);
		for (const auto &[identity, keys] : authoredTracks) {
			const bool source = keys.front()->Interpolation == "source";
			if (std::any_of(keys.begin(), keys.end(), [source](const Keyframe *key) {
					return (key->Interpolation == "source") != source;
				})) {
				SetDiagnostic(
					diagnostic,
					Status::InvalidValue,
					"keyframe track mixes legacy and side easing",
					identity.first,
					identity.second
				);
				return diagnostic.Code;
			}
		}
		std::set<std::pair<std::string, std::string>> trackIds;
		for (const AnimationTrack &track : document.Tracks) {
			const auto identity = std::pair{track.NodeId, track.Port};
			if (!trackIds.insert(identity).second) {
				SetDiagnostic(
					diagnostic, Status::DuplicateId, "duplicate animation track", track.NodeId, track.Port
				);
				return diagnostic.Code;
			}
			const auto authored = authoredTracks.find(identity);
			if (authored == authoredTracks.end()) {
				SetDiagnostic(
					diagnostic,
					Status::InvalidValue,
					"animation track has no keyframes",
					track.NodeId,
					track.Port
				);
				return diagnostic.Code;
			}
			if (track.NodeId.size() > Limits::MaximumTextBytes ||
				track.Port.size() > Limits::MaximumTextBytes || track.End.size() > Limits::MaximumTextBytes ||
				(track.End != "hold" && track.End != "loop" && track.End != "ping" && track.End != "wrap") ||
				track.LoopRange < -1 || track.LoopRange >= static_cast<int64_t>(authored->second.size())) {
				SetDiagnostic(
					diagnostic,
					Status::InvalidValue,
					"animation track has invalid end or loop range",
					track.NodeId,
					track.Port
				);
				return diagnostic.Code;
			}
			if (track.End == "wrap" && !document.Timeline) {
				SetDiagnostic(
					diagnostic,
					Status::InvalidValue,
					"wrap track needs timeline frame count",
					track.NodeId,
					track.Port
				);
				return diagnostic.Code;
			}
		}
		for (const auto &[identity, keys] : authoredTracks) {
			if (keys.front()->Interpolation == "source" && !trackIds.contains(identity)) {
				SetDiagnostic(
					diagnostic,
					Status::InvalidValue,
					"source keyframes need an animation track",
					identity.first,
					identity.second
				);
				return diagnostic.Code;
			}
		}

		std::vector<size_t> indegree(document.Nodes.size(), 0);
		std::vector<std::vector<size_t>> downstream(document.Nodes.size());
		std::set<std::tuple<std::string, std::string, std::string, std::string>> uniqueLinks;
		std::unordered_set<std::string> linkedInputs;
		std::unordered_map<std::string, const Link *> junctionInputs;
		for (const Link &link : document.Links) {
			if (link.FromNode.size() > Limits::MaximumTextBytes ||
				link.FromPort.size() > Limits::MaximumTextBytes ||
				link.ToNode.size() > Limits::MaximumTextBytes ||
				link.ToPort.size() > Limits::MaximumTextBytes) {
				SetDiagnostic(
					diagnostic,
					Status::LimitExceeded,
					"link text exceeds the byte limit",
					link.ToNode,
					link.ToPort
				);
				return diagnostic.Code;
			}
			const auto key = std::make_tuple(link.FromNode, link.FromPort, link.ToNode, link.ToPort);
			if (!uniqueLinks.insert(key).second) {
				SetDiagnostic(
					diagnostic, Status::DuplicateLink, "duplicate graph link", link.ToNode, link.ToPort
				);
				return diagnostic.Code;
			}
			const auto from = nodeIndices.find(link.FromNode);
			const auto to = nodeIndices.find(link.ToNode);
			const auto fromJunction = junctionIndices.find(link.FromNode);
			const auto toJunction = junctionIndices.find(link.ToNode);
			if ((from == nodeIndices.end() && fromJunction == junctionIndices.end()) ||
				(to == nodeIndices.end() && toJunction == junctionIndices.end())) {
				SetDiagnostic(
					diagnostic,
					Status::InvalidOutput,
					"link refers to an unknown node or junction",
					link.FromNode,
					link.FromPort
				);
				return diagnostic.Code;
			}
			const std::string &sourceGroup = from != nodeIndices.end()
												 ? document.Nodes[from->second].GroupId
												 : document.Junctions[fromJunction->second].GroupId;
			const std::string &targetGroup = to != nodeIndices.end()
												 ? document.Nodes[to->second].GroupId
												 : document.Junctions[toJunction->second].GroupId;
			if (sourceGroup != targetGroup && document.FormatVersion >= 2) {
				const auto sourceOwner =
					std::find_if(document.Groups.begin(), document.Groups.end(), [&](const Group &group) {
						return group.Id == sourceGroup;
					});
				const auto targetOwner =
					std::find_if(document.Groups.begin(), document.Groups.end(), [&](const Group &group) {
						return group.Id == targetGroup;
					});
				const bool declaredBoundary =
					(sourceOwner != document.Groups.end() && !sourceOwner->Ports.empty()) ||
					(targetOwner != document.Groups.end() && !targetOwner->Ports.empty());
				if (declaredBoundary) {
					const bool leavesChild =
						sourceOwner != document.Groups.end() && sourceOwner->ParentId == targetGroup &&
						fromJunction != junctionIndices.end() &&
						std::any_of(
							sourceOwner->Ports.begin(), sourceOwner->Ports.end(), [&](const GroupPort &port) {
								return port.JunctionId == link.FromNode &&
									   port.Direction == PortDirection::Output;
							}
						);
					const bool entersChild =
						targetOwner != document.Groups.end() && targetOwner->ParentId == sourceGroup &&
						toJunction != junctionIndices.end() &&
						std::any_of(
							targetOwner->Ports.begin(), targetOwner->Ports.end(), [&](const GroupPort &port) {
								return port.JunctionId == link.ToNode &&
									   port.Direction == PortDirection::Input;
							}
						);
					if (!leavesChild && !entersChild) {
						SetDiagnostic(
							diagnostic,
							Status::InvalidGroup,
							"link crosses a subgraph without an interface port",
							link.ToNode,
							link.ToPort
						);
						return diagnostic.Code;
					}
				}
			}
			std::optional<ValueType> source;
			if (from != nodeIndices.end())
				source = FindPortType(document.Nodes[from->second], link.FromPort, PortDirection::Output);
			else if (link.FromPort == "value")
				source = document.Junctions[fromJunction->second].Type;
			std::optional<ValueType> target;
			if (to != nodeIndices.end())
				target = FindPortType(document.Nodes[to->second], link.ToPort, PortDirection::Input);
			else if (link.ToPort == "value")
				target = document.Junctions[toJunction->second].Type;
			if (!source || !target) {
				SetDiagnostic(
					diagnostic,
					Status::UnknownPort,
					"link refers to an unknown port",
					link.ToNode,
					link.ToPort
				);
				return diagnostic.Code;
			}
			const ValueType sourceType = source.value_or(ValueType::Boolean);
			const ValueType targetType = target.value_or(ValueType::Boolean);
			const bool arrayElement = from != nodeIndices.end() && to != nodeIndices.end() &&
									  sourceType == ValueType::Array && targetType == ValueType::Image &&
									  document.Nodes[to->second].Type == "value.array";
			const bool heightBlendArrayInput = to != nodeIndices.end() && sourceType == ValueType::Array &&
											   targetType == ValueType::Image &&
											   document.Nodes[to->second].Type == "image.height_blend";
			const bool heightBlendArrayOutput = from != nodeIndices.end() && sourceType == ValueType::Image &&
												targetType == ValueType::Array &&
												document.Nodes[from->second].Type == "image.height_blend";
			if (sourceType != targetType && !arrayElement && !heightBlendArrayInput &&
				!heightBlendArrayOutput) {
				SetDiagnostic(
					diagnostic,
					Status::TypeMismatch,
					"link connects incompatible port types",
					link.ToNode,
					link.ToPort
				);
				return diagnostic.Code;
			}
			const std::string inputKey = link.ToNode + "\n" + link.ToPort;
			if (!linkedInputs.insert(inputKey).second) {
				SetDiagnostic(
					diagnostic,
					Status::DuplicateLink,
					"input port has more than one link",
					link.ToNode,
					link.ToPort
				);
				return diagnostic.Code;
			}
			if (toJunction != junctionIndices.end()) junctionInputs.emplace(link.ToNode, &link);
		}
		std::vector<Link> effectiveLinks;
		std::vector<ResolvedInput> resolvedInputs;
		for (const Link &link : document.Links) {
			if (!nodeIndices.contains(link.ToNode)) continue;
			Link effective = link;
			std::unordered_set<std::string> visited;
			bool usedDefault = false;
			while (junctionIndices.contains(effective.FromNode)) {
				if (!visited.insert(effective.FromNode).second) {
					SetDiagnostic(
						diagnostic,
						Status::Cycle,
						"junction routing contains a cycle",
						effective.FromNode,
						"value"
					);
					return diagnostic.Code;
				}
				const auto input = junctionInputs.find(effective.FromNode);
				if (input == junctionInputs.end()) {
					const Junction &junction = document.Junctions[junctionIndices.at(effective.FromNode)];
					if (!junction.Default) {
						SetDiagnostic(
							diagnostic,
							Status::InvalidValue,
							"junction route has no source or default",
							junction.Id,
							"value"
						);
						return diagnostic.Code;
					}
					resolvedInputs.push_back({link.ToNode, link.ToPort, *junction.Default});
					usedDefault = true;
					break;
				}
				effective.FromNode = input->second->FromNode;
				effective.FromPort = input->second->FromPort;
			}
			if (!usedDefault) effectiveLinks.push_back(std::move(effective));
		}
		for (const Junction &junction : document.Junctions) {
			std::unordered_set<std::string> visited;
			std::string current = junction.Id;
			while (junctionIndices.contains(current)) {
				if (!visited.insert(current).second) {
					SetDiagnostic(
						diagnostic, Status::Cycle, "junction routing contains a cycle", current, "value"
					);
					return diagnostic.Code;
				}
				const auto input = junctionInputs.find(current);
				if (input == junctionInputs.end()) break;
				current = input->second->FromNode;
			}
		}
		for (const Link &link : effectiveLinks) {
			const size_t from = nodeIndices.at(link.FromNode);
			const size_t to = nodeIndices.at(link.ToNode);
			indegree[to]++;
			downstream[from].push_back(to);
		}
		for (const Node &node : document.Nodes) {
			if (node.Type == "image.transform_3d" && !linkedInputs.contains(node.Id + "\nsurface")) {
				SetDiagnostic(
					diagnostic,
					Status::InvalidValue,
					"transform image 3D requires a surface input",
					node.Id,
					"surface"
				);
				return diagnostic.Code;
			}
			if ((node.Type == "image.passthrough" || node.Type == "image.flip" ||
				 node.Type == "image.invert" || node.Type == "image.alpha_cutoff" ||
				 node.Type == "image.offset" || node.Type == "image.threshold" ||
				 node.Type == "image.posterize" || node.Type == "image.tile" ||
				 node.Type == "image.vignette" || node.Type == "image.displace" ||
				 node.Type == "image.polar" || node.Type == "image.curve" || node.Type == "image.colorize") &&
				!linkedInputs.contains(node.Id + "\nimage")) {
				SetDiagnostic(
					diagnostic, Status::InvalidValue, "node requires an image input", node.Id, "image"
				);
				return diagnostic.Code;
			}
			if (node.Type == "image.displace" && !linkedInputs.contains(node.Id + "\ndisplace_map")) {
				SetDiagnostic(
					diagnostic, Status::InvalidValue, "displace requires a map input", node.Id, "displace_map"
				);
				return diagnostic.Code;
			}
			if (node.Type == "image.height_blend") {
				for (const char *port : {"background", "foreground"}) {
					if (!linkedInputs.contains(node.Id + "\n" + port)) {
						SetDiagnostic(
							diagnostic,
							Status::InvalidValue,
							"height blend requires both image inputs",
							node.Id,
							port
						);
						return diagnostic.Code;
					}
				}
			}
			if (node.Type == "image.blend" && !linkedInputs.contains(node.Id + "\nbackground")) {
				SetDiagnostic(
					diagnostic,
					Status::InvalidValue,
					"blend requires a background image",
					node.Id,
					"background"
				);
				return diagnostic.Code;
			}
			if (node.Type == "value.array_get" && !linkedInputs.contains(node.Id + "\narray")) {
				SetDiagnostic(
					diagnostic, Status::InvalidValue, "array get requires an array input", node.Id, "array"
				);
				return diagnostic.Code;
			}
			if (node.Type == "value.array") {
				if (node.DynamicInputs.empty()) {
					SetDiagnostic(
						diagnostic, Status::InvalidValue, "array requires at least one input", node.Id
					);
					return diagnostic.Code;
				}
				for (const DynamicInput &input : node.DynamicInputs) {
					const bool connected =
						std::any_of(effectiveLinks.begin(), effectiveLinks.end(), [&](const Link &link) {
							return link.ToNode == node.Id && link.ToPort == input.Id;
						});
					if ((input.Type == ValueType::Image || input.Type == ValueType::Array) && !connected) {
						SetDiagnostic(
							diagnostic,
							Status::InvalidValue,
							"image array input requires a link",
							node.Id,
							input.Id
						);
						return diagnostic.Code;
					}
				}
			}
		}

		std::vector<size_t> outputNodes;
		for (const Output &output : document.Outputs) {
			if (output.Id.size() > Limits::MaximumTextBytes ||
				output.NodeId.size() > Limits::MaximumTextBytes ||
				output.Port.size() > Limits::MaximumTextBytes) {
				SetDiagnostic(
					diagnostic,
					Status::LimitExceeded,
					"output text exceeds the byte limit",
					output.NodeId,
					output.Port
				);
				return diagnostic.Code;
			}
			if (output.Id.empty() || !outputIds.insert(output.Id).second) {
				SetDiagnostic(
					diagnostic,
					Status::DuplicateId,
					"output id is empty or duplicated",
					output.NodeId,
					output.Port
				);
				return diagnostic.Code;
			}
			const auto node = nodeIndices.find(output.NodeId);
			const PortSchema *port =
				node == nodeIndices.end()
					? nullptr
					: FindPort(document.Nodes[node->second].Type, output.Port, PortDirection::Output);
			if (!port) {
				SetDiagnostic(
					diagnostic,
					Status::InvalidOutput,
					"output must name a registered output",
					output.NodeId,
					output.Port
				);
				return diagnostic.Code;
			}
			outputNodes.push_back(node->second);
		}
		if (document.Outputs.empty()) {
			SetDiagnostic(diagnostic, Status::InvalidOutput, "document has no declared output");
			return diagnostic.Code;
		}

		std::priority_queue<size_t, std::vector<size_t>, std::greater<>> ready;
		for (size_t index = 0; index < indegree.size(); index++) {
			if (indegree[index] == 0) ready.push(index);
		}
		Plan compiled;
		compiled.OutputNodes = std::move(outputNodes);
		compiled.EffectiveLinks = std::move(effectiveLinks);
		compiled.ResolvedInputs = std::move(resolvedInputs);
		while (!ready.empty()) {
			const size_t current = ready.top();
			ready.pop();
			compiled.NodeOrder.push_back(current);
			for (const size_t next : downstream[current]) {
				if (--indegree[next] == 0) ready.push(next);
			}
		}
		if (compiled.NodeOrder.size() != document.Nodes.size()) {
			SetDiagnostic(diagnostic, Status::Cycle, "graph contains a cycle");
			return diagnostic.Code;
		}
		plan = std::move(compiled);
		diagnostic = {};
		return Status::Ok;
	}

	using ValueOutputs = std::vector<AuthoredValue>;
	struct ShapeOutputs {
		Image Colored, Mask, Height, UV;
	};
	struct ConversionOutputs {
		std::array<Image, 4> Channels;
		bool Hsv = false;
	};
	struct MirrorOutputs {
		Image Colored, Mask;
	};
	using NodeResult =
		std::variant<Image, ImageArray, ValueOutputs, ShapeOutputs, ConversionOutputs, MirrorOutputs>;
	static const Image *FindImageOutput(const NodeResult &result, std::string_view port) {
		if (const auto *image = std::get_if<Image>(&result)) return image;
		if (const auto *shape = std::get_if<ShapeOutputs>(&result)) {
			if (port == "colored") return &shape->Colored;
			if (port == "mask") return &shape->Mask;
			if (port == "height") return &shape->Height;
			if (port == "uv") return &shape->UV;
		}
		if (const auto *conversion = std::get_if<ConversionOutputs>(&result)) {
			const std::array<std::string_view, 4> names =
				conversion->Hsv ? std::array<std::string_view, 4>{"hue", "saturation", "value", "alpha"}
								: std::array<std::string_view, 4>{"red", "green", "blue", "alpha"};
			for (size_t index = 0; index < names.size(); ++index)
				if (port == names[index]) return &conversion->Channels[index];
		}
		if (const auto *mirror = std::get_if<MirrorOutputs>(&result)) {
			if (port == "image") return &mirror->Colored;
			if (port == "mirror_mask") return &mirror->Mask;
		}
		return nullptr;
	}

	static ImageArrayItem RebaseItem(const ImageArrayItem &source, size_t offset) {
		if (const auto *leaf = std::get_if<size_t>(&source.Data)) return ImageArrayItem{*leaf + offset};
		std::vector<ImageArrayItem> children;
		for (const ImageArrayItem &child : std::get<std::vector<ImageArrayItem>>(source.Data))
			children.push_back(RebaseItem(child, offset));
		return ImageArrayItem{std::move(children)};
	}

	static uint64_t ResultBytes(const ImageArray &images) {
		uint64_t bytes = 0;
		for (const Image &image : images.Images)
			bytes += image.Pixels.size();
		return bytes;
	}

	static uint64_t ResultBytes(const NodeResult &result) {
		if (const auto *image = std::get_if<Image>(&result)) return image->Pixels.size();
		if (const auto *array = std::get_if<ImageArray>(&result)) return ResultBytes(*array);
		if (const auto *shape = std::get_if<ShapeOutputs>(&result))
			return shape->Colored.Pixels.size() + shape->Mask.Pixels.size() + shape->Height.Pixels.size() +
				   shape->UV.Pixels.size();
		if (const auto *conversion = std::get_if<ConversionOutputs>(&result)) {
			uint64_t bytes = 0;
			for (const Image &image : conversion->Channels)
				bytes += image.Pixels.size();
			return bytes;
		}
		if (const auto *mirror = std::get_if<MirrorOutputs>(&result))
			return mirror->Colored.Pixels.size() + mirror->Mask.Pixels.size();
		uint64_t bytes = 0;
		for (const AuthoredValue &value : std::get<ValueOutputs>(result)) {
			bytes += sizeof(Value);
			if (const auto *text = std::get_if<std::string>(&value.Data)) bytes += text->size();
			if (const auto *array = std::get_if<ArrayValue>(&value.Data)) {
				bytes += array->Elements.size() * sizeof(ElementValue);
				for (const ElementValue &element : array->Elements)
					if (const auto *text = std::get_if<std::string>(&element)) bytes += text->size();
			}
		}
		return bytes;
	}

	static Status EvaluateGraph(
		const Document &document,
		const Plan &plan,
		const std::string &outputId,
		const EvaluationRequest &request,
		NodeResult &outputValue,
		Diagnostic &diagnostic
	) {
		// Pure image nodes do not sample the seed. Future random nodes receive this same request.
		(void)request.Seed;
		const Status captureStatus = ValidateAudioCaptureFrames(request.AudioFrames, diagnostic);
		if (captureStatus != Status::Ok) return captureStatus;
		Plan currentPlan;
		const Status compileStatus = Compile(document, currentPlan, diagnostic);
		if (compileStatus != Status::Ok) return compileStatus;
		if (currentPlan != plan) {
			SetDiagnostic(
				diagnostic, Status::InvalidOutput, "compile plan does not match the authored document"
			);
			return diagnostic.Code;
		}
		const auto output =
			std::find_if(document.Outputs.begin(), document.Outputs.end(), [&](const Output &candidate) {
				return candidate.Id == outputId;
			});
		if (output == document.Outputs.end()) {
			SetDiagnostic(diagnostic, Status::InvalidOutput, "selected output does not exist", {}, outputId);
			return diagnostic.Code;
		}
		std::unordered_map<std::string, size_t> nodeIndices;
		for (size_t index = 0; index < document.Nodes.size(); index++) {
			nodeIndices.emplace(document.Nodes[index].Id, index);
		}
		const size_t targetIndex = nodeIndices.at(output->NodeId);
		std::vector<std::vector<size_t>> upstream(document.Nodes.size());
		for (const Link &link : plan.EffectiveLinks) {
			upstream[nodeIndices.at(link.ToNode)].push_back(nodeIndices.at(link.FromNode));
		}
		std::vector<uint8_t> needed(document.Nodes.size(), 0);
		std::vector<size_t> pending{targetIndex};
		while (!pending.empty()) {
			const size_t current = pending.back();
			pending.pop_back();
			if (needed[current]) continue;
			needed[current] = 1;
			for (const size_t source : upstream[current])
				pending.push_back(source);
		}
		std::vector<size_t> remainingConsumers(document.Nodes.size(), 0);
		for (size_t index = 0; index < upstream.size(); index++) {
			if (!needed[index]) continue;
			for (const size_t source : upstream[index])
				remainingConsumers[source]++;
		}

		std::vector<NodeResult> results(document.Nodes.size());
		std::vector<uint8_t> produced(document.Nodes.size(), 0);
		uint64_t evaluationBytes = 0;
		for (const size_t index : plan.NodeOrder) {
			if (!needed[index]) continue;
			const Node &node = document.Nodes[index];
			Image result;
			ImageArray arrayResult;
			bool producedArray = false;
			ValueOutputs valueResult;
			bool producedValue = false;
			ShapeOutputs shapeResult;
			bool producedShape = false;
			ConversionOutputs conversionResult;
			bool producedConversion = false;
			MirrorOutputs mirrorResult;
			bool producedMirror = false;
			const auto resolveImage = [&](std::string_view port, bool required, const Image *&resolved) {
				resolved = nullptr;
				const auto link = std::find_if(
					plan.EffectiveLinks.begin(), plan.EffectiveLinks.end(), [&](const Link &candidate) {
						return candidate.ToNode == node.Id && candidate.ToPort == port;
					}
				);
				if (link == plan.EffectiveLinks.end()) {
					if (!required) return true;
					SetDiagnostic(
						diagnostic, Status::InvalidValue, "image input is missing", node.Id, std::string(port)
					);
					return false;
				}
				const size_t sourceIndex = nodeIndices.at(link->FromNode);
				if (!produced[sourceIndex]) {
					SetDiagnostic(
						diagnostic,
						Status::InvalidOutput,
						"image source was not evaluated",
						node.Id,
						std::string(port)
					);
					return false;
				}
				resolved = FindImageOutput(results[sourceIndex], link->FromPort);
				if (resolved == nullptr) {
					SetDiagnostic(
						diagnostic,
						Status::UnsupportedExecution,
						"image input received an array",
						node.Id,
						std::string(port)
					);
					return false;
				}
				return true;
			};
			if (detail::FindValueNodeSchema(node.Type)) {
				producedValue = true;
				std::vector<AuthoredValue> inputs = node.Values;
				const auto resolveValueInput = [&](std::string_view port, const Value *fallback) {
					const auto link = std::find_if(
						plan.EffectiveLinks.begin(), plan.EffectiveLinks.end(), [&](const Link &candidate) {
							return candidate.ToNode == node.Id && candidate.ToPort == port;
						}
					);
					const Value *value = fallback;
					if (link != plan.EffectiveLinks.end()) {
						const size_t sourceIndex = nodeIndices.at(link->FromNode);
						const ValueOutputs *source = produced[sourceIndex]
														 ? std::get_if<ValueOutputs>(&results[sourceIndex])
														 : nullptr;
						if (!source) {
							SetDiagnostic(
								diagnostic,
								Status::UnsupportedExecution,
								"value input needs a typed source",
								node.Id,
								std::string(port)
							);
							return false;
						}
						const auto found =
							std::find_if(source->begin(), source->end(), [&](const AuthoredValue &entry) {
								return entry.Port == link->FromPort;
							});
						if (found == source->end()) {
							SetDiagnostic(
								diagnostic,
								Status::InvalidOutput,
								"source did not produce the linked port",
								node.Id,
								std::string(port)
							);
							return false;
						}
						value = &found->Data;
					} else {
						const auto resolved = std::find_if(
							plan.ResolvedInputs.begin(),
							plan.ResolvedInputs.end(),
							[&](const ResolvedInput &entry) {
								return entry.NodeId == node.Id && entry.Port == port;
							}
						);
						if (resolved != plan.ResolvedInputs.end()) value = &resolved->Data;
					}
					if (!value) return true;
					const auto existing =
						std::find_if(inputs.begin(), inputs.end(), [&](const AuthoredValue &entry) {
							return entry.Port == port;
						});
					if (existing == inputs.end())
						inputs.push_back({std::string(port), *value});
					else
						existing->Data = *value;
					return true;
				};
				for (const PortSchema &port : FindSchema(node.Type)->Ports) {
					if (port.Direction != PortDirection::Input) continue;
					if (!resolveValueInput(port.Id, nullptr)) return diagnostic.Code;
				}
				for (const DynamicInput &input : node.DynamicInputs)
					if (!resolveValueInput(input.Id, input.Default ? &*input.Default : nullptr))
						return diagnostic.Code;
				std::string failedPort;
				std::string failureMessage;
				const Status status = detail::EvaluateValueNode(
					node,
					inputs,
					request,
					document.Timeline ? &*document.Timeline : nullptr,
					valueResult,
					failedPort,
					failureMessage
				);
				if (status != Status::Ok) {
					SetDiagnostic(diagnostic, status, failureMessage, node.Id, failedPort);
					return diagnostic.Code;
				}
			} else if (node.Type == "image.solid") {
				const AuthoredValue *widthValue = FindValue(node, "width");
				const AuthoredValue *heightValue = FindValue(node, "height");
				const AuthoredValue *colourValue = FindValue(node, "colour");
				const auto *widthInteger = widthValue ? std::get_if<int64_t>(&widthValue->Data) : nullptr;
				const auto *heightInteger = heightValue ? std::get_if<int64_t>(&heightValue->Data) : nullptr;
				const auto *colour = colourValue ? std::get_if<Colour>(&colourValue->Data) : nullptr;
				if (!widthInteger || !heightInteger || !colour) {
					SetDiagnostic(
						diagnostic, Status::InvalidValue, "solid node values are incomplete", node.Id
					);
					return diagnostic.Code;
				}
				const Image *foreground = nullptr;
				const Image *mask = nullptr;
				if (!resolveImage("foreground", false, foreground) || !resolveImage("mask", false, mask))
					return diagnostic.Code;
				const AuthoredValue *maskDimensionValue = FindValue(node, "use_mask_dimension");
				const bool maskDimension =
					maskDimensionValue ? std::get<bool>(maskDimensionValue->Data) : true;
				const uint64_t width =
					mask && maskDimension ? mask->Width : static_cast<uint64_t>(*widthInteger);
				const uint64_t height =
					mask && maskDimension ? mask->Height : static_cast<uint64_t>(*heightInteger);
				const uint64_t byteCount = width * height * 4;
				if (byteCount > Limits::MaximumEvaluationBytes - evaluationBytes) {
					SetDiagnostic(
						diagnostic,
						Status::LimitExceeded,
						"evaluation intermediates exceed the byte budget",
						node.Id
					);
					return diagnostic.Code;
				}
				result.Width = static_cast<uint32_t>(width);
				result.Height = static_cast<uint32_t>(height);
				result.Pixels.resize(static_cast<size_t>(byteCount));
				const AuthoredValue *emptyValue = FindValue(node, "empty");
				const bool empty = emptyValue ? std::get<bool>(emptyValue->Data) : false;
				const AuthoredValue *alphaOnlyValue = FindValue(node, "mask_alpha_only");
				const bool alphaOnly = alphaOnlyValue ? std::get<bool>(alphaOnlyValue->Data) : false;
				detail::GenerateSolid(result, *colour, foreground, mask, empty, alphaOnly);
				result.Hash = detail::PixelHash(result);
			} else if (node.Type == "image.greyscale" || node.Type == "image.bw") {
				const Image *source = nullptr, *mask = nullptr, *brightnessMap = nullptr,
							*contrastMap = nullptr;
				if (!resolveImage("image", true, source) || !resolveImage("mask", false, mask) ||
					!resolveImage("brightness_map", false, brightnessMap) ||
					!resolveImage("contrast_map", false, contrastMap))
					return diagnostic.Code;
				if (brightnessMap || contrastMap) {
					SetDiagnostic(
						diagnostic,
						Status::UnsupportedExecution,
						"monochrome mapped controls are unavailable",
						node.Id
					);
					return diagnostic.Code;
				}
				const auto scalar = [&](std::string_view port, double fallback) {
					const AuthoredValue *value = FindValue(node, port);
					return value ? std::get<double>(value->Data) : fallback;
				};
				const double mix = scalar("mix", 1.0);
				const double feather = scalar("mask_feather", 0.0);
				const AuthoredValue *channelValue = FindValue(node, "channel");
				const int64_t channel = channelValue ? std::get<int64_t>(channelValue->Data) : 15;
				if (mix < 0.0 || mix > 1.0 || channel < 0 || channel > 15) {
					SetDiagnostic(
						diagnostic, Status::InvalidValue, "monochrome mix or channel is invalid", node.Id
					);
					return diagnostic.Code;
				}
				if (feather != 0.0) {
					SetDiagnostic(
						diagnostic,
						Status::UnsupportedExecution,
						"monochrome mask feather is unavailable",
						node.Id,
						"mask_feather"
					);
					return diagnostic.Code;
				}
				if (source->Pixels.size() > Limits::MaximumEvaluationBytes - evaluationBytes) {
					SetDiagnostic(
						diagnostic, Status::LimitExceeded, "monochrome exceeds byte budget", node.Id
					);
					return diagnostic.Code;
				}
				result = *source;
				const detail::ConversionStatus status = detail::RenderMonochrome(
					*source,
					result,
					node.Type == "image.bw",
					scalar("brightness", 0.0),
					scalar("contrast", 1.0)
				);
				if (status != detail::ConversionStatus::Ok) {
					SetDiagnostic(
						diagnostic, Status::InvalidValue, "monochrome controls or image are invalid", node.Id
					);
					return diagnostic.Code;
				}
				const AuthoredValue *invertValue = FindValue(node, "invert_mask");
				detail::ApplyMaskMix(
					*source, result, mask, mix, invertValue && std::get<bool>(invertValue->Data)
				);
				detail::ApplyChannels(*source, result, channel);
				result.Hash = detail::PixelHash(result);
			} else if (node.Type == "image.grey_alpha") {
				const Image *source = nullptr;
				if (!resolveImage("image", true, source)) return diagnostic.Code;
				if (source->Pixels.size() > Limits::MaximumEvaluationBytes - evaluationBytes) {
					SetDiagnostic(
						diagnostic, Status::LimitExceeded, "grey alpha exceeds byte budget", node.Id
					);
					return diagnostic.Code;
				}
				result = *source;
				const AuthoredValue *curve = FindValue(node, "curve");
				const AuthoredValue *invert = FindValue(node, "invert");
				const AuthoredValue *replace = FindValue(node, "replace_color");
				const AuthoredValue *colour = FindValue(node, "color");
				const detail::ConversionStatus status = detail::RenderGreyAlpha(
					*source,
					result,
					curve ? std::get<Curve>(curve->Data) : detail::IdentityColorCurve(),
					invert && std::get<bool>(invert->Data),
					!replace || std::get<bool>(replace->Data),
					colour ? std::get<Colour>(colour->Data) : Colour{255, 255, 255, 255}
				);
				if (status != detail::ConversionStatus::Ok) {
					SetDiagnostic(
						diagnostic,
						status == detail::ConversionStatus::UndefinedCurve ? Status::UnsupportedExecution
																		   : Status::InvalidValue,
						"grey alpha curve or image is invalid",
						node.Id,
						"curve"
					);
					return diagnostic.Code;
				}
				result.Hash = detail::PixelHash(result);
			} else if (node.Type == "image.rgb_extract" || node.Type == "image.hsv_extract") {
				const Image *source = nullptr;
				if (!resolveImage("image", true, source)) return diagnostic.Code;
				const AuthoredValue *arrayValue = FindValue(node, "output_array");
				if (arrayValue && std::get<bool>(arrayValue->Data)) {
					SetDiagnostic(
						diagnostic,
						Status::UnsupportedExecution,
						"conversion Output Array needs conditional port routing",
						node.Id,
						"output_array"
					);
					return diagnostic.Code;
				}
				if (source->Pixels.size() > (Limits::MaximumEvaluationBytes - evaluationBytes) / 4) {
					SetDiagnostic(
						diagnostic,
						Status::LimitExceeded,
						"conversion four outputs exceed byte budget",
						node.Id
					);
					return diagnostic.Code;
				}
				for (Image &image : conversionResult.Channels) {
					image.Width = source->Width;
					image.Height = source->Height;
					image.Pixels.resize(source->Pixels.size());
				}
				conversionResult.Hsv = node.Type == "image.hsv_extract";
				const detail::ConversionStatus status =
					conversionResult.Hsv ? detail::RenderHsvExtract(
											   *source,
											   conversionResult.Channels,
											   FindValue(node, "color_space")
												   ? std::get<int64_t>(FindValue(node, "color_space")->Data)
												   : 0
										   )
										 : detail::RenderRgbExtract(
											   *source,
											   conversionResult.Channels,
											   FindValue(node, "output_type")
												   ? std::get<int64_t>(FindValue(node, "output_type")->Data)
												   : 0,
											   FindValue(node, "keep_alpha") &&
												   std::get<bool>(FindValue(node, "keep_alpha")->Data)
										   );
				if (status != detail::ConversionStatus::Ok) {
					SetDiagnostic(
						diagnostic,
						Status::InvalidValue,
						"conversion extract controls or image are invalid",
						node.Id
					);
					return diagnostic.Code;
				}
				for (Image &image : conversionResult.Channels)
					image.Hash = detail::PixelHash(image);
				producedConversion = true;
			} else if (node.Type == "image.combine_rgb" || node.Type == "image.combine_hsv" ||
					   node.Type == "image.override_channel") {
				const bool rgb = node.Type == "image.combine_rgb";
				const bool hsv = node.Type == "image.combine_hsv";
				const AuthoredValue *arrayInput = FindValue(node, "array_input");
				const auto hasInput = [&](std::string_view port) {
					return std::any_of(
						plan.EffectiveLinks.begin(), plan.EffectiveLinks.end(), [&](const Link &link) {
							return link.ToNode == node.Id && link.ToPort == port;
						}
					);
				};
				if ((arrayInput && std::get<bool>(arrayInput->Data)) ||
					(rgb && (hasInput("rgba_array") || hasInput("base_map"))) ||
					(hsv && hasInput("hsv_array"))) {
					SetDiagnostic(
						diagnostic,
						Status::UnsupportedExecution,
						"channel assembly array or mapped base input is unavailable",
						node.Id,
						arrayInput && std::get<bool>(arrayInput->Data) ? "array_input"
						: rgb && hasInput("base_map")				   ? "base_map"
						: rgb										   ? "rgba_array"
																	   : "hsv_array"
					);
					return diagnostic.Code;
				}
				const std::array<std::string_view, 4> names =
					hsv ? std::array<std::string_view, 4>{"hue", "saturation", "value", "alpha"}
						: std::array<std::string_view, 4>{"red", "green", "blue", "alpha"};
				std::array<const Image *, 4> inputs{};
				for (size_t channel = 0; channel < 4; ++channel)
					if (!resolveImage(names[channel], false, inputs[channel])) return diagnostic.Code;
				const Image *base = nullptr;
				if (!rgb && !hsv && !resolveImage("image", true, base)) return diagnostic.Code;
				const Image *dimensions = base;
				if (!dimensions)
					for (size_t channel = 0; channel < 3 && !dimensions; ++channel)
						dimensions = inputs[channel];
				if (!dimensions) {
					SetDiagnostic(
						diagnostic,
						Status::UnsupportedExecution,
						"channel assembly has no size-setting source",
						node.Id
					);
					return diagnostic.Code;
				}
				if (dimensions->Pixels.size() > Limits::MaximumEvaluationBytes - evaluationBytes) {
					SetDiagnostic(
						diagnostic, Status::LimitExceeded, "channel assembly exceeds byte budget", node.Id
					);
					return diagnostic.Code;
				}
				result.Width = dimensions->Width;
				result.Height = dimensions->Height;
				result.Pixels.resize(dimensions->Pixels.size());
				const AuthoredValue *mode = FindValue(node, hsv ? "color_space" : "sampling_type");
				const int64_t modeValue = mode ? std::get<int64_t>(mode->Data) : 0;
				const AuthoredValue *baseValue = rgb ? FindValue(node, "base_value") : nullptr;
				const detail::ChannelAssemblyStatus status =
					rgb ? detail::RenderRgbCombine(
							  inputs, result, modeValue, baseValue ? std::get<double>(baseValue->Data) : 0.0
						  )
					: hsv ? detail::RenderHsvCombine(inputs, result, modeValue)
						  : detail::RenderOverrideChannel(*base, inputs, result, modeValue);
				if (status != detail::ChannelAssemblyStatus::Ok) {
					SetDiagnostic(
						diagnostic,
						status == detail::ChannelAssemblyStatus::MissingSource ? Status::UnsupportedExecution
																			   : Status::InvalidValue,
						"channel assembly controls or source dimensions are invalid",
						node.Id
					);
					return diagnostic.Code;
				}
				result.Hash = detail::PixelHash(result);
			} else if (node.Type == "image.multiply_alpha" || node.Type == "image.gamma_map") {
				const Image *source = nullptr, *mask = nullptr;
				if (!resolveImage("image", true, source) ||
					(node.Type == "image.multiply_alpha" && !resolveImage("mask", false, mask)))
					return diagnostic.Code;
				const AuthoredValue *active = FindValue(node, "active");
				const bool enabled = !active || std::get<bool>(active->Data);
				const AuthoredValue *feather = FindValue(node, "mask_feather");
				const AuthoredValue *oversample = FindValue(node, "oversample");
				if ((feather && std::get<double>(feather->Data) != 0.0) ||
					(oversample && std::get<int64_t>(oversample->Data) != 0)) {
					SetDiagnostic(
						diagnostic,
						Status::UnsupportedExecution,
						"multiply alpha mask feather or oversample is unavailable",
						node.Id,
						feather && std::get<double>(feather->Data) != 0.0 ? "mask_feather" : "oversample"
					);
					return diagnostic.Code;
				}
				if (source->Pixels.size() > Limits::MaximumEvaluationBytes - evaluationBytes) {
					SetDiagnostic(
						diagnostic, Status::LimitExceeded, "channel filter exceeds byte budget", node.Id
					);
					return diagnostic.Code;
				}
				result = *source;
				if (enabled && node.Type == "image.gamma_map") {
					const AuthoredValue *invert = FindValue(node, "invert");
					if (detail::RenderGammaMap(*source, result, invert && std::get<bool>(invert->Data)) !=
						detail::ChannelAssemblyStatus::Ok) {
						SetDiagnostic(
							diagnostic, Status::InvalidValue, "gamma map image is invalid", node.Id
						);
						return diagnostic.Code;
					}
				} else if (enabled) {
					const AuthoredValue *threshold = FindValue(node, "threshold");
					const AuthoredValue *background = FindValue(node, "bg_color");
					const auto status = detail::RenderMultiplyAlpha(
						*source,
						result,
						threshold ? std::get<double>(threshold->Data) : 0.0,
						background ? std::get<Colour>(background->Data) : Colour{255, 255, 255, 255}
					);
					if (status != detail::ChannelAssemblyStatus::Ok) {
						SetDiagnostic(
							diagnostic, Status::InvalidValue, "multiply alpha controls are invalid", node.Id
						);
						return diagnostic.Code;
					}
					const AuthoredValue *mix = FindValue(node, "mix");
					const AuthoredValue *channel = FindValue(node, "channel");
					const double mixValue = mix ? std::get<double>(mix->Data) : 1.0;
					const int64_t channelValue = channel ? std::get<int64_t>(channel->Data) : 15;
					if (mixValue < 0.0 || mixValue > 1.0 || channelValue < 0 || channelValue > 15) {
						SetDiagnostic(
							diagnostic,
							Status::InvalidValue,
							"multiply alpha mix or channel is invalid",
							node.Id
						);
						return diagnostic.Code;
					}
					const AuthoredValue *invertMask = FindValue(node, "invert_mask");
					detail::ApplyMaskMix(
						*source, result, mask, mixValue, invertMask && std::get<bool>(invertMask->Data)
					);
					detail::ApplyChannels(*source, result, channelValue);
				}
				result.Hash = detail::PixelHash(result);
			} else if (node.Type == "image.mirror" || node.Type == "image.barrel_distort" ||
					   node.Type == "image.chromatic_aberration" || node.Type == "image.spherize" ||
					   node.Type == "image.dilate") {
				const bool mirror = node.Type == "image.mirror";
				const bool barrel = node.Type == "image.barrel_distort";
				const bool chromatic = node.Type == "image.chromatic_aberration";
				const bool spherize = node.Type == "image.spherize";
				const Image *source = nullptr, *mask = nullptr;
				if (!resolveImage("image", true, source) || !resolveImage("mask", false, mask))
					return diagnostic.Code;
				const auto scalar = [&](std::string_view key, double fallback) {
					const AuthoredValue *value = FindValue(node, key);
					return value ? std::get<double>(value->Data) : fallback;
				};
				const auto integer = [&](std::string_view key, int64_t fallback) {
					const AuthoredValue *value = FindValue(node, key);
					return value ? std::get<int64_t>(value->Data) : fallback;
				};
				const auto boolean = [&](std::string_view key, bool fallback) {
					const AuthoredValue *value = FindValue(node, key);
					return value ? std::get<bool>(value->Data) : fallback;
				};
				const auto vector = [&](std::string_view key, Vector2 fallback) {
					const AuthoredValue *value = FindValue(node, key);
					return value ? std::get<Vector2>(value->Data) : fallback;
				};
				const auto linked = [&](std::string_view key) {
					return std::any_of(
						plan.EffectiveLinks.begin(), plan.EffectiveLinks.end(), [&](const Link &link) {
							return link.ToNode == node.Id && link.ToPort == key;
						}
					);
				};
				for (const std::string_view port :
					 {"uv_map", "intensity_map", "strength_map", "radius_map", "shift_map", "scale_map"}) {
					if (linked(port)) {
						SetDiagnostic(
							diagnostic,
							Status::UnsupportedExecution,
							"spatial mapped control is unavailable",
							node.Id,
							std::string(port)
						);
						return diagnostic.Code;
					}
				}
				if (!boolean("active", true)) {
					SetDiagnostic(
						diagnostic,
						Status::UnsupportedExecution,
						"inactive spatial node behavior is unavailable",
						node.Id,
						"active"
					);
					return diagnostic.Code;
				}
				if (scalar("mask_feather", 0.0) != 0.0 || FindValue(node, "strength_curve")) {
					const bool feather = scalar("mask_feather", 0.0) != 0.0;
					SetDiagnostic(
						diagnostic,
						Status::UnsupportedExecution,
						"spatial curve or mask feather is unavailable",
						node.Id,
						feather ? "mask_feather" : "strength_curve"
					);
					return diagnostic.Code;
				}
				const int64_t oversample = integer("oversample", spherize ? 3 : 0);
				if (oversample != (spherize ? 3 : 0) || integer("interpolation", 0) != 0 ||
					((chromatic || node.Type == "image.dilate") && scalar("uv_mix", 1.0) != 1.0)) {
					SetDiagnostic(
						diagnostic,
						Status::UnsupportedExecution,
						"spatial sampler variant is unavailable",
						node.Id
					);
					return diagnostic.Code;
				}
				const double mix = scalar("mix", 1.0);
				const int64_t channel = integer("channel", 15);
				if (mix < 0.0 || mix > 1.0 || channel < 0 || channel > 15) {
					SetDiagnostic(
						diagnostic, Status::InvalidValue, "spatial mix or channel is invalid", node.Id
					);
					return diagnostic.Code;
				}
				const uint64_t byteCount = source->Pixels.size();
				if (byteCount > (Limits::MaximumEvaluationBytes - evaluationBytes) / (mirror ? 2 : 1)) {
					SetDiagnostic(
						diagnostic, Status::LimitExceeded, "spatial outputs exceed byte budget", node.Id
					);
					return diagnostic.Code;
				}
				detail::SpatialWarpStatus status = detail::SpatialWarpStatus::InvalidControl;
				if (mirror) {
					mirrorResult.Colored = *source;
					mirrorResult.Mask = *source;
					const Vector2 position = vector("position", {0.5, 0.5});
					detail::MirrorControl control;
					control.PositionX = position.X * source->Width;
					control.PositionY = position.Y * source->Height;
					control.AngleDegrees = scalar("angle", 0.0);
					control.Flip = boolean("flip", false);
					control.BothSide = boolean("both_side", false);
					status = detail::RenderMirror(*source, mirrorResult.Colored, mirrorResult.Mask, control);
				} else {
					result = *source;
					const Vector2 center = vector("center", {0.5, 0.5});
					if (barrel) {
						detail::BarrelControl control;
						control.CenterX = center.X * source->Width;
						control.CenterY = center.Y * source->Height;
						control.Intensity = scalar("intensity", 1.5);
						const Vector2 scale = vector("scale", {1.0, 1.0});
						control.ScaleX = scale.X;
						control.ScaleY = scale.Y;
						control.DistanceMethod = integer("distance_method", 0);
						status = detail::RenderBarrel(*source, result, control);
					} else if (chromatic) {
						if (integer("type", 0) != 0 || scalar("shift", 0.0) != 0.0 ||
							scalar("scale", 1.0) != 1.0 || integer("resolution", 64) != 64) {
							SetDiagnostic(
								diagnostic,
								Status::UnsupportedExecution,
								"chromatic non-Scale controls are unavailable",
								node.Id,
								"type"
							);
							return diagnostic.Code;
						}
						const int64_t iterations = integer("iterations", 1);
						if (iterations < 1 || iterations > 64) {
							SetDiagnostic(
								diagnostic,
								Status::InvalidValue,
								"chromatic iteration count is invalid",
								node.Id,
								"iterations"
							);
							return diagnostic.Code;
						}
						detail::ChromaticControl control;
						control.CenterX = center.X * source->Width;
						control.CenterY = center.Y * source->Height;
						control.Strength = scalar("strength", 1.0);
						control.Intensity = scalar("intensity", 1.0);
						control.Iterations = static_cast<uint32_t>(iterations);
						status = detail::RenderChromaticScale(*source, result, control);
					} else if (spherize) {
						detail::SpherizeControl control;
						control.CenterX = center.X * source->Width;
						control.CenterY = center.Y * source->Height;
						const Vector2 position = vector("position", {0.0, 0.0});
						control.PositionX = position.X * source->Width;
						control.PositionY = position.Y * source->Height;
						control.RotationDegrees = scalar("rotation", 0.0);
						control.Strength = scalar("strength", 1.0);
						control.Radius = scalar("radius", 0.2);
						control.Normalize = boolean("normalize", false);
						control.Trim = scalar("trim", 0.0);
						const Vector2 textureOffset = vector("texture_offset", {0.0, 0.0});
						const Vector2 textureScale = vector("texture_scale", {1.0, 1.0});
						control.TextureOffsetX = textureOffset.X;
						control.TextureOffsetY = textureOffset.Y;
						control.TextureScaleX = textureScale.X;
						control.TextureScaleY = textureScale.Y;
						status = detail::RenderSpherize(*source, result, control);
					} else {
						detail::DilateControl control;
						control.CenterX = center.X * source->Width;
						control.CenterY = center.Y * source->Height;
						control.Strength = scalar("strength", 1.0);
						control.Radius = scalar("radius", 0.5);
						status = detail::RenderDilate(*source, result, control);
					}
				}
				if (status != detail::SpatialWarpStatus::Ok) {
					SetDiagnostic(
						diagnostic,
						status == detail::SpatialWarpStatus::UndefinedDivision ? Status::UnsupportedExecution
																			   : Status::InvalidValue,
						status == detail::SpatialWarpStatus::UndefinedDivision
							? "spatial warp has undefined division or UV"
							: "spatial warp controls or image are invalid",
						node.Id
					);
					return diagnostic.Code;
				}
				Image &colored = mirror ? mirrorResult.Colored : result;
				detail::ApplyMaskMix(*source, colored, mask, mix, boolean("invert_mask", false));
				if (!chromatic) detail::ApplyChannels(*source, colored, channel);
				colored.Hash = detail::PixelHash(colored);
				if (mirror) {
					mirrorResult.Mask.Hash = detail::PixelHash(mirrorResult.Mask);
					producedMirror = true;
				}
			} else if (node.Type == "image.curve") {
				const Image *source = nullptr, *mask = nullptr;
				if (!resolveImage("image", true, source) || !resolveImage("mask", false, mask))
					return diagnostic.Code;
				const auto scalar = [&](std::string_view port, double fallback) {
					const AuthoredValue *value = FindValue(node, port);
					return value ? std::get<double>(value->Data) : fallback;
				};
				const auto integer = [&](std::string_view port, int64_t fallback) {
					const AuthoredValue *value = FindValue(node, port);
					return value ? std::get<int64_t>(value->Data) : fallback;
				};
				const double mix = scalar("mix", 1.0);
				const int64_t channel = integer("channel", 15);
				if (mix < 0.0 || mix > 1.0 || channel < 0 || channel > 15) {
					SetDiagnostic(
						diagnostic, Status::InvalidValue, "curve mix or channel is invalid", node.Id
					);
					return diagnostic.Code;
				}
				if (scalar("mask_feather", 0.0) != 0.0) {
					SetDiagnostic(
						diagnostic,
						Status::UnsupportedExecution,
						"curve mask feather is unavailable",
						node.Id,
						"mask_feather"
					);
					return diagnostic.Code;
				}
				if (source->Pixels.size() > (Limits::MaximumEvaluationBytes - evaluationBytes) / 2) {
					SetDiagnostic(diagnostic, Status::LimitExceeded, "curve exceeds byte budget", node.Id);
					return diagnostic.Code;
				}
				result = *source;
				const auto curve = [&](std::string_view port) {
					const AuthoredValue *value = FindValue(node, port);
					return value ? std::get<Curve>(value->Data) : detail::IdentityColorCurve();
				};
				const detail::CurveColorControls control{
					curve("brightness"), curve("red"), curve("green"), curve("blue"), curve("alpha")
				};
				if (detail::RenderCurveColor(*source, result, control) != detail::CurveColorStatus::Ok) {
					SetDiagnostic(
						diagnostic,
						Status::UnsupportedExecution,
						"curve mode, handles or anchor count is unavailable",
						node.Id
					);
					return diagnostic.Code;
				}
				const AuthoredValue *invertValue = FindValue(node, "invert_mask");
				const bool invert = invertValue ? std::get<bool>(invertValue->Data) : false;
				detail::ApplyMaskMix(*source, result, mask, mix, invert);
				detail::ApplyChannels(*source, result, channel);
				result.Hash = detail::PixelHash(result);
			} else if (node.Type == "image.colorize") {
				const Image *source = nullptr, *mask = nullptr, *gradientMap = nullptr, *shiftMap = nullptr;
				if (!resolveImage("image", true, source) || !resolveImage("mask", false, mask) ||
					!resolveImage("gradient_map", false, gradientMap) ||
					!resolveImage("shift_map", false, shiftMap))
					return diagnostic.Code;
				if (gradientMap || shiftMap) {
					SetDiagnostic(
						diagnostic,
						Status::UnsupportedExecution,
						"colorize mapped gradient or shift is unavailable",
						node.Id
					);
					return diagnostic.Code;
				}
				const auto scalar = [&](std::string_view port, double fallback) {
					const AuthoredValue *value = FindValue(node, port);
					return value ? std::get<double>(value->Data) : fallback;
				};
				const auto integer = [&](std::string_view port, int64_t fallback) {
					const AuthoredValue *value = FindValue(node, port);
					return value ? std::get<int64_t>(value->Data) : fallback;
				};
				const auto boolean = [&](std::string_view port, bool fallback) {
					const AuthoredValue *value = FindValue(node, port);
					return value ? std::get<bool>(value->Data) : fallback;
				};
				const double mix = scalar("mix", 1.0);
				const int64_t channel = integer("channel", 15);
				if (mix < 0.0 || mix > 1.0 || channel < 0 || channel > 15) {
					SetDiagnostic(
						diagnostic, Status::InvalidValue, "colorize mix or channel is invalid", node.Id
					);
					return diagnostic.Code;
				}
				if (scalar("mask_feather", 0.0) != 0.0) {
					SetDiagnostic(
						diagnostic,
						Status::UnsupportedExecution,
						"colorize mask feather is unavailable",
						node.Id,
						"mask_feather"
					);
					return diagnostic.Code;
				}
				if (source->Pixels.size() > (Limits::MaximumEvaluationBytes - evaluationBytes) / 2) {
					SetDiagnostic(diagnostic, Status::LimitExceeded, "colorize exceeds byte budget", node.Id);
					return diagnostic.Code;
				}
				result = *source;
				const AuthoredValue *gradientValue = FindValue(node, "gradient");
				const Gradient gradient =
					gradientValue ? std::get<Gradient>(gradientValue->Data)
								  : Gradient{0, {{0.0, {0, 0, 0, 255}}, {1.0, {255, 255, 255, 255}}}};
				detail::ColorizeControls control;
				control.Shift = scalar("shift", 0.0);
				if (const AuthoredValue *range = FindValue(node, "color_range"))
					control.Range = std::get<Vector2>(range->Data);
				control.Overflow = integer("overflow", 0);
				control.MultiplyAlpha = boolean("multiply_alpha", true);
				control.KeepAlpha = boolean("keep_alpha", true);
				const detail::CurveColorStatus status =
					detail::RenderColorize(*source, result, gradient, control);
				if (status != detail::CurveColorStatus::Ok) {
					SetDiagnostic(
						diagnostic,
						Status::UnsupportedExecution,
						status == detail::CurveColorStatus::UndefinedDivision
							? "colorize range division is undefined"
							: "colorize gradient or controls are unavailable",
						node.Id
					);
					return diagnostic.Code;
				}
				detail::ApplyMaskMix(*source, result, mask, mix, boolean("invert_mask", false));
				detail::ApplyChannels(*source, result, channel);
				result.Hash = detail::PixelHash(result);
			} else if (node.Type == "image.displace") {
				const Image *source = nullptr, *map = nullptr, *map2 = nullptr;
				const Image *uvMap = nullptr, *mask = nullptr, *strengthMap = nullptr, *midMap = nullptr;
				if (!resolveImage("image", true, source) || !resolveImage("displace_map", true, map) ||
					!resolveImage("displace_map_2", false, map2) || !resolveImage("uv_map", false, uvMap) ||
					!resolveImage("mask", false, mask) || !resolveImage("strength_map", false, strengthMap) ||
					!resolveImage("mid_value_map", false, midMap))
					return diagnostic.Code;
				const auto scalar = [&](std::string_view port, double fallback) {
					const AuthoredValue *value = FindValue(node, port);
					return value ? std::get<double>(value->Data) : fallback;
				};
				const auto integer = [&](std::string_view port, int64_t fallback) {
					const AuthoredValue *value = FindValue(node, port);
					return value ? std::get<int64_t>(value->Data) : fallback;
				};
				const auto boolean = [&](std::string_view port, bool fallback) {
					const AuthoredValue *value = FindValue(node, port);
					return value ? std::get<bool>(value->Data) : fallback;
				};
				if (map2 || uvMap || mask || strengthMap || midMap || FindValue(node, "strength_curve") ||
					integer("oversample", 1) != 1 || integer("oversample_mode", 0) != 0 ||
					scalar("uv_mix", 1.0) != 1.0 || scalar("mix", 1.0) != 1.0 ||
					boolean("invert_mask", false) || scalar("mask_feather", 0.0) != 0.0 ||
					integer("channel", 15) != 15 || boolean("separate_axis", false) ||
					boolean("iterate", false) || integer("blend_mode", 0) != 0 ||
					integer("iteration", 16) != 16 || scalar("mix_ratio", 0.5) != 0.5 ||
					boolean("fade_distance", false) || boolean("reposition", false) ||
					integer("repeat", 1) != 1 || boolean("stop_empty", false)) {
					SetDiagnostic(
						diagnostic,
						Status::UnsupportedExecution,
						"displace mapped, mask, channel, iteration or oversample control is unavailable",
						node.Id
					);
					return diagnostic.Code;
				}
				if (source->Pixels.size() > Limits::MaximumEvaluationBytes - evaluationBytes) {
					SetDiagnostic(diagnostic, Status::LimitExceeded, "displace exceeds byte budget", node.Id);
					return diagnostic.Code;
				}
				result = *source;
				detail::DisplaceControls control;
				control.Mode = integer("mode", 0);
				control.Strength = scalar("strength", 1.0);
				control.MidValue = scalar("mid_value", 0.5);
				control.AngleOffsetDegrees = scalar("angle_offset", 0.0);
				if (const AuthoredValue *position = FindValue(node, "position")) {
					const Vector2 normalized = std::get<Vector2>(position->Data);
					control.PositionPixels = {normalized.X * source->Width, normalized.Y * source->Height};
				} else {
					control.PositionPixels = {double(source->Width), 0.0};
				}
				const int64_t interpolation = integer("interpolation", 1);
				if (interpolation != 1 && interpolation != 2) {
					SetDiagnostic(
						diagnostic,
						Status::UnsupportedExecution,
						"displace interpolation must resolve to Pixel or Bilinear",
						node.Id,
						"interpolation"
					);
					return diagnostic.Code;
				}
				control.Sampling =
					interpolation == 1 ? detail::WarpSampling::Nearest : detail::WarpSampling::Linear;
				const detail::WarpStatus status = detail::RenderDisplace(*source, *map, result, control);
				if (status != detail::WarpStatus::Ok) {
					SetDiagnostic(
						diagnostic,
						status == detail::WarpStatus::InvalidControl ? Status::UnsupportedExecution
																	 : Status::InvalidValue,
						"displace controls or image are unsupported",
						node.Id
					);
					return diagnostic.Code;
				}
				result.Hash = detail::PixelHash(result);
			} else if (node.Type == "image.polar") {
				const Image *source = nullptr, *mask = nullptr, *angleMap = nullptr;
				const Image *blendMap = nullptr, *twistMap = nullptr;
				if (!resolveImage("image", true, source) || !resolveImage("mask", false, mask) ||
					!resolveImage("angle_map", false, angleMap) ||
					!resolveImage("blend_map", false, blendMap) ||
					!resolveImage("twist_map", false, twistMap))
					return diagnostic.Code;
				const auto scalar = [&](std::string_view port, double fallback) {
					const AuthoredValue *value = FindValue(node, port);
					return value ? std::get<double>(value->Data) : fallback;
				};
				const auto integer = [&](std::string_view port, int64_t fallback) {
					const AuthoredValue *value = FindValue(node, port);
					return value ? std::get<int64_t>(value->Data) : fallback;
				};
				const auto boolean = [&](std::string_view port, bool fallback) {
					const AuthoredValue *value = FindValue(node, port);
					return value ? std::get<bool>(value->Data) : fallback;
				};
				if (mask || angleMap || blendMap || twistMap || integer("channel", 15) != 15 ||
					scalar("mix", 1.0) != 1.0 || boolean("invert_mask", false) ||
					scalar("mask_feather", 0.0) != 0.0) {
					SetDiagnostic(
						diagnostic,
						Status::UnsupportedExecution,
						"polar mask, channel, or mapped control is unavailable",
						node.Id
					);
					return diagnostic.Code;
				}
				if (source->Pixels.size() > Limits::MaximumEvaluationBytes - evaluationBytes) {
					SetDiagnostic(diagnostic, Status::LimitExceeded, "polar exceeds byte budget", node.Id);
					return diagnostic.Code;
				}
				result = *source;
				detail::PolarControls control;
				if (const AuthoredValue *tile = FindValue(node, "tile")) {
					const Vector2 value = std::get<Vector2>(tile->Data);
					control.Tile = {value.X, value.Y};
				}
				if (const AuthoredValue *center = FindValue(node, "center")) {
					const Vector2 value = std::get<Vector2>(center->Data);
					control.Center = {value.X, value.Y};
				}
				if (const AuthoredValue *range = FindValue(node, "range")) {
					const Vector2 value = std::get<Vector2>(range->Data);
					control.RangeDegrees = {value.X, value.Y};
				}
				control.AngleDegrees = scalar("angle", 0.0);
				control.Invert = boolean("invert", false);
				control.SwapAxis = boolean("swap_axis", false);
				control.Blend = scalar("blend", 1.0);
				control.Twist = scalar("twist", 0.0);
				control.RadiusMode = integer("radius_mode", 0);
				const int64_t interpolation = integer("interpolation", 1);
				if (interpolation != 1 && interpolation != 2) {
					SetDiagnostic(
						diagnostic,
						Status::UnsupportedExecution,
						"polar interpolation must resolve to Pixel or Bilinear",
						node.Id,
						"interpolation"
					);
					return diagnostic.Code;
				}
				control.Sampling =
					interpolation == 1 ? detail::WarpSampling::Nearest : detail::WarpSampling::Linear;
				const detail::WarpStatus status = detail::RenderPolar(*source, result, control);
				if (status != detail::WarpStatus::Ok) {
					SetDiagnostic(
						diagnostic,
						status == detail::WarpStatus::InvalidImage ? Status::InvalidValue
																   : Status::UnsupportedExecution,
						"polar controls or range are unsupported",
						node.Id
					);
					return diagnostic.Code;
				}
				result.Hash = detail::PixelHash(result);
			} else if (node.Type == "image.vignette") {
				const Image *source = nullptr, *mask = nullptr;
				const Image *roundnessMap = nullptr, *exposureMap = nullptr, *strengthMap = nullptr;
				if (!resolveImage("image", true, source) || !resolveImage("mask", false, mask) ||
					!resolveImage("roundness_map", false, roundnessMap) ||
					!resolveImage("exposure_map", false, exposureMap) ||
					!resolveImage("strength_map", false, strengthMap))
					return diagnostic.Code;
				const auto scalar = [&](std::string_view port, double fallback) {
					const AuthoredValue *value = FindValue(node, port);
					return value ? std::get<double>(value->Data) : fallback;
				};
				const auto boolean = [&](std::string_view port, bool fallback) {
					const AuthoredValue *value = FindValue(node, port);
					return value ? std::get<bool>(value->Data) : fallback;
				};
				if (mask || roundnessMap || exposureMap || strengthMap || FindValue(node, "strength_curve") ||
					scalar("mix", 1.0) != 1.0 || boolean("invert_mask", false) ||
					scalar("mask_feather", 1.0) != 1.0) {
					SetDiagnostic(
						diagnostic,
						Status::UnsupportedExecution,
						"vignette mask, mapped parameter or curve path is unavailable",
						node.Id
					);
					return diagnostic.Code;
				}
				if (source->Pixels.size() > Limits::MaximumEvaluationBytes - evaluationBytes) {
					SetDiagnostic(diagnostic, Status::LimitExceeded, "vignette exceeds byte budget", node.Id);
					return diagnostic.Code;
				}
				result = *source;
				detail::VignetteControls control;
				if (const AuthoredValue *center = FindValue(node, "center"))
					control.Center = std::get<Vector2>(center->Data);
				control.Roundness = scalar("roundness", 0.0);
				control.Exposure = scalar("exposure", 15.0);
				control.Strength = scalar("strength", 1.0);
				control.Lighten = scalar("lighten", 0.0);
				if (const AuthoredValue *color = FindValue(node, "color"))
					control.Color = std::get<Colour>(color->Data);
				// The pinned processor declares Exponent but never reads it.
				const detail::VignetteStatus status = detail::RenderVignetteScalar(*source, result, control);
				if (status != detail::VignetteStatus::Ok) {
					SetDiagnostic(
						diagnostic,
						status == detail::VignetteStatus::UndefinedPower ? Status::UnsupportedExecution
																		 : Status::InvalidValue,
						"vignette controls or power are undefined",
						node.Id
					);
					return diagnostic.Code;
				}
				result.Hash = detail::PixelHash(result);
			} else if (node.Type == "image.color_adjust") {
				const Image *source = nullptr, *mask = nullptr, *palette = nullptr;
				if (!resolveImage("image", true, source) || !resolveImage("mask", false, mask) ||
					!resolveImage("palette", false, palette))
					return diagnostic.Code;
				const auto scalar = [&](std::string_view port, double fallback) {
					const AuthoredValue *value = FindValue(node, port);
					return value ? std::get<double>(value->Data) : fallback;
				};
				const auto integer = [&](std::string_view port, int64_t fallback) {
					const AuthoredValue *value = FindValue(node, port);
					return value ? std::get<int64_t>(value->Data) : fallback;
				};
				const auto boolean = [&](std::string_view port, bool fallback) {
					const AuthoredValue *value = FindValue(node, port);
					return value ? std::get<bool>(value->Data) : fallback;
				};
				if (integer("input_type", 0) != 0 || palette || boolean("mapped", false) ||
					boolean("invert_mask", false) || scalar("mask_feather", 0.0) != 0.0 ||
					scalar("mix", 1.0) != 1.0) {
					SetDiagnostic(
						diagnostic,
						Status::UnsupportedExecution,
						"color adjust palette, mapped, mask modification or mix path is unavailable",
						node.Id
					);
					return diagnostic.Code;
				}
				if (source->Pixels.size() > Limits::MaximumEvaluationBytes - evaluationBytes) {
					SetDiagnostic(
						diagnostic, Status::LimitExceeded, "color adjust exceeds byte budget", node.Id
					);
					return diagnostic.Code;
				}
				result = *source;
				detail::ColorAdjustControls control;
				control.Brightness = scalar("brightness", 0.0);
				control.Contrast = scalar("contrast", 0.5);
				control.Exposure = scalar("exposure", 1.0);
				control.Hue = scalar("hue", 0.0);
				control.Saturation = scalar("saturation", 0.0);
				control.Value = scalar("value", 0.0);
				control.Alpha = scalar("alpha", 1.0);
				control.BlendMode = integer("blend_mode", 0);
				control.BlendAmount = scalar("blend_amount", 0.0);
				if (const AuthoredValue *blend = FindValue(node, "blend"))
					control.Blend = std::get<Colour>(blend->Data);
				if (detail::ColorAdjustSurface(*source, result, control, mask) !=
					detail::ColorAdjustStatus::Ok) {
					SetDiagnostic(
						diagnostic, Status::InvalidValue, "color adjust controls are invalid", node.Id
					);
					return diagnostic.Code;
				}
				detail::ApplyChannels(*source, result, integer("channel", 15));
				result.Hash = detail::PixelHash(result);
			} else if (node.Type == "image.blur") {
				const Image *source = nullptr, *mask = nullptr, *uvMap = nullptr;
				if (!resolveImage("image", true, source) || !resolveImage("mask", false, mask) ||
					!resolveImage("uv_map", false, uvMap))
					return diagnostic.Code;
				const auto scalar = [&](std::string_view port, double fallback) {
					const AuthoredValue *value = FindValue(node, port);
					return value ? std::get<double>(value->Data) : fallback;
				};
				const auto integer = [&](std::string_view port, int64_t fallback) {
					const AuthoredValue *value = FindValue(node, port);
					return value ? std::get<int64_t>(value->Data) : fallback;
				};
				const auto boolean = [&](std::string_view port, bool fallback) {
					const AuthoredValue *value = FindValue(node, port);
					return value ? std::get<bool>(value->Data) : fallback;
				};
				if (mask || uvMap || boolean("invert_mask", false) || scalar("mask_feather", 0.0) != 0.0 ||
					scalar("mix", 1.0) != 1.0 || FindValue(node, "uv_mix")) {
					SetDiagnostic(
						diagnostic,
						Status::UnsupportedExecution,
						"blur mask, UV or mix sampling path is unavailable",
						node.Id
					);
					return diagnostic.Code;
				}
				if (source->Pixels.size() > (Limits::MaximumEvaluationBytes - evaluationBytes) / 2) {
					SetDiagnostic(diagnostic, Status::LimitExceeded, "blur exceeds byte budget", node.Id);
					return diagnostic.Code;
				}
				result = *source;
				detail::GaussianBlurControls control;
				control.Size = scalar("size", 8.0);
				control.Intensity = integer("intensity", 0);
				control.AspectRatio = scalar("aspect", 1.0);
				control.DirectionRadians = scalar("direction", 0.0) * std::acos(-1.0) / 180.0;
				control.GammaCorrection = boolean("gamma", false);
				control.OverrideColor = boolean("override_color", false);
				if (const AuthoredValue *colour = FindValue(node, "color"))
					control.OverrideColour = std::get<Colour>(colour->Data);
				const detail::BlurStatus status = detail::GaussianBlurDefault(*source, result, control);
				if (status != detail::BlurStatus::Ok) {
					SetDiagnostic(
						diagnostic,
						status == detail::BlurStatus::UnsupportedControl ? Status::UnsupportedExecution
																		 : Status::InvalidValue,
						"blur size or sampling mode is unavailable",
						node.Id,
						"size"
					);
					return diagnostic.Code;
				}
				detail::ApplyChannels(*source, result, integer("channel", 15));
				result.Hash = detail::PixelHash(result);
			} else if (node.Type == "image.shape") {
				const auto *widthValue = FindValue(node, "width");
				const auto *heightValue = FindValue(node, "height");
				if (!widthValue || !heightValue) {
					SetDiagnostic(
						diagnostic, Status::InvalidValue, "shape requires width and height", node.Id
					);
					return diagnostic.Code;
				}
				const int64_t width = std::get<int64_t>(widthValue->Data);
				const int64_t height = std::get<int64_t>(heightValue->Data);
				if (width < 1 || height < 1 || width > Limits::MaximumDimension ||
					height > Limits::MaximumDimension ||
					uint64_t(width) * height * 16 > Limits::MaximumEvaluationBytes - evaluationBytes ||
					uint64_t(width) * height * 4 > Limits::MaximumOutputBytes) {
					SetDiagnostic(
						diagnostic, Status::LimitExceeded, "shape outputs exceed native limits", node.Id
					);
					return diagnostic.Code;
				}
				const auto scalar = [&](std::string_view port, double fallback) {
					const AuthoredValue *value = FindValue(node, port);
					return value ? std::get<double>(value->Data) : fallback;
				};
				const auto integer = [&](std::string_view port, int64_t fallback) {
					const AuthoredValue *value = FindValue(node, port);
					return value ? std::get<int64_t>(value->Data) : fallback;
				};
				const auto boolean = [&](std::string_view port, bool fallback) {
					const AuthoredValue *value = FindValue(node, port);
					return value ? std::get<bool>(value->Data) : fallback;
				};
				const auto vector = [&](std::string_view port, Vector2 fallback) {
					const AuthoredValue *value = FindValue(node, port);
					return value ? std::get<Vector2>(value->Data) : fallback;
				};
				const auto colour = [&](std::string_view port, Colour fallback) {
					const AuthoredValue *value = FindValue(node, port);
					return value ? std::get<Colour>(value->Data) : fallback;
				};
				const Image *uvMap = nullptr, *mask = nullptr, *background = nullptr;
				if (!resolveImage("uv_map", false, uvMap) || !resolveImage("mask", false, mask) ||
					!resolveImage("bg_surface", false, background))
					return diagnostic.Code;
				for (const char *unsupported :
					 {"curve", "uv_mix", "twist", "shear", "corner", "invert_mask"}) {
					if (FindValue(node, unsupported)) {
						SetDiagnostic(
							diagnostic,
							Status::UnsupportedExecution,
							"shape control needs a separate sampling path",
							node.Id,
							unsupported
						);
						return diagnostic.Code;
					}
				}
				if (uvMap || integer("position_mode", 1) != 1) {
					SetDiagnostic(
						diagnostic,
						Status::UnsupportedExecution,
						"shape UV map or position mode is unavailable",
						node.Id
					);
					return diagnostic.Code;
				}
				detail::ShapeRenderControls control;
				const auto *kind = FindValue(node, "shape");
				const std::string name = kind ? std::get<std::string>(kind->Data) : "Rectangle";
				if (name == "Rectangle")
					control.Geometry.Kind = detail::ShapeKind::Rectangle;
				else if (name == "Ellipse")
					control.Geometry.Kind = detail::ShapeKind::Ellipse;
				else if (name == "Half")
					control.Geometry.Kind = detail::ShapeKind::Half;
				else if (name == "Triangle")
					control.Geometry.Kind = detail::ShapeKind::Triangle;
				else {
					SetDiagnostic(
						diagnostic,
						Status::UnsupportedExecution,
						"shape kind is unavailable",
						node.Id,
						"shape"
					);
					return diagnostic.Code;
				}
				const Vector2 center = vector("center", {0.5, 0.5});
				const Vector2 halfSize = vector("half_size", {0.5, 0.5});
				const double scale = scalar("shape_scale", 1.0);
				control.Geometry.Center = {center.X, center.Y};
				control.Geometry.HalfSize = {halfSize.X * scale, halfSize.Y * scale};
				control.Geometry.RotationRadians = scalar("shape_rotation", 0.0) * std::acos(-1.0) / 180.0;
				const auto point = [&](std::string_view port, Vector2 fallback) {
					const Vector2 value = vector(port, fallback);
					return detail::ShapePoint{value.X * width, value.Y * height};
				};
				control.Geometry.Point1 = point("point1", {0.0, 0.0});
				control.Geometry.Point2 = point("point2", {1.0, 1.0});
				control.Geometry.Point3 = point("point3", {1.0, 0.0});
				control.Color = colour("color", {255, 255, 255, 255});
				control.Background = integer("background", 2);
				control.BackgroundColor = colour("bg_color", {0, 0, 0, 255});
				control.BackgroundBlend = integer("bg_blend", 0);
				control.Antialias = boolean("antialias", false);
				control.Height = boolean("height_render", false);
				control.Opacity = boolean("opacity", false);
				control.MultiplyAlpha = boolean("multiply_alpha", false);
				control.MaskAlphaOnly = boolean("mask_alpha_only", false);
				const Vector2 level = vector("level", {0.0, 1.0});
				if (scale == 0.0) {
					SetDiagnostic(
						diagnostic,
						Status::InvalidValue,
						"shape scale causes undefined level division",
						node.Id,
						"shape_scale"
					);
					return diagnostic.Code;
				}
				control.LevelIn = level.X / scale;
				control.LevelOut = level.Y / scale;
				const auto makeImage = [&]() {
					return Image{
						static_cast<uint32_t>(width),
						static_cast<uint32_t>(height),
						std::vector<uint8_t>(static_cast<size_t>(width * height * 4)),
						0
					};
				};
				shapeResult.Colored = makeImage();
				shapeResult.Mask = makeImage();
				shapeResult.Height = makeImage();
				shapeResult.UV = makeImage();
				const auto status = detail::RenderShapeBase(
					shapeResult.Colored,
					control,
					mask,
					background,
					{&shapeResult.Mask, &shapeResult.Height, &shapeResult.UV}
				);
				if (status != detail::ShapeRenderStatus::Ok) {
					SetDiagnostic(
						diagnostic, Status::InvalidValue, "shape controls or distance are undefined", node.Id
					);
					return diagnostic.Code;
				}
				for (Image *image :
					 {&shapeResult.Colored, &shapeResult.Mask, &shapeResult.Height, &shapeResult.UV})
					image->Hash = detail::PixelHash(*image);
				producedShape = true;
			} else if (node.Type == "image.checker") {
				const uint32_t width =
					static_cast<uint32_t>(std::get<int64_t>(FindValue(node, "width")->Data));
				const uint32_t height =
					static_cast<uint32_t>(std::get<int64_t>(FindValue(node, "height")->Data));
				const uint64_t bytes = uint64_t(width) * height * 4;
				if (bytes > Limits::MaximumEvaluationBytes - evaluationBytes) {
					SetDiagnostic(
						diagnostic, Status::LimitExceeded, "checker exceeds evaluation byte budget", node.Id
					);
					return diagnostic.Code;
				}
				const Image *uvMap = nullptr, *mask = nullptr, *sizeMap = nullptr, *angleMap = nullptr;
				if (!resolveImage("uv_map", false, uvMap) || !resolveImage("mask", false, mask) ||
					!resolveImage("size_map", false, sizeMap) || !resolveImage("angle_map", false, angleMap))
					return diagnostic.Code;
				const auto scalar = [&](std::string_view port, double fallback) {
					const AuthoredValue *value = FindValue(node, port);
					return value ? std::get<double>(value->Data) : fallback;
				};
				const auto integer = [&](std::string_view port, int64_t fallback) {
					const AuthoredValue *value = FindValue(node, port);
					return value ? std::get<int64_t>(value->Data) : fallback;
				};
				if (uvMap || mask || sizeMap || angleMap || integer("type", 0) != 0 ||
					FindValue(node, "uv_mix")) {
					SetDiagnostic(
						diagnostic,
						Status::UnsupportedExecution,
						"checker mapped, mask, UV or non-solid render control is unavailable",
						node.Id
					);
					return diagnostic.Code;
				}
				detail::CheckerControls control;
				control.Size = scalar("size", 0.5);
				control.Aspect = scalar("aspect", 1.0);
				control.AngleDegrees = scalar("angle", 0.0);
				if (const AuthoredValue *position = FindValue(node, "position"))
					control.Position = std::get<Vector2>(position->Data);
				if (const AuthoredValue *diagonal = FindValue(node, "diagonal"))
					control.Diagonal = std::get<bool>(diagonal->Data);
				if (const AuthoredValue *first = FindValue(node, "color_1"))
					control.First = std::get<Colour>(first->Data);
				if (const AuthoredValue *second = FindValue(node, "color_2"))
					control.Second = std::get<Colour>(second->Data);
				result.Width = width;
				result.Height = height;
				result.Pixels.resize(static_cast<size_t>(bytes));
				const detail::CheckerStatus status = detail::RenderChecker(result, control);
				if (status != detail::CheckerStatus::Ok) {
					SetDiagnostic(
						diagnostic,
						status == detail::CheckerStatus::InvalidImage ? Status::InvalidValue
																	  : Status::UnsupportedExecution,
						status == detail::CheckerStatus::UndefinedRange
							? "checker diagonal period is undefined"
							: "checker size or geometry is unsupported",
						node.Id,
						"size"
					);
					return diagnostic.Code;
				}
				result.Hash = detail::PixelHash(result);
			} else if (node.Type == "image.gradient") {
				const auto *gradient = std::get_if<Gradient>(&FindValue(node, "gradient")->Data);
				const uint32_t width =
					static_cast<uint32_t>(std::get<int64_t>(FindValue(node, "width")->Data));
				const uint32_t height =
					static_cast<uint32_t>(std::get<int64_t>(FindValue(node, "height")->Data));
				const uint64_t bytes = static_cast<uint64_t>(width) * height * 4;
				if (bytes > Limits::MaximumEvaluationBytes - evaluationBytes) {
					SetDiagnostic(
						diagnostic, Status::LimitExceeded, "gradient exceeds evaluation byte budget", node.Id
					);
					return diagnostic.Code;
				}
				const Image *uvMap = nullptr;
				const Image *angleMap = nullptr;
				const Image *radiusMap = nullptr;
				const Image *shiftMap = nullptr;
				const Image *scaleMap = nullptr;
				const Image *mask = nullptr;
				if (!resolveImage("uv_map", false, uvMap) || !resolveImage("angle_map", false, angleMap) ||
					!resolveImage("radius_map", false, radiusMap) ||
					!resolveImage("shift_map", false, shiftMap) ||
					!resolveImage("scale_map", false, scaleMap) || !resolveImage("mask", false, mask))
					return diagnostic.Code;
				if ((uvMap && (uvMap->Width != width || uvMap->Height != height)) ||
					(angleMap && (angleMap->Width != width || angleMap->Height != height)) ||
					(radiusMap && (radiusMap->Width != width || radiusMap->Height != height)) ||
					(shiftMap && (shiftMap->Width != width || shiftMap->Height != height)) ||
					(scaleMap && (scaleMap->Width != width || scaleMap->Height != height))) {
					SetDiagnostic(
						diagnostic,
						Status::UnsupportedExecution,
						"gradient maps require matching output dimensions",
						node.Id
					);
					return diagnostic.Code;
				}
				if (gradient->Mode > 6) {
					SetDiagnostic(
						diagnostic,
						Status::UnsupportedExecution,
						"gradient key interpolation mode is unavailable",
						node.Id,
						"gradient"
					);
					return diagnostic.Code;
				}
				const auto scalar = [&](std::string_view port, double fallback) {
					const AuthoredValue *value = FindValue(node, port);
					return value ? std::get<double>(value->Data) : fallback;
				};
				const auto integer = [&](std::string_view port, int64_t fallback) {
					const AuthoredValue *value = FindValue(node, port);
					return value ? std::get<int64_t>(value->Data) : fallback;
				};
				const auto vector = [&](std::string_view port, Vector2 fallback) {
					const AuthoredValue *value = FindValue(node, port);
					return value ? std::get<Vector2>(value->Data) : fallback;
				};
				const Vector2 center = vector("center", {0.5, 0.5});
				const Vector2 shape = vector("shape", {1.0, 1.0});
				detail::GradientGeometry geometry;
				geometry.Type = integer("type", 0);
				geometry.Loop = integer("loop", 0);
				geometry.AngleDegrees = scalar("angle", 0.0);
				const auto angleLink = std::find_if(
					plan.EffectiveLinks.begin(), plan.EffectiveLinks.end(), [&](const Link &link) {
						return link.ToNode == node.Id && link.ToPort == "angle_value";
					}
				);
				if (angleLink != plan.EffectiveLinks.end()) {
					const size_t sourceIndex = nodeIndices.at(angleLink->FromNode);
					const ValueOutputs *source =
						produced[sourceIndex] ? std::get_if<ValueOutputs>(&results[sourceIndex]) : nullptr;
					const AuthoredValue *value = nullptr;
					if (source) {
						const auto found =
							std::find_if(source->begin(), source->end(), [&](const AuthoredValue &item) {
								return item.Port == angleLink->FromPort;
							});
						if (found != source->end()) value = &*found;
					}
					const double *angle = value ? std::get_if<double>(&value->Data) : nullptr;
					if (!angle) {
						SetDiagnostic(
							diagnostic,
							Status::UnsupportedExecution,
							"gradient Angle needs a typed scalar source",
							node.Id,
							"angle_value"
						);
						return diagnostic.Code;
					}
					geometry.AngleDegrees = *angle;
				}
				geometry.Radius = scalar("radius", 0.5);
				geometry.CenterX = center.X;
				geometry.CenterY = center.Y;
				geometry.ShapeX = shape.X;
				geometry.ShapeY = shape.Y;
				const AuthoredValue *uniformValue = FindValue(node, "uniform_ratio");
				geometry.UniformRatio = uniformValue ? std::get<bool>(uniformValue->Data) : true;
				geometry.Shift = scalar("shift", 0.0);
				geometry.Scale = scalar("scale", 1.0);
				detail::GradientRenderControls controls;
				controls.UVMap = uvMap;
				controls.AngleMap = angleMap;
				controls.RadiusMap = radiusMap;
				controls.ShiftMap = shiftMap;
				controls.ScaleMap = scaleMap;
				controls.UVMix = scalar("uv_mix", 1.0);
				controls.AngleMaximum = scalar("angle_max", geometry.AngleDegrees);
				controls.RadiusMaximum = scalar("radius_max", geometry.Radius);
				controls.ShiftMaximum = scalar("shift_max", geometry.Shift);
				controls.ScaleMaximum = scalar("scale_max", geometry.Scale);
				controls.InverseAxis = scalar("inverse_axis", 0.0);
				const auto curve = [&](std::string_view port) -> const Curve * {
					const AuthoredValue *value = FindValue(node, port);
					return value ? &std::get<Curve>(value->Data) : nullptr;
				};
				controls.InverseCurve = curve("inverse_curve");
				controls.ProgressRemap = curve("progress_remap");
				controls.BrightnessCurve = curve("curve");
				controls.LevelIn = vector("level_in", {0.0, 1.0});
				controls.LevelOut = vector("level_out", {0.0, 1.0});
				std::vector<detail::GradientKey> keys;
				keys.reserve(gradient->Keys.size());
				for (const GradientKey &key : gradient->Keys)
					keys.push_back({key.Time, key.Color});
				result.Width = width;
				result.Height = height;
				result.Pixels.resize(static_cast<size_t>(bytes));
				const detail::GradientStatus status =
					detail::RenderGradientBase(result, geometry, keys, gradient->Mode, mask, controls);
				if (status != detail::GradientStatus::Ok) {
					const bool undefinedDivision = status == detail::GradientStatus::UndefinedDivision ||
												   status == detail::GradientStatus::UndefinedCmykBlack;
					SetDiagnostic(
						diagnostic,
						undefinedDivision ? Status::UnsupportedExecution : Status::InvalidValue,
						undefinedDivision ? (status == detail::GradientStatus::UndefinedCmykBlack
												 ? "gradient CMYK pure black has undefined division"
												 : "gradient control has undefined division")
										  : "gradient geometry or key data is invalid",
						node.Id,
						"gradient"
					);
					return diagnostic.Code;
				}
				result.Hash = detail::PixelHash(result);
			} else if (node.Type == "image.noise_simplex") {
				const uint32_t width =
					static_cast<uint32_t>(std::get<int64_t>(FindValue(node, "width")->Data));
				const uint32_t height =
					static_cast<uint32_t>(std::get<int64_t>(FindValue(node, "height")->Data));
				const uint64_t bytes = static_cast<uint64_t>(width) * height * 4;
				if (bytes > Limits::MaximumEvaluationBytes - evaluationBytes) {
					SetDiagnostic(
						diagnostic, Status::LimitExceeded, "simplex exceeds evaluation byte budget", node.Id
					);
					return diagnostic.Code;
				}
				const Image *uvMap = nullptr;
				const Image *mask = nullptr;
				if (!resolveImage("uv_map", false, uvMap) || !resolveImage("mask", false, mask))
					return diagnostic.Code;
				const auto integer = [&](std::string_view port, int64_t fallback) {
					const AuthoredValue *value = FindValue(node, port);
					return value ? std::get<int64_t>(value->Data) : fallback;
				};
				const auto scalar = [&](std::string_view port, double fallback) {
					const AuthoredValue *value = FindValue(node, port);
					return value ? std::get<double>(value->Data) : fallback;
				};
				const auto vector = [&](std::string_view port, Vector2 fallback) {
					const AuthoredValue *value = FindValue(node, port);
					return value ? std::get<Vector2>(value->Data) : fallback;
				};
				const AuthoredValue *mappedValue = FindValue(node, "mapped");
				if (uvMap || mask || FindValue(node, "uv_mix") ||
					(mappedValue && std::get<bool>(mappedValue->Data))) {
					SetDiagnostic(
						diagnostic,
						Status::UnsupportedExecution,
						"simplex UV, mask or mapped control is unavailable",
						node.Id
					);
					return diagnostic.Code;
				}
				if (integer("color_mode", 0) != 0 || FindValue(node, "color_range_r") ||
					FindValue(node, "color_range_g") || FindValue(node, "color_range_b")) {
					SetDiagnostic(
						diagnostic,
						Status::UnsupportedExecution,
						"simplex color mode is unavailable",
						node.Id,
						"color_mode"
					);
					return diagnostic.Code;
				}
				detail::SimplexControls control;
				control.Seed = scalar("seed", 0.0);
				control.Iterations = integer("iterations", 1);
				const AuthoredValue *tileValue = FindValue(node, "tile");
				control.Tile = tileValue ? std::get<bool>(tileValue->Data) : true;
				const Vector2 position = vector("position", {0.0, 0.0});
				control.Position = {position.X, position.Y};
				control.RotationRadians = scalar("rotation", 0.0) * std::acos(-1.0) / 180.0;
				const Vector2 scale = vector("scale", {0.25, 0.25});
				control.Scale = {scale.X, scale.Y};
				control.IterationScaling = scalar("iteration_scaling", 2.0);
				control.IterationAmplitude = scalar("iteration_amplitude", 0.5);
				const Vector2 levelIn = vector("level_in", {0.0, 1.0});
				const Vector2 levelOut = vector("level_out", {0.0, 1.0});
				control.LevelIn = {levelIn.X, levelIn.Y};
				control.LevelOut = {levelOut.X, levelOut.Y};
				result.Width = width;
				result.Height = height;
				result.Pixels.resize(static_cast<size_t>(bytes));
				const detail::NoiseStatus status = detail::SimplexGrey(control, result);
				if (status != detail::NoiseStatus::Ok) {
					SetDiagnostic(
						diagnostic, Status::InvalidValue, "simplex controls are invalid or undefined", node.Id
					);
					return diagnostic.Code;
				}
				result.Hash = detail::PixelHash(result);
			} else if (node.Type == "image.tile") {
				const Image *source = nullptr;
				const Image *uvMap = nullptr;
				if (!resolveImage("image", true, source) || !resolveImage("uv_map", false, uvMap))
					return diagnostic.Code;
				if (uvMap || FindValue(node, "uv_mix")) {
					SetDiagnostic(
						diagnostic,
						Status::UnsupportedExecution,
						"tile UV Map is unavailable",
						node.Id,
						"uv_map"
					);
					return diagnostic.Code;
				}
				const auto integer = [&](std::string_view port, int64_t fallback) {
					const AuthoredValue *value = FindValue(node, port);
					return value ? std::get<int64_t>(value->Data) : fallback;
				};
				const auto scalar = [&](std::string_view port, double fallback) {
					const AuthoredValue *value = FindValue(node, port);
					return value ? std::get<double>(value->Data) : fallback;
				};
				const auto vector = [&](std::string_view port, Vector2 fallback) {
					const AuthoredValue *value = FindValue(node, port);
					return value ? std::get<Vector2>(value->Data) : fallback;
				};
				const int64_t scaling = integer("scaling_type", 0);
				double requestedWidth = 0.0;
				double requestedHeight = 0.0;
				if (scaling == 0) {
					requestedWidth = integer("width", 0);
					requestedHeight = integer("height", 0);
				} else {
					const Vector2 amount = vector("amount", {2.0, 2.0});
					requestedWidth = source->Width * amount.X;
					requestedHeight = source->Height * amount.Y;
				}
				if (!std::isfinite(requestedWidth) || !std::isfinite(requestedHeight) ||
					requestedWidth < 1.0 || requestedHeight < 1.0 ||
					requestedWidth > Limits::MaximumDimension || requestedHeight > Limits::MaximumDimension ||
					std::trunc(requestedWidth) != requestedWidth ||
					std::trunc(requestedHeight) != requestedHeight) {
					SetDiagnostic(
						diagnostic,
						Status::LimitExceeded,
						"tile output dimension is invalid or too large",
						node.Id
					);
					return diagnostic.Code;
				}
				const uint64_t bytes =
					static_cast<uint64_t>(requestedWidth) * static_cast<uint64_t>(requestedHeight) * 4;
				if (bytes > Limits::MaximumOutputBytes ||
					bytes > Limits::MaximumEvaluationBytes - evaluationBytes) {
					SetDiagnostic(
						diagnostic, Status::LimitExceeded, "tile output exceeds native byte budget", node.Id
					);
					return diagnostic.Code;
				}
				const Vector2 spacing = vector("spacing", {0.0, 0.0});
				const Vector2 position = vector("position", {0.0, 0.0});
				const Vector2 scale = vector("scale", {1.0, 1.0});
				detail::TileControls control;
				control.Spacing = {spacing.X, spacing.Y};
				control.Position = {position.X, position.Y};
				control.RotationRadians = scalar("rotation", 0.0) * std::acos(-1.0) / 180.0;
				control.Scale = {scale.X, scale.Y};
				control.ShiftAxis = integer("shift_axis", 0);
				control.Shift = scalar("shift", 0.0);
				control.Pattern = integer("pattern", 0);
				result.Width = static_cast<uint32_t>(requestedWidth);
				result.Height = static_cast<uint32_t>(requestedHeight);
				result.Pixels.resize(static_cast<size_t>(bytes));
				if (detail::TileImage(*source, result, control) != detail::TileStatus::Ok) {
					SetDiagnostic(
						diagnostic, Status::InvalidValue, "tile controls are invalid or undefined", node.Id
					);
					return diagnostic.Code;
				}
				result.Hash = detail::PixelHash(result);
			} else if (node.Type == "image.blend") {
				const Image *background = nullptr;
				const Image *foreground = nullptr;
				const Image *mask = nullptr;
				if (!resolveImage("background", true, background) ||
					!resolveImage("foreground", false, foreground) || !resolveImage("mask", false, mask))
					return diagnostic.Code;
				const auto authoredBool = [&](std::string_view port, bool fallback) {
					const AuthoredValue *value = FindValue(node, port);
					return value ? std::get<bool>(value->Data) : fallback;
				};
				const auto authoredInteger = [&](std::string_view port, int64_t fallback) {
					const AuthoredValue *value = FindValue(node, port);
					return value ? std::get<int64_t>(value->Data) : fallback;
				};
				const auto authoredScalar = [&](std::string_view port, double fallback) {
					const AuthoredValue *value = FindValue(node, port);
					return value ? std::get<double>(value->Data) : fallback;
				};
				if (authoredBool("swap", false)) std::swap(background, foreground);
				if (background == nullptr) {
					SetDiagnostic(
						diagnostic,
						Status::UnsupportedExecution,
						"swapped blend has no background image",
						node.Id,
						"swap"
					);
					return diagnostic.Code;
				}
				uint32_t width = background->Width;
				uint32_t height = background->Height;
				const int64_t dimension = authoredInteger("output_dimension", 0);
				if (dimension == 1) {
					width = foreground ? foreground->Width : 1;
					height = foreground ? foreground->Height : 1;
				} else if (dimension == 2) {
					width = mask ? mask->Width : 1;
					height = mask ? mask->Height : 1;
				} else if (dimension == 3) {
					width = std::max({width, foreground ? foreground->Width : 1u, mask ? mask->Width : 1u});
					height =
						std::max({height, foreground ? foreground->Height : 1u, mask ? mask->Height : 1u});
				} else if (dimension == 4) {
					const Vector2 size = std::get<Vector2>(FindValue(node, "constant_dimension")->Data);
					width = static_cast<uint32_t>(size.X);
					height = static_cast<uint32_t>(size.Y);
				}
				const uint64_t bytes = static_cast<uint64_t>(width) * height * 4;
				if (bytes > Limits::MaximumOutputBytes ||
					bytes > Limits::MaximumEvaluationBytes - evaluationBytes) {
					SetDiagnostic(
						diagnostic, Status::LimitExceeded, "blend output exceeds byte budget", node.Id
					);
					return diagnostic.Code;
				}
				result.Width = width;
				result.Height = height;
				result.Pixels.resize(static_cast<size_t>(bytes));
				const bool invertMask = authoredBool("invert_mask", false);
				const bool maskAlphaOnly = authoredBool("mask_alpha_only", false);
				const double feather = authoredScalar("mask_feather", 1.0);
				Image modifiedMask;
				if (mask != nullptr) {
					const uint64_t radius = std::max<int64_t>(1, std::lround(feather));
					const uint64_t samples =
						static_cast<uint64_t>(mask->Width) * mask->Height * (2 * radius - 1) * 2;
					if (samples > 64'000'000 ||
						mask->Pixels.size() >
							(Limits::MaximumEvaluationBytes - evaluationBytes - bytes) / 3) {
						SetDiagnostic(
							diagnostic,
							Status::LimitExceeded,
							"blend mask processing exceeds native budget",
							node.Id,
							"mask_feather"
						);
						return diagnostic.Code;
					}
					modifiedMask = detail::ModifyBlendMask(*mask, invertMask, maskAlphaOnly, feather);
					mask = &modifiedMask;
				}
				const AuthoredValue *positionValue = FindValue(node, "position");
				const Vector2 position =
					positionValue ? std::get<Vector2>(positionValue->Data) : Vector2{0.5, 0.5};
				const detail::BlendPixelStatus blendStatus = detail::BlendCanvas(
					*background,
					foreground,
					mask,
					result,
					authoredInteger("blend_mode", 0),
					authoredScalar("opacity", 1.0),
					authoredBool("preserve_alpha", false),
					authoredInteger("fill_mode", 0),
					position,
					maskAlphaOnly
				);
				if (blendStatus != detail::BlendPixelStatus::Ok) {
					SetDiagnostic(
						diagnostic,
						Status::UnsupportedExecution,
						"blend shader equation is undefined for these pixels",
						node.Id,
						"blend_mode"
					);
					return diagnostic.Code;
				}
				result.Hash = detail::PixelHash(result);
			} else if (node.Type == "image.height_blend") {
				const AuthoredValue *modeValue = FindValue(node, "mode");
				const AuthoredValue *typeValue = FindValue(node, "type");
				const AuthoredValue *factorValue = FindValue(node, "factor");
				const int64_t mode = modeValue ? std::get<int64_t>(modeValue->Data) : 0;
				const int64_t type = typeValue ? std::get<int64_t>(typeValue->Data) : 1;
				const double factor = factorValue ? std::get<double>(factorValue->Data) : 0.5;
				const AuthoredValue *processValue = FindValue(node, "array_process");
				const int64_t process = processValue ? std::get<int64_t>(processValue->Data) : 0;
				if (process < 0 || process > 3) {
					SetDiagnostic(
						diagnostic,
						Status::InvalidValue,
						"array process mode must be 0, 1, 2 or 3",
						node.Id,
						"array_process"
					);
					return diagnostic.Code;
				}
				std::array<const NodeResult *, 2> inputs{};
				std::array<const Image *, 2> directImages{};
				std::array<size_t, 2> lengths{};
				for (size_t input = 0; input < inputs.size(); input++) {
					const char *port = input == 0 ? "background" : "foreground";
					const auto link = std::find_if(
						plan.EffectiveLinks.begin(), plan.EffectiveLinks.end(), [&](const Link &candidate) {
							return candidate.ToNode == node.Id && candidate.ToPort == port;
						}
					);
					if (link == plan.EffectiveLinks.end() || !produced[nodeIndices.at(link->FromNode)]) {
						SetDiagnostic(
							diagnostic,
							Status::InvalidValue,
							"height blend image input is unavailable",
							node.Id,
							port
						);
						return diagnostic.Code;
					}
					inputs[input] = &results[nodeIndices.at(link->FromNode)];
					directImages[input] = FindImageOutput(*inputs[input], link->FromPort);
					if (const auto *array = std::get_if<ImageArray>(inputs[input])) {
						lengths[input] = array->Items.size();
						producedArray = true;
					} else
						lengths[input] = 1;
				}
				std::vector<std::vector<size_t>> schedule;
				const Status scheduleStatus = detail::BuildArraySchedule(
					lengths,
					static_cast<detail::ArrayProcessMode>(process),
					Limits::MaximumArrayElements,
					schedule
				);
				if (scheduleStatus != Status::Ok) {
					SetDiagnostic(
						diagnostic,
						scheduleStatus,
						"height blend array schedule exceeds native bounds or has an empty input",
						node.Id,
						"array_process"
					);
					return diagnostic.Code;
				}
				uint64_t outputBytes = 0;
				for (const auto &row : schedule) {
					std::array<const Image *, 2> selected{};
					for (size_t input = 0; input < selected.size(); input++) {
						if (directImages[input])
							selected[input] = directImages[input];
						else {
							const ImageArray &array = std::get<ImageArray>(*inputs[input]);
							const auto *leaf = std::get_if<size_t>(&array.Items[row[input]].Data);
							if (!leaf) {
								SetDiagnostic(
									diagnostic,
									Status::UnsupportedExecution,
									"height blend cannot process a nested image array",
									node.Id,
									input == 0 ? "background" : "foreground"
								);
								return diagnostic.Code;
							}
							selected[input] = &array.Images[*leaf];
						}
					}
					const uint64_t bytes = selected[0]->Pixels.size();
					if (bytes > Limits::MaximumOutputBytes - outputBytes ||
						bytes > Limits::MaximumEvaluationBytes - evaluationBytes - outputBytes) {
						SetDiagnostic(
							diagnostic,
							Status::LimitExceeded,
							"height blend array exceeds byte budget",
							node.Id,
							"image"
						);
						return diagnostic.Code;
					}
					outputBytes += bytes;
					Image frame;
					frame.Width = selected[0]->Width;
					frame.Height = selected[0]->Height;
					frame.Pixels.resize(static_cast<size_t>(bytes));
					if (!detail::BlendHeight(*selected[0], *selected[1], frame, mode, type, factor)) {
						SetDiagnostic(
							diagnostic,
							Status::UnsupportedExecution,
							"height blend formula is undefined for these pixels and controls",
							node.Id
						);
						return diagnostic.Code;
					}
					frame.Hash = detail::PixelHash(frame);
					if (producedArray) {
						arrayResult.Items.push_back(ImageArrayItem{arrayResult.Images.size()});
						arrayResult.Images.push_back(std::move(frame));
					} else
						result = std::move(frame);
				}
			} else if (node.Type == "image.passthrough" || node.Type == "image.flip" ||
					   node.Type == "image.invert" || node.Type == "image.alpha_cutoff" ||
					   node.Type == "image.offset" || node.Type == "image.threshold" ||
					   node.Type == "image.posterize") {
				const auto link = std::find_if(
					plan.EffectiveLinks.begin(), plan.EffectiveLinks.end(), [&](const Link &candidate) {
						return candidate.ToNode == node.Id && candidate.ToPort == "image";
					}
				);
				if (link == plan.EffectiveLinks.end()) {
					SetDiagnostic(
						diagnostic, Status::InvalidValue, "image input is missing", node.Id, "image"
					);
					return diagnostic.Code;
				}
				const size_t sourceIndex = nodeIndices.at(link->FromNode);
				if (!produced[sourceIndex]) {
					SetDiagnostic(
						diagnostic, Status::InvalidOutput, "image source was not evaluated", node.Id, "image"
					);
					return diagnostic.Code;
				}
				const Image *sourceImage = FindImageOutput(results[sourceIndex], link->FromPort);
				if (!sourceImage) {
					SetDiagnostic(
						diagnostic,
						Status::UnsupportedExecution,
						"image input received an array",
						node.Id,
						"image"
					);
					return diagnostic.Code;
				}
				const bool maskedFilter = node.Type != "image.passthrough";
				const Image *mask = nullptr;
				if (maskedFilter) {
					const auto maskLink = std::find_if(
						plan.EffectiveLinks.begin(), plan.EffectiveLinks.end(), [&](const Link &candidate) {
							return candidate.ToNode == node.Id && candidate.ToPort == "mask";
						}
					);
					if (maskLink != plan.EffectiveLinks.end()) {
						const size_t maskIndex = nodeIndices.at(maskLink->FromNode);
						if (!produced[maskIndex]) {
							SetDiagnostic(
								diagnostic,
								Status::InvalidOutput,
								"mask source was not evaluated",
								node.Id,
								"mask"
							);
							return diagnostic.Code;
						}
						mask = FindImageOutput(results[maskIndex], maskLink->FromPort);
						if (!mask) {
							SetDiagnostic(
								diagnostic,
								Status::UnsupportedExecution,
								"mask input received an array",
								node.Id,
								"mask"
							);
							return diagnostic.Code;
						}
					}
				}
				if (sourceImage->Pixels.size() > Limits::MaximumEvaluationBytes - evaluationBytes) {
					SetDiagnostic(
						diagnostic,
						Status::LimitExceeded,
						"evaluation intermediates exceed the byte budget",
						node.Id
					);
					return diagnostic.Code;
				}
				result = *sourceImage;
				const AuthoredValue *mixValue = FindValue(node, "mix");
				const double mix = mixValue ? std::get<double>(mixValue->Data) : 1.0;
				const AuthoredValue *invertMaskValue = FindValue(node, "invert_mask");
				const bool invertMask = invertMaskValue ? std::get<bool>(invertMaskValue->Data) : false;
				const AuthoredValue *featherValue = FindValue(node, "mask_feather");
				const double feather = featherValue ? std::get<double>(featherValue->Data) : 0.0;
				Image featheredMask;
				if (mask != nullptr && feather > 0.0) {
					const uint64_t radius = std::max<int64_t>(1, std::lround(feather));
					const uint64_t sampleCount =
						static_cast<uint64_t>(mask->Width) * mask->Height * (2 * radius - 1) * 2;
					if (sampleCount > 64'000'000) {
						SetDiagnostic(
							diagnostic,
							Status::LimitExceeded,
							"mask feather exceeds work budget",
							node.Id,
							"mask_feather"
						);
						return diagnostic.Code;
					}
					if (mask->Pixels.size() >
						(Limits::MaximumEvaluationBytes - evaluationBytes - result.Pixels.size()) / 2) {
						SetDiagnostic(
							diagnostic,
							Status::LimitExceeded,
							"mask feather exceeds byte budget",
							node.Id,
							"mask_feather"
						);
						return diagnostic.Code;
					}
					featheredMask = detail::FeatherMask(*mask, feather);
					mask = &featheredMask;
				}
				const AuthoredValue *channelValue = FindValue(node, "channel");
				const int64_t channels = channelValue ? std::get<int64_t>(channelValue->Data) : 15;
				if (node.Type == "image.flip") {
					const int64_t axis = std::get<int64_t>(FindValue(node, "axis")->Data);
					detail::Flip(*sourceImage, result, axis);
					detail::ApplyMaskMix(*sourceImage, result, mask, mix, invertMask);
					detail::ApplyChannels(*sourceImage, result, channels);
				} else if (node.Type == "image.invert") {
					const bool includeAlpha = std::get<bool>(FindValue(node, "include_alpha")->Data);
					detail::Invert(result, includeAlpha);
					detail::ApplyMaskMix(*sourceImage, result, mask, mix, invertMask);
					detail::ApplyChannels(*sourceImage, result, channels);
				} else if (node.Type == "image.alpha_cutoff") {
					const double minimum = std::get<double>(FindValue(node, "minimum")->Data);
					detail::AlphaCutoff(result, minimum);
					detail::ApplyMaskMix(*sourceImage, result, mask, mix, invertMask);
				} else if (node.Type == "image.offset") {
					const auto scalar = [&](std::string_view port, double fallback) {
						const AuthoredValue *value = FindValue(node, port);
						return value ? std::get<double>(value->Data) : fallback;
					};
					const detail::BasicFilterStatus status = detail::OffsetImage(
						*sourceImage,
						result,
						scalar("x_offset", 0.0),
						scalar("y_offset", 0.0),
						scalar("angle", 0.0)
					);
					if (status != detail::BasicFilterStatus::Ok) {
						SetDiagnostic(
							diagnostic,
							Status::UnsupportedExecution,
							"offset cannot sample these controls",
							node.Id
						);
						return diagnostic.Code;
					}
					detail::ApplyMaskMix(*sourceImage, result, mask, mix, invertMask);
				} else if (node.Type == "image.threshold") {
					const auto integer = [&](std::string_view port, int64_t fallback) {
						const AuthoredValue *value = FindValue(node, port);
						return value ? std::get<int64_t>(value->Data) : fallback;
					};
					const auto scalar = [&](std::string_view port, double fallback) {
						const AuthoredValue *value = FindValue(node, port);
						return value ? std::get<double>(value->Data) : fallback;
					};
					const auto boolean = [&](std::string_view port, bool fallback) {
						const AuthoredValue *value = FindValue(node, port);
						return value ? std::get<bool>(value->Data) : fallback;
					};
					if (integer("algorithm", 0) != 0) {
						SetDiagnostic(
							diagnostic,
							Status::UnsupportedExecution,
							"adaptive threshold requires a native sampler",
							node.Id,
							"algorithm"
						);
						return diagnostic.Code;
					}
					const detail::SimpleThreshold control{
						boolean("brightness", false),
						scalar("brightness_threshold", 0.5),
						scalar("brightness_smoothness", 0.0),
						boolean("brightness_invert", false),
						boolean("brightness_multiply", false),
						integer("apply_to_alpha", 0),
						boolean("alpha", false),
						scalar("alpha_threshold", 0.5),
						scalar("alpha_smoothness", 0.0),
						boolean("alpha_invert", false)
					};
					if (detail::ThresholdImage(*sourceImage, result, control) !=
						detail::BasicFilterStatus::Ok) {
						SetDiagnostic(
							diagnostic,
							Status::UnsupportedExecution,
							"threshold cannot sample these controls",
							node.Id
						);
						return diagnostic.Code;
					}
					detail::ApplyMaskMix(*sourceImage, result, mask, mix, invertMask);
					detail::ApplyChannels(*sourceImage, result, integer("channel", 15));
				} else if (node.Type == "image.posterize") {
					const auto boolean = [&](std::string_view port, bool fallback) {
						const AuthoredValue *value = FindValue(node, port);
						return value ? std::get<bool>(value->Data) : fallback;
					};
					if (boolean("use_palette", true)) {
						const AuthoredValue *paletteValue = FindValue(node, "palette");
						const auto *paletteArray =
							paletteValue ? std::get_if<ArrayValue>(&paletteValue->Data) : nullptr;
						detail::PaletteValue palette;
						if (paletteArray == nullptr || !detail::MakePalette(*paletteArray, palette)) {
							SetDiagnostic(
								diagnostic,
								Status::InvalidValue,
								"posterize palette is invalid",
								node.Id,
								"palette"
							);
							return diagnostic.Code;
						}
						if (detail::PosterizeWithPalette(
								*sourceImage, result, palette, boolean("posterize_alpha", true)
							) != detail::PaletteStatus::Ok) {
							SetDiagnostic(
								diagnostic,
								Status::UnsupportedExecution,
								"posterize palette cannot process this image",
								node.Id,
								"palette"
							);
							return diagnostic.Code;
						}
						detail::ApplyMaskMix(*sourceImage, result, mask, mix, invertMask);
					} else {
						const AuthoredValue *stepsValue = FindValue(node, "steps");
						const AuthoredValue *gammaValue = FindValue(node, "gamma");
						const detail::NonPalettePosterize control{
							stepsValue ? std::get<int64_t>(stepsValue->Data) : 4,
							gammaValue ? std::get<double>(gammaValue->Data) : 1.0,
							boolean("use_global_range", true),
							boolean("posterize_alpha", true)
						};
						const detail::BasicFilterStatus status =
							detail::PosterizeWithoutPalette(*sourceImage, result, control);
						if (status != detail::BasicFilterStatus::Ok) {
							SetDiagnostic(
								diagnostic,
								Status::UnsupportedExecution,
								"posterize range is undefined for these pixels",
								node.Id,
								"use_global_range"
							);
							return diagnostic.Code;
						}
						detail::ApplyMaskMix(*sourceImage, result, mask, mix, invertMask);
					}
				}
				if (node.Type != "image.passthrough") {
					result.Hash = detail::PixelHash(result);
				}
			} else if (node.Type == "value.array") {
				producedArray = true;
				uint64_t arrayBytes = 0;
				std::vector<ImageArrayItem> inputItems;
				for (const DynamicInput &input : node.DynamicInputs) {
					if (input.Type != ValueType::Image && input.Type != ValueType::Array) {
						SetDiagnostic(
							diagnostic,
							Status::UnsupportedExecution,
							"CPU image array needs image elements",
							node.Id,
							input.Id
						);
						return diagnostic.Code;
					}
					const auto link = std::find_if(
						plan.EffectiveLinks.begin(), plan.EffectiveLinks.end(), [&](const Link &candidate) {
							return candidate.ToNode == node.Id && candidate.ToPort == input.Id;
						}
					);
					if (link == plan.EffectiveLinks.end()) {
						SetDiagnostic(
							diagnostic,
							Status::InvalidValue,
							"image array input is unconnected",
							node.Id,
							input.Id
						);
						return diagnostic.Code;
					}
					const size_t sourceIndex = nodeIndices.at(link->FromNode);
					if (!produced[sourceIndex]) {
						SetDiagnostic(
							diagnostic,
							Status::InvalidOutput,
							"image array source was not evaluated",
							node.Id,
							input.Id
						);
						return diagnostic.Code;
					}
					const auto appendImage = [&](const Image &image) {
						if (arrayResult.Images.size() == Limits::MaximumArrayElements ||
							image.Pixels.size() > Limits::MaximumOutputBytes - arrayBytes ||
							image.Pixels.size() >
								Limits::MaximumEvaluationBytes - evaluationBytes - arrayBytes)
							return false;
						arrayBytes += image.Pixels.size();
						arrayResult.Images.push_back(image);
						return true;
					};
					if (const Image *image = FindImageOutput(results[sourceIndex], link->FromPort)) {
						if (!appendImage(*image)) {
							SetDiagnostic(
								diagnostic,
								Status::LimitExceeded,
								"image array exceeds byte budget",
								node.Id,
								input.Id
							);
							return diagnostic.Code;
						}
						inputItems.push_back(ImageArrayItem{arrayResult.Images.size() - 1});
					} else {
						const ImageArray *source = std::get_if<ImageArray>(&results[sourceIndex]);
						if (!source) {
							SetDiagnostic(
								diagnostic,
								Status::TypeMismatch,
								"image array input received a value array",
								node.Id,
								input.Id
							);
							return diagnostic.Code;
						}
						const ImageArray &sourceArray = *source;
						const size_t offset = arrayResult.Images.size();
						for (const Image &image : sourceArray.Images) {
							if (!appendImage(image)) {
								SetDiagnostic(
									diagnostic,
									Status::LimitExceeded,
									"image array exceeds byte budget",
									node.Id,
									input.Id
								);
								return diagnostic.Code;
							}
						}
						std::vector<ImageArrayItem> children;
						for (const ImageArrayItem &item : sourceArray.Items)
							children.push_back(RebaseItem(item, offset));
						inputItems.push_back(ImageArrayItem{std::move(children)});
					}
				}
				const AuthoredValue *spreadValue = FindValue(node, "spread");
				const bool spread = spreadValue ? std::get<bool>(spreadValue->Data) : false;
				const Status collectStatus = detail::CollectArray<size_t>(
					inputItems, spread, Limits::MaximumArrayElements, arrayResult.Items
				);
				if (collectStatus != Status::Ok) {
					SetDiagnostic(
						diagnostic, collectStatus, "image array shape exceeds native bounds", node.Id, "array"
					);
					return diagnostic.Code;
				}
			} else if (node.Type == "value.array_get") {
				const auto link = std::find_if(
					plan.EffectiveLinks.begin(), plan.EffectiveLinks.end(), [&](const Link &candidate) {
						return candidate.ToNode == node.Id && candidate.ToPort == "array";
					}
				);
				if (link == plan.EffectiveLinks.end()) {
					SetDiagnostic(
						diagnostic, Status::InvalidValue, "array get has no input", node.Id, "array"
					);
					return diagnostic.Code;
				}
				const size_t sourceIndex = nodeIndices.at(link->FromNode);
				const ImageArray *sourceArray =
					produced[sourceIndex] ? std::get_if<ImageArray>(&results[sourceIndex]) : nullptr;
				if (!sourceArray) {
					SetDiagnostic(
						diagnostic,
						Status::UnsupportedExecution,
						"array get needs an image array",
						node.Id,
						"array"
					);
					return diagnostic.Code;
				}
				const int64_t length = static_cast<int64_t>(sourceArray->Items.size());
				if (length == 0) {
					SetDiagnostic(
						diagnostic,
						Status::InvalidValue,
						"array get cannot read an empty array",
						node.Id,
						"array"
					);
					return diagnostic.Code;
				}
				const AuthoredValue *indexValue = FindValue(node, "index");
				const AuthoredValue *overflowValue = FindValue(node, "overflow");
				int64_t selected = indexValue ? std::get<int64_t>(indexValue->Data) : 0;
				const int64_t overflow = overflowValue ? std::get<int64_t>(overflowValue->Data) : 0;
				if (overflow == 0) {
					if (selected < 0 && selected >= -length) selected += length;
					selected = std::clamp<int64_t>(selected, 0, length - 1);
				} else if (overflow == 1) {
					selected = ((selected % length) + length) % length;
				} else {
					const int64_t period = length == 1 ? 1 : 2 * (length - 1);
					selected = ((selected % period) + period) % period;
					if (selected >= length) selected = period - selected;
				}
				const auto *imageIndex =
					std::get_if<size_t>(&sourceArray->Items[static_cast<size_t>(selected)].Data);
				if (!imageIndex) {
					SetDiagnostic(
						diagnostic,
						Status::UnsupportedExecution,
						"array get selected a nested array",
						node.Id,
						"array"
					);
					return diagnostic.Code;
				}
				result = sourceArray->Images[*imageIndex];
			} else {
				SetDiagnostic(
					diagnostic,
					Status::UnsupportedExecution,
					"CPU evaluator has no node implementation",
					node.Id
				);
				return diagnostic.Code;
			}
			const uint64_t resultBytes = producedValue		  ? ResultBytes(NodeResult{valueResult})
										 : producedArray	  ? ResultBytes(arrayResult)
										 : producedShape	  ? ResultBytes(NodeResult{shapeResult})
										 : producedConversion ? ResultBytes(NodeResult{conversionResult})
										 : producedMirror	  ? ResultBytes(NodeResult{mirrorResult})
															  : result.Pixels.size();
			if (resultBytes > Limits::MaximumEvaluationBytes - evaluationBytes) {
				SetDiagnostic(
					diagnostic,
					Status::LimitExceeded,
					"evaluation intermediates exceed the byte budget",
					node.Id
				);
				return diagnostic.Code;
			}
			evaluationBytes += resultBytes;
			if (producedValue)
				results[index] = std::move(valueResult);
			else if (producedArray)
				results[index] = std::move(arrayResult);
			else if (producedShape)
				results[index] = std::move(shapeResult);
			else if (producedConversion)
				results[index] = std::move(conversionResult);
			else if (producedMirror)
				results[index] = std::move(mirrorResult);
			else
				results[index] = std::move(result);
			produced[index] = 1;
			// A large chain needs only the current input and output in memory.
			for (const size_t source : upstream[index]) {
				if (--remainingConsumers[source] == 0 && source != targetIndex) {
					evaluationBytes -= ResultBytes(results[source]);
					results[source] = Image{};
				}
			}
		}
		if (!produced[targetIndex]) {
			SetDiagnostic(
				diagnostic,
				Status::UnsupportedExecution,
				"selected output was not evaluated",
				output->NodeId,
				output->Port
			);
			return diagnostic.Code;
		}
		if (std::holds_alternative<MirrorOutputs>(results[targetIndex])) {
			MirrorOutputs &mirror = std::get<MirrorOutputs>(results[targetIndex]);
			Image *selected = output->Port == "image"		  ? &mirror.Colored
							  : output->Port == "mirror_mask" ? &mirror.Mask
															  : nullptr;
			if (!selected) {
				SetDiagnostic(
					diagnostic,
					Status::InvalidOutput,
					"mirror output port is unknown",
					output->NodeId,
					output->Port
				);
				return diagnostic.Code;
			}
			outputValue = std::move(*selected);
		} else if (std::holds_alternative<ConversionOutputs>(results[targetIndex])) {
			ConversionOutputs &conversion = std::get<ConversionOutputs>(results[targetIndex]);
			const Image *selected = FindImageOutput(results[targetIndex], output->Port);
			if (!selected) {
				SetDiagnostic(
					diagnostic,
					Status::InvalidOutput,
					"conversion output port is unknown",
					output->NodeId,
					output->Port
				);
				return diagnostic.Code;
			}
			const size_t index = static_cast<size_t>(selected - conversion.Channels.data());
			outputValue = std::move(conversion.Channels[index]);
		} else if (std::holds_alternative<ShapeOutputs>(results[targetIndex])) {
			ShapeOutputs &shape = std::get<ShapeOutputs>(results[targetIndex]);
			Image *selected = output->Port == "colored"	 ? &shape.Colored
							  : output->Port == "mask"	 ? &shape.Mask
							  : output->Port == "height" ? &shape.Height
							  : output->Port == "uv"	 ? &shape.UV
														 : nullptr;
			if (!selected) {
				SetDiagnostic(
					diagnostic,
					Status::InvalidOutput,
					"shape output port is unknown",
					output->NodeId,
					output->Port
				);
				return diagnostic.Code;
			}
			outputValue = std::move(*selected);
		} else
			outputValue = std::move(results[targetIndex]);
		diagnostic = {};
		return Status::Ok;
	}

	Status ValidateTickRange(const TickRange &range, size_t &frameCount, Diagnostic &diagnostic) {
		frameCount = 0;
		if (range.Step == 0 || range.First > range.Last) {
			SetDiagnostic(
				diagnostic,
				Status::InvalidValue,
				"tick range needs an ordered interval and positive step",
				{},
				"tick"
			);
			return diagnostic.Code;
		}
		if (range.Last > Limits::MaximumTick) {
			SetDiagnostic(
				diagnostic, Status::LimitExceeded, "tick range exceeds the fixed timeline limit", {}, "tick"
			);
			return diagnostic.Code;
		}
		const uint64_t count = 1 + (range.Last - range.First) / range.Step;
		if (count > Limits::MaximumRangeFrames) {
			SetDiagnostic(diagnostic, Status::LimitExceeded, "tick range has too many frames", {}, "tick");
			return diagnostic.Code;
		}
		frameCount = static_cast<size_t>(count);
		diagnostic = {};
		return Status::Ok;
	}

	static Status EvaluateResult(
		const Document &document,
		const Plan &plan,
		const std::string &outputId,
		const EvaluationRequest &request,
		NodeResult &outputValue,
		Diagnostic &diagnostic,
		Document *resolvedDocument = nullptr
	) {
		const uint64_t tick = request.Tick;
		if (tick > Limits::MaximumTick) {
			SetDiagnostic(
				diagnostic,
				Status::LimitExceeded,
				"evaluation tick exceeds the fixed timeline limit",
				{},
				"tick"
			);
			return diagnostic.Code;
		}
		if (!std::isfinite(request.Subframe) || request.Subframe < 0 || request.Subframe >= 1 ||
			(tick == Limits::MaximumTick && request.Subframe != 0)) {
			SetDiagnostic(
				diagnostic, Status::InvalidValue, "subframe must stay within the bounded tick", {}, "subframe"
			);
			return diagnostic.Code;
		}
		if (document.Keyframes.empty()) {
			if (resolvedDocument != nullptr) *resolvedDocument = document;
			return EvaluateGraph(document, plan, outputId, request, outputValue, diagnostic);
		}

		Plan currentPlan;
		const Status compileStatus = Compile(document, currentPlan, diagnostic);
		if (compileStatus != Status::Ok) return compileStatus;
		if (currentPlan != plan) {
			SetDiagnostic(
				diagnostic, Status::InvalidOutput, "compile plan does not match the authored document"
			);
			return diagnostic.Code;
		}
		const auto output =
			std::find_if(document.Outputs.begin(), document.Outputs.end(), [&](const Output &candidate) {
				return candidate.Id == outputId;
			});
		if (output == document.Outputs.end()) {
			SetDiagnostic(diagnostic, Status::InvalidOutput, "selected output does not exist", {}, outputId);
			return diagnostic.Code;
		}
		std::unordered_map<std::string, size_t> nodeIndices;
		for (size_t index = 0; index < document.Nodes.size(); index++) {
			nodeIndices.emplace(document.Nodes[index].Id, index);
		}
		std::vector<std::vector<size_t>> upstream(document.Nodes.size());
		for (const Link &link : plan.EffectiveLinks) {
			upstream[nodeIndices.at(link.ToNode)].push_back(nodeIndices.at(link.FromNode));
		}
		std::vector<uint8_t> needed(document.Nodes.size(), 0);
		std::vector<size_t> pending{nodeIndices.at(output->NodeId)};
		while (!pending.empty()) {
			const size_t index = pending.back();
			pending.pop_back();
			if (needed[index]) continue;
			needed[index] = 1;
			for (const size_t source : upstream[index])
				pending.push_back(source);
		}

		using PropertyKey = std::pair<size_t, std::string_view>;
		std::map<PropertyKey, std::vector<const Keyframe *>> tracks;
		for (const Keyframe &keyframe : document.Keyframes) {
			const size_t nodeIndex = nodeIndices.at(keyframe.NodeId);
			if (!needed[nodeIndex]) continue;
			tracks[{nodeIndex, keyframe.Port}].push_back(&keyframe);
		}
		if (tracks.empty()) {
			if (resolvedDocument != nullptr) *resolvedDocument = document;
			return EvaluateGraph(document, plan, outputId, request, outputValue, diagnostic);
		}
		std::map<std::pair<std::string_view, std::string_view>, const AnimationTrack *> configuredTracks;
		for (const AnimationTrack &track : document.Tracks)
			configuredTracks[{track.NodeId, track.Port}] = &track;
		Document resolved = document;
		for (auto &[property, keys] : tracks) {
			std::sort(keys.begin(), keys.end(), [](const Keyframe *left, const Keyframe *right) {
				return left->Tick < right->Tick;
			});
			Value value;
			const auto configured =
				configuredTracks.find({resolved.Nodes[property.first].Id, property.second});
			const Keyframe *left = nullptr;
			const Keyframe *right = nullptr;
			uint64_t numerator = 0;
			uint64_t denominator = 1;
			double fractionalRatio = 0;
			double driverRatio = 0.5;
			long double driverFrame = static_cast<long double>(tick) + request.Subframe;
			bool suppressDriver = false;
			if (configured == configuredTracks.end()) {
				const auto next =
					std::upper_bound(keys.begin(), keys.end(), tick, [](uint64_t value, const Keyframe *key) {
						return value < key->Tick;
					});
				if (next == keys.begin()) {
					left = keys.front();
				} else {
					left = *(next - 1);
					if (next != keys.end() && (left->Tick != tick || request.Subframe != 0)) {
						right = *next;
						numerator = tick - left->Tick;
						denominator = right->Tick - left->Tick;
						if (request.Subframe != 0)
							fractionalRatio = static_cast<double>(
								(static_cast<long double>(numerator) + request.Subframe) / denominator
							);
					}
				}
			} else {
				std::vector<uint64_t> keyTicks;
				keyTicks.reserve(keys.size());
				for (const Keyframe *key : keys)
					keyTicks.push_back(key->Tick);
				const AnimationTrack &track = *configured->second;
				const detail::KeyEnd end = track.End == "loop"	 ? detail::KeyEnd::Loop
										   : track.End == "ping" ? detail::KeyEnd::Ping
										   : track.End == "wrap" ? detail::KeyEnd::Wrap
																 : detail::KeyEnd::Hold;
				const size_t loopStart =
					track.LoopRange < 0 ? 0 : keys.size() - 1 - static_cast<size_t>(track.LoopRange);
				const uint64_t totalFrames =
					document.Timeline ? document.Timeline->Frames : keys.back()->Tick + 1;
				const long double sampleFrame = driverFrame;
				const long double firstFrame = static_cast<long double>(keyTicks[loopStart]);
				const long double lastFrame = static_cast<long double>(keyTicks.back());
				// The source bypasses range remapping for its single-key fast path.
				if (keys.size() > 1 && track.End == "loop" && sampleFrame > lastFrame) {
					const long double period = lastFrame - firstFrame + 1.0L;
					driverFrame = firstFrame + std::fmod(sampleFrame - lastFrame, period);
				} else if (keys.size() > 1 && track.End == "ping" && sampleFrame > lastFrame) {
					const long double duration = lastFrame - firstFrame;
					if (duration == 0.0L) {
						driverFrame = firstFrame;
					} else {
						const long double phase = std::fmod(sampleFrame - firstFrame, duration * 2.0L);
						driverFrame =
							phase < duration ? firstFrame + phase : firstFrame + duration * 2.0L - phase;
					}
				}
				if (request.Subframe == 0) {
					detail::KeySelection selection;
					if (!detail::SelectKeys(keyTicks, tick, totalFrames, end, loopStart, selection)) {
						SetDiagnostic(
							diagnostic,
							Status::InvalidValue,
							"keyframe time is outside the bounded track",
							resolved.Nodes[property.first].Id,
							std::string(property.second)
						);
						return diagnostic.Code;
					}
					left = keys[selection.From];
					if (selection.To != selection.From) right = keys[selection.To];
					numerator = selection.Numerator;
					denominator = selection.Denominator;
				} else {
					detail::FractionalKeySelection selection;
					if (!detail::SelectKeysFractional(
							keyTicks, tick, request.Subframe, totalFrames, end, loopStart, selection
						)) {
						SetDiagnostic(
							diagnostic,
							Status::InvalidValue,
							"fractional keyframe time is outside the bounded track",
							resolved.Nodes[property.first].Id,
							std::string(property.second)
						);
						return diagnostic.Code;
					}
					left = keys[selection.From];
					if (selection.To != selection.From) right = keys[selection.To];
					fractionalRatio = selection.Ratio;
				}
			}
			if (right) {
				driverRatio = request.Subframe == 0
								  ? static_cast<double>(numerator) / static_cast<double>(denominator)
								  : fractionalRatio;
			} else if (keys.size() > 1 && left != keys.back() && driverFrame == left->Tick) {
				driverRatio = 0.0;
			}
			const bool wrappingSegment = configured != configuredTracks.end() &&
										 configured->second->End == "wrap" && left == keys.back() &&
										 right == keys.front();
			const bool beforeFirst = !wrappingSegment && driverFrame < left->Tick;
			if (!wrappingSegment && keys.size() > 1 && left == keys.front() && driverFrame == 0.0L)
				suppressDriver = true;
			if (!right || left->Interpolation == "step" ||
				(left->Interpolation != "source" && left->Tick == tick)) {
				value = left->Data;
			} else if (left->Interpolation == "cubic") {
				SetDiagnostic(
					diagnostic,
					Status::UnsupportedExecution,
					"cubic keyframe needs authored tangent controls",
					resolved.Nodes[property.first].Id,
					std::string(property.second)
				);
				return diagnostic.Code;
			} else if (left->Interpolation == "source") {
				const auto side = [](const std::string &type) {
					return type == "bezier" ? detail::CurveSide::Bezier
						   : type == "cut"	? detail::CurveSide::Cut
											: detail::CurveSide::Linear;
				};
				const detail::KeyEase ease{
					side(left->Ease->OutType),
					side(right->Ease->InType),
					left->Ease->Out.X,
					left->Ease->Out.Y,
					right->Ease->In.X,
					right->Ease->In.Y
				};
				detail::KeyBlend blend;
				const double ratio =
					request.Subframe == 0 ? static_cast<double>(numerator) / denominator : fractionalRatio;
				if (!detail::EaseKeys(ease, ratio, blend)) {
					SetDiagnostic(
						diagnostic,
						Status::InvalidValue,
						"keyframe easing could not be evaluated",
						resolved.Nodes[property.first].Id,
						std::string(property.second)
					);
					return diagnostic.Code;
				}
				if (blend.Choice == detail::KeyChoice::From) {
					value = left->Data;
					suppressDriver = true;
				} else if (blend.Choice == detail::KeyChoice::To) {
					value = right->Data;
					suppressDriver = true;
				} else {
					const Status status =
						detail::InterpolateEased(left->Data, right->Data, blend.Ratio, value);
					if (status != Status::Ok) {
						SetDiagnostic(
							diagnostic,
							status,
							"source easing needs finite scalar, vector or colour values",
							resolved.Nodes[property.first].Id,
							std::string(property.second)
						);
						return diagnostic.Code;
					}
				}
			} else {
				const Status status =
					request.Subframe == 0
						? detail::Interpolate(left->Data, right->Data, numerator, denominator, value)
						: detail::InterpolateEased(left->Data, right->Data, fractionalRatio, value);
				if (status != Status::Ok) {
					SetDiagnostic(
						diagnostic,
						status,
						"linear interpolation needs finite scalar, vector or colour values",
						resolved.Nodes[property.first].Id,
						std::string(property.second)
					);
					return diagnostic.Code;
				}
			}
			if (left->SineDriver && !suppressDriver && !beforeFirst) {
				const KeyframeSineDriver &driver = *left->SineDriver;
				const uint64_t totalFrames =
					document.Timeline ? document.Timeline->Frames : keys.back()->Tick + 1;
				const double frameRate = driver.Frequency / static_cast<double>(totalFrames);
				const double wholeFrame = std::floor(static_cast<double>(driverFrame));
				const double subframe = static_cast<double>(driverFrame - wholeFrame);
				const double cycle = std::fmod(
					std::fmod(driver.Phase, 1.0) + std::fmod(frameRate, 1.0) * wholeFrame +
						std::fmod(frameRate * subframe, 1.0),
					1.0
				);
				constexpr double twoPi = 6.2831853071795864769252867665590057683943387987502;
				double envelope = 1.0;
				if (driver.Smooth > 0.0) {
					const double edge = std::clamp(
						std::min(
							{1.0,
							 2.0 * driverRatio / driver.Smooth,
							 2.0 * (1.0 - driverRatio) / driver.Smooth}
						),
						0.0,
						1.0
					);
					envelope = edge * edge * (3.0 - 2.0 * edge);
				}
				const double modulation = std::sin(cycle * twoPi) * driver.Amplitude * envelope;
				const double base = std::get<double>(value);
				const double driven = base + modulation;
				if (!std::isfinite(driven)) {
					SetDiagnostic(
						diagnostic,
						Status::InvalidValue,
						"sine keyframe driver result is not finite",
						resolved.Nodes[property.first].Id,
						std::string(property.second)
					);
					return diagnostic.Code;
				}
				value = driven;
			}
			Node &node = resolved.Nodes[property.first];
			const auto authored =
				std::find_if(node.Values.begin(), node.Values.end(), [&](const AuthoredValue &candidate) {
					return candidate.Port == property.second;
				});
			if (authored == node.Values.end()) {
				node.Values.push_back({std::string(property.second), std::move(value)});
			} else {
				authored->Data = std::move(value);
			}
		}
		if (resolvedDocument != nullptr) *resolvedDocument = resolved;
		return EvaluateGraph(resolved, plan, outputId, request, outputValue, diagnostic);
	}

	Status ResolveNodeValues(
		const Document &document,
		const Plan &plan,
		const std::string &outputId,
		const std::string &nodeId,
		const EvaluationRequest &request,
		std::vector<AuthoredValue> &values,
		Diagnostic &diagnostic
	) {
		values.clear();
		Document resolved;
		NodeResult ignored;
		const Status status =
			EvaluateResult(document, plan, outputId, request, ignored, diagnostic, &resolved);
		if (status != Status::Ok && resolved.Nodes.empty()) return status;
		const auto node =
			std::find_if(resolved.Nodes.begin(), resolved.Nodes.end(), [&](const Node &candidate) {
				return candidate.Id == nodeId;
			});
		if (node == resolved.Nodes.end()) {
			SetDiagnostic(diagnostic, Status::InvalidValue, "selected node does not exist", nodeId);
			return diagnostic.Code;
		}
		values = node->Values;
		return status == Status::UnsupportedExecution ? Status::Ok : status;
	}

	Status Evaluate(
		const Document &document,
		const Plan &plan,
		const std::string &outputId,
		const EvaluationRequest &request,
		Image &image,
		Diagnostic &diagnostic
	) {
		NodeResult result;
		const Status status = EvaluateResult(document, plan, outputId, request, result, diagnostic);
		if (status != Status::Ok) return status;
		if (auto *single = std::get_if<Image>(&result)) {
			image = std::move(*single);
			return Status::Ok;
		}
		ImageArray *array = std::get_if<ImageArray>(&result);
		if (!array) {
			const auto output =
				std::find_if(document.Outputs.begin(), document.Outputs.end(), [&](const Output &candidate) {
					return candidate.Id == outputId;
				});
			SetDiagnostic(
				diagnostic,
				Status::InvalidOutput,
				"selected output is a typed value",
				output->NodeId,
				output->Port
			);
			return diagnostic.Code;
		}
		if (array->Items.size() != 1 || !std::holds_alternative<size_t>(array->Items.front().Data)) {
			const auto output =
				std::find_if(document.Outputs.begin(), document.Outputs.end(), [&](const Output &candidate) {
					return candidate.Id == outputId;
				});
			SetDiagnostic(
				diagnostic,
				Status::InvalidOutput,
				"single-image consumer requires exactly one top-level image",
				output->NodeId,
				output->Port
			);
			return diagnostic.Code;
		}
		image = std::move(array->Images[std::get<size_t>(array->Items.front().Data)]);
		return Status::Ok;
	}

	Status EvaluateArray(
		const Document &document,
		const Plan &plan,
		const std::string &outputId,
		const EvaluationRequest &request,
		ImageArray &images,
		Diagnostic &diagnostic
	) {
		NodeResult result;
		const Status status = EvaluateResult(document, plan, outputId, request, result, diagnostic);
		if (status != Status::Ok) return status;
		if (auto *array = std::get_if<ImageArray>(&result)) {
			images = std::move(*array);
			return Status::Ok;
		}
		const auto output =
			std::find_if(document.Outputs.begin(), document.Outputs.end(), [&](const Output &candidate) {
				return candidate.Id == outputId;
			});
		SetDiagnostic(
			diagnostic,
			Status::InvalidOutput,
			"selected output is one image, not an image array",
			output->NodeId,
			output->Port
		);
		return diagnostic.Code;
	}

	Status EvaluateValue(
		const Document &document,
		const Plan &plan,
		const std::string &outputId,
		const EvaluationRequest &request,
		EvaluatedValue &value,
		Diagnostic &diagnostic
	) {
		NodeResult result;
		const Status status = EvaluateResult(document, plan, outputId, request, result, diagnostic);
		if (status != Status::Ok) return status;
		const auto output =
			std::find_if(document.Outputs.begin(), document.Outputs.end(), [&](const Output &candidate) {
				return candidate.Id == outputId;
			});
		const auto *ports = std::get_if<ValueOutputs>(&result);
		if (!ports) {
			SetDiagnostic(
				diagnostic, Status::InvalidOutput, "selected output is an image", output->NodeId, output->Port
			);
			return diagnostic.Code;
		}
		const auto selected = std::find_if(ports->begin(), ports->end(), [&](const AuthoredValue &port) {
			return port.Port == output->Port;
		});
		if (selected == ports->end()) {
			SetDiagnostic(
				diagnostic,
				Status::InvalidOutput,
				"value node did not produce selected port",
				output->NodeId,
				output->Port
			);
			return diagnostic.Code;
		}
		value = {selected->Port, selected->Data};
		diagnostic = {};
		return Status::Ok;
	}

	Status Evaluate(
		const Document &document,
		const Plan &plan,
		const std::string &outputId,
		uint64_t tick,
		Image &image,
		Diagnostic &diagnostic
	) {
		return Evaluate(document, plan, outputId, EvaluationRequest{tick, 0}, image, diagnostic);
	}

	Status Evaluate(
		const Document &document,
		const Plan &plan,
		const std::string &outputId,
		Image &image,
		Diagnostic &diagnostic
	) {
		return Evaluate(document, plan, outputId, 0, image, diagnostic);
	}
}
