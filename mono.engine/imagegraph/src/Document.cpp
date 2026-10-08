#include "ArrayOps.hpp"
#include "AudioPayload.hpp"
#include "CacheGroupReplayClone.hpp"
#include "EvaluationAllocator.hpp"
#include "GroupCallbackOpaque.hpp"
#include "GroupInputDepth.hpp"
#include "GroupReplayInternal.hpp"
#include "HostCaptureReceipts.hpp"
#include "ImageArrayCollector.hpp"
#include "KeyframeText.hpp"
#include "NativeSamplerBindings.hpp"
#include "NodeExecutors.hpp"
#include "PaletteOps.hpp"
#include "ParticleCodec.hpp"
#include "PcxControls.hpp"
#include "PendingGraph.hpp"
#include "PixelBoxMath.hpp"
#include "PixelBuilderPayload.hpp"
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
#include "ProcessorBatch.hpp"
#include "PuppetControl.hpp"
#include "RigidSchedule.hpp"
#include "SimulationAliases.hpp"
#include "SnapshotAudioMoves.hpp"
#include "SourceAnimatorIdentity.hpp"
#include "SourceAnimatorPersistence.hpp"
#include "SourceArgumentTransport.hpp"
#include "SourceAtlasCodec.hpp"
#include "SourceAxisStorage.hpp"
#include "SourceCommonAuthoring.hpp"
#include "SourceCommonExecution.hpp"
#include "SourceCommonMembership.hpp"
#include "SourceFontReceipts.hpp"
#include "SourceFontTransport.hpp"
#include "SourceFrameCacheLookup.hpp"
#include "SourceGetterProjection.hpp"
#include "SourceInputEvaluation.hpp"
#include "SourceInputOrigin.hpp"
#include "SourceLuaSockets.hpp"
#include "SourceMirrorPathProjection.hpp"
#include "SourcePathBakeCodec.hpp"
#include "SourcePathSequentialCodec.hpp"
#include "SourcePathShapeCodec.hpp"
#include "SourcePathShiftMemo.hpp"
#include "SourcePathSpiralCodec.hpp"
#include "SourceRegionOrigin.hpp"
#include "SourceRigidCodec.hpp"
#include "SourceSeparatedVec2.hpp"
#include "SourceTilesetCodec.hpp"
#include "SourceTunnel.hpp"
#include "SourceTunnelRegistry.hpp"
#include "SourceVec2Defaults.hpp"
#include "SourceVerletPathCodec.hpp"
#include "StrandCodec.hpp"
#include "Timeline.hpp"
#include "TimelineDrivers.hpp"
#include "TimelineOverrides.hpp"
#include "TimelineSchedule.hpp"
#include "ValueNodeEval.hpp"
#include "ValueNodeSchemas.hpp"
#include "ValuePayload.hpp"
#include "ValueText.hpp"
#include "nodes/ArraySource.hpp"
#include "nodes/Families.hpp"

#include <engine/core/Metrics.hpp>
#include <engine/imagegraph/AudioCapture.hpp>
#include <engine/imagegraph/Catalogue.hpp>
#include <engine/imagegraph/Document.hpp>
#include <engine/imagegraph/FrameTime.hpp>
#include <engine/imagegraph/GroupRenderSession.hpp>
#include <engine/imagegraph/NoiseField.hpp>
#include <engine/imagegraph/PortCompatibility.hpp>
#include <engine/imagegraph/SliceStackReplay.hpp>
#include <engine/imagegraph/SourceBuiltinRandom.hpp>
#include <engine/imagegraph/SourceCommonDispatch.hpp>
#include <engine/imagegraph/SourceFont.hpp>
#include <engine/imagegraph/SourceInputProcessingObserver.hpp>
#include <engine/imagegraph/SourceTimeline.hpp>
#include <engine/imagegraph/StatefulReplay.hpp>
#include <engine/imagegraph/StatefulTemporalCone.hpp>
#include <engine/imagegraph/Vector2Presentation.hpp>
#include <engine/imagegraph/WavPreview.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <iomanip>
#include <limits>
#include <locale>
#include <map>
#include <new>
#include <queue>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace engine::imagegraph {
	namespace {
		bool SourceModePort(const Node &node, std::string_view port) {
			if (node.Type == "pc.global_scope")
				return std::ranges::any_of(node.DynamicInputs, [port](const DynamicInput &input) {
					return input.Id == port;
				});
			if (detail::AliasedSourceInput(node, port) ||
				(node.Type == "pc.group_input" && port == "parent_value"))
				return true;
			if (node.Type != "pc.gradient" || port != "gradient_map_range") return false;
			const auto *entry = FindCatalogueEntry(node.Type);
			const auto *input = entry ? FindCatalogueInput(*entry, port) : nullptr;
			return input && input->SourceIndex == 16 && input->SourceKind == "Vec4" &&
				   input->Type == ValueType::Vector4;
		}

		bool EmptySourceScalarArray(const Value &value) {
			const auto *array = std::get_if<ArrayValue>(&value);
			return array && array->ElementType == ValueType::Scalar && array->Elements.empty() &&
				   array->Nested.empty() && array->Items.empty();
		}
		bool SourceEmptyGroupVectorKey(
			const Document &document, const Node &node, const CatalogueInput *input, std::string_view port
		) {
			if (document.FormatVersion < 9 || !input ||
				(node.Type != "pc.group_input" && node.Type != "pc.group_output") ||
				input->Type != ValueType::Vector2 ||
				(input->SourceKind != "Range" && input->SourceKind != "Vec2"))
				return false;
			if (std::find(node.SourceAnimatedInputs.begin(), node.SourceAnimatedInputs.end(), port) ==
					node.SourceAnimatedInputs.end() &&
				std::find(node.SourceStaticInputs.begin(), node.SourceStaticInputs.end(), port) ==
					node.SourceStaticInputs.end())
				return false;
			if (std::none_of(
					document.Tracks.begin(), document.Tracks.end(), [&](const AnimationTrack &track) {
						return track.NodeId == node.Id && track.Port == port;
					}
				))
				return false;
			const Keyframe *found = nullptr;
			for (const auto &key : document.Keyframes)
				if (key.NodeId == node.Id && key.Port == port) {
					if (found) return false;
					found = &key;
				}
			return found && found->Interpolation == "source" && found->Kind == KeyframeKind::Normal &&
				   !found->SourceDriver && !found->SineDriver && EmptySourceScalarArray(found->Data);
		}
		constexpr std::array<PropertySchema, 1> GROUP_CALLBACK_OPAQUE_PROPERTIES{
			{{"source_type", ValueType::Text}}
		};
		const NodeSchema GROUP_CALLBACK_OPAQUE_SCHEMA{
			detail::GroupCallbackOpaqueType, {}, GROUP_CALLBACK_OPAQUE_PROPERTIES
		};
		constexpr std::array<PortSchema, 1> CAPTURED_IMAGE_PORTS{
			{{"image", ValueType::Image, PortDirection::Output}}
		};
		constexpr std::array<PropertySchema, 1> CAPTURED_IMAGE_PROPERTIES{{{"source_id", ValueType::Text}}};
		const NodeSchema CAPTURED_IMAGE_SCHEMA{
			"image.captured", CAPTURED_IMAGE_PORTS, CAPTURED_IMAGE_PROPERTIES
		};
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
		constexpr std::array<PortSchema, 4> SIMPLEX_PORTS = {{
			{"uv_map", ValueType::Image, PortDirection::Input},
			{"mask", ValueType::Image, PortDirection::Input},
			{"image", ValueType::Image, PortDirection::Output},
			{"field", ValueType::Noise2D, PortDirection::Output},
		}};
		constexpr std::array<PropertySchema, 19> SIMPLEX_PROPERTIES = {{
			{"output_type", ValueType::Enum},
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

		// Indexed by ValueType. Names are durable document text.
		constexpr std::array<std::string_view, 51> TYPE_NAMES = {
			"boolean",
			"integer",
			"scalar",
			"text",
			"colour",
			"vector2",
			"image",
			"array",
			"gradient",
			"area",
			"curve",
			"vector4",
			"path2d",
			"vector3",
			"quaternion",
			"enum",
			"mesh",
			"audiobit",
			"mesh2d",
			"matrix",
			"particle",
			"rigid",
			"fluid_domain",
			"smoke_domain",
			"strand",
			"sdf",
			"armature",
			"atlas",
			"tileset",
			"pixel_box",
			"scene3d",
			"material3d",
			"light3d",
			"buffer",
			"struct",
			"any",
			"node_ref",
			"pcx_node",
			"object",
			"dynamic_surface",
			"path3d",
			"font",
			"noise1d",
			"noise2d",
			"noise3d",
			"noise1d_vector2",
			"noise2d_vector2",
			"noise3d_vector2",
			"noise1d_vector3",
			"noise2d_vector3",
			"noise3d_vector3",
		};
		static_assert(static_cast<size_t>(ValueType::Noise3DVector3) + 1 == TYPE_NAMES.size());

		constexpr std::array<std::string_view, 9> DEPTH_NAMES = {
			"input", "inherited", "rgba4", "rgba8", "rgba16f", "rgba32f", "r8", "r16f", "r32f"
		};
		std::optional<int64_t> ParseDepth(std::string_view name) {
			const auto found = std::find(DEPTH_NAMES.begin(), DEPTH_NAMES.end(), name);
			if (found == DEPTH_NAMES.end()) return std::nullopt;
			return std::distance(DEPTH_NAMES.begin(), found);
		}

		std::string_view TypeName(ValueType type) {
			const size_t index = static_cast<size_t>(type);
			return index < TYPE_NAMES.size() ? TYPE_NAMES[index] : std::string_view{};
		}

		std::optional<ValueType> ParseType(std::string_view name) {
			for (size_t index = 0; index < TYPE_NAMES.size(); index++) {
				if (TYPE_NAMES[index] == name) return static_cast<ValueType>(index);
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
			std::string_view node = {},
			std::string_view port = {}
		) {
			diagnostic = {status, std::string(node), std::string(port), std::move(message)};
		}

		ValueType TypeOf(const Value &value) {
			return detail::PayloadType(value);
		}

		bool IsFinite(const Value &value) {
			if (const auto *array = std::get_if<ArrayValue>(&value); array && !array->Nested.empty())
				return array->ElementType != ValueType::Image && array->ElementType != ValueType::Array &&
					   (array->ElementType < ValueType::Gradient ||
						(array->ElementType == ValueType::Area || array->ElementType == ValueType::Enum ||
						 array->ElementType == ValueType::Particle ||
						 array->ElementType == ValueType::Strand ||
						 array->ElementType == ValueType::Tileset || array->ElementType == ValueType::Rigid ||
						 array->ElementType == ValueType::Atlas)) &&
					   detail::ValidPayload(*array, true);
			return detail::ValidValuePayload(value, false);
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

		// node_value_types.gml value_bit: junction families a link may join. Numbers of
		// every shape share one family, as the source stores them all as integer, float
		// or boolean junctions.
		uint64_t JunctionBits(ValueType type) {
			constexpr uint64_t NUMBER = 1ull << 1;
			switch (type) {
			case ValueType::Integer:
			case ValueType::Enum:
				return 1ull << 0 | NUMBER;
			case ValueType::Boolean:
				return 1ull << 3 | NUMBER;
			case ValueType::Scalar:
			case ValueType::Vector2:
			case ValueType::Vector3:
			case ValueType::Vector4:
			case ValueType::Quaternion:
			case ValueType::Array:
			case ValueType::Area:
			case ValueType::Matrix:
				return 1ull << 2 | NUMBER;
			case ValueType::Colour:
				return 1ull << 4;
			case ValueType::Gradient:
				return 1ull << 25;
			case ValueType::Image:
			case ValueType::Atlas:
			case ValueType::DynamicSurface:
				return 1ull << 5 | 1ull << 33;
			case ValueType::Text:
				return 1ull << 10;
			case ValueType::Font:
				// Source Font shares the string bit; native resource transport is restricted to Font getters.
				return 1ull << 39;
			case ValueType::Object:
				return 1ull << 13;
			case ValueType::Path2D:
			case ValueType::Path3D:
				return 1ull << 15;
			case ValueType::Particle:
				return 1ull << 16;
			case ValueType::Rigid:
				return 1ull << 17;
			case ValueType::SmokeDomain:
				return 1ull << 18;
			case ValueType::Struct:
				return 1ull << 19;
			case ValueType::Strand:
				return 1ull << 20;
			case ValueType::Mesh2D:
				return 1ull << 21;
			case ValueType::Armature:
				return 1ull << 26 | 1ull << 19;
			case ValueType::NodeRef:
				return 1ull << 32;
			case ValueType::Buffer:
				return 1ull << 27;
			case ValueType::PixelBox:
				return 1ull << 28;
			case ValueType::Mesh:
			case ValueType::Light3D:
				return 1ull << 29;
			case ValueType::Scene3D:
				return 1ull << 29 | 1ull << 30;
			case ValueType::Material3D:
				return 1ull << 33;
			case ValueType::PcxNode:
				return 1ull << 34;
			case ValueType::AudioBit:
				return 1ull << 35;
			case ValueType::FluidDomain:
				return 1ull << 36;
			case ValueType::Sdf:
				return 1ull << 37;
			case ValueType::Tileset:
			case ValueType::Curve:
				return 1ull << 38;
			case ValueType::Noise1D:
				return 1ull << 40;
			case ValueType::Noise2D:
				return 1ull << 41;
			case ValueType::Noise3D:
				return 1ull << 42;
			case ValueType::Noise1DVector2:
				return 1ull << 43;
			case ValueType::Noise2DVector2:
				return 1ull << 44;
			case ValueType::Noise3DVector2:
				return 1ull << 45;
			case ValueType::Noise1DVector3:
				return 1ull << 46;
			case ValueType::Noise2DVector3:
				return 1ull << 47;
			case ValueType::Noise3DVector3:
				return 1ull << 48;
			case ValueType::Any:
				return ~0ull & ~(1ull << 32);
			}
			return 0;
		}

		// node_value_types.gml typeCompatible with its directional casts, for links
		// touching catalogue nodes.
		bool JunctionCompatible(ValueType from, ValueType to) {
			if ((JunctionBits(from) & JunctionBits(to)) != 0) return true;
			const bool number = (JunctionBits(from) & (1ull << 1)) != 0;
			const bool toNumber = (JunctionBits(to) & (1ull << 1)) != 0;
			if (from == ValueType::Image && toNumber) return true;
			if (number && to == ValueType::Text) return true;
			if (number && to == ValueType::Colour) return true;
			if (from == ValueType::Colour && toNumber) return true;
			if (from == ValueType::Colour && to == ValueType::Gradient) return true;
			if (from == ValueType::Strand && to == ValueType::Path2D) return true;
			if ((from == ValueType::Colour || from == ValueType::Mesh2D || from == ValueType::Particle) &&
				to == ValueType::Struct)
				return true;
			return false;
		}

		bool IsNoisePortSelector(const Node &node, std::string_view port) {
			return ((node.Type == "value.noise_field" || node.Type == "value.sample_noise" ||
					 IsNoiseImageGenerator(node.Type)) &&
					port == "output_type") ||
				   (node.Type == "value.noise_field" && (port == "mode" || port == "dimension"));
		}

		// A catalogue input's bypass junction forwards that input's value unchanged.
		constexpr std::string_view BYPASS_SUFFIX = ".bypass";

		std::optional<ValueType> FindPortType(
			const Node &node, std::string_view id, PortDirection side, const Document *document = nullptr
		) {
			// grug PCX-only reads also need a dependency; undeclared ports must not hide an opaque producer.
			if (detail::IsGroupCallbackOpaque(node) && !id.empty() && id.size() <= Limits::MaximumTextBytes &&
				(side == PortDirection::Input || side == PortDirection::Output))
				return ValueType::Any;
			if (const PortSchema *port = FindPort(node.Type, id, side)) {
				if (node.Type == "value.noise_field" || node.Type == "value.sample_noise" ||
					(IsNoiseImageGenerator(node.Type) && side == PortDirection::Output && id == "field")) {
					const auto instance = NoiseNodePort(node, id, side, document);
					return instance ? std::optional(instance->Type) : std::nullopt;
				}
				if (side == PortDirection::Output &&
					(node.Type == "pc.rgb_channel" || node.Type == "pc.hsv_channel")) {
					bool outputArray = false;
					for (const auto &value : node.Values)
						if (value.Port == "output_array") {
							if (const auto *flag = std::get_if<bool>(&value.Data))
								outputArray = *flag;
							else if (const auto *real = std::get_if<double>(&value.Data))
								outputArray = *real != 0;
						}
					if (outputArray)
						return (id == "red" || id == "hue") ? std::optional<ValueType>{ValueType::Array}
															: std::nullopt;
				}
				if (side == PortDirection::Output && node.Type == "pc.csv_file_read" && id == "content") {
					for (const auto &value : node.Values)
						if (value.Port == "convert_to_number") {
							if (const auto *flag = std::get_if<bool>(&value.Data))
								return *flag ? ValueType::Scalar : ValueType::Text;
						}
				}
				if (side == PortDirection::Output && node.Type == "pc.directory_search" && id == "outputs") {
					for (const auto &value : node.Values)
						if (value.Port == "type")
							if (const auto *mode = std::get_if<EnumValue>(&value.Data))
								return mode->Value == 1 ? ValueType::Text : ValueType::Image;
				}
				return port->Type;
			}
			if (side == PortDirection::Output && id.ends_with(BYPASS_SUFFIX)) {
				if (const CatalogueEntry *entry = FindCatalogueEntry(node.Type)) {
					if (const auto *input =
							FindCatalogueInput(*entry, id.substr(0, id.size() - BYPASS_SUFFIX.size())))
						return input->Type;
					// HLSL source input bypasses also identify authored dynamic argument records.
					if (node.Type == "pc.hlsl") {
						const auto inputId = id.substr(0, id.size() - BYPASS_SUFFIX.size());
						for (const auto &input : node.DynamicInputs)
							if (input.Id == inputId) return input.Type;
					}
				}
			}
			if (side == PortDirection::Output) {
				for (const DynamicOutput &output : node.DynamicOutputs)
					if (output.Id == id) return output.Type;
			}
			if (side == PortDirection::Input) {
				for (const DynamicInput &input : node.DynamicInputs) {
					if (input.Id == id) {
						if (node.Type != "pc.hlsl")
							if (const auto selected = detail::SourceArgumentType(node, id)) return selected;
						return input.Type;
					}
				}
			}
			return std::nullopt;
		}

		bool WithinArrayBudget(const ArrayValue &array) {
			if (array.ElementType == ValueType::Particle || array.ElementType == ValueType::Tileset ||
				array.ElementType == ValueType::Rigid || array.ElementType == ValueType::Atlas ||
				array.ElementType == ValueType::Strand)
				return detail::ValidPayload(array, true);
			if (!array.Nested.empty()) {
				if (array.Nested.size() > Limits::MaximumArrayElements) return false;
				size_t count = array.Elements.size();
				if (count > Limits::MaximumArrayElements) return false;
				for (const auto &row : array.Nested) {
					if (row.size() > Limits::MaximumArrayElements - count) return false;
					count += row.size();
				}
				return detail::PayloadOwnedBytes(array) <= Limits::MaximumArrayBytes;
			}
			if (array.ElementType == ValueType::Any || !array.Items.empty())
				return detail::ValidPayload(array, false);
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
				return detail::ValidAudioPlanes(
					audio->Samples, audio->Channels, Limits::MaximumAudioClipSamples
				);
			if (const auto *matrix = std::get_if<MatrixValue>(&value))
				return matrix->Values.size() <= Limits::MaximumArrayElements;
			return true;
		}

		bool ValidArray(const ArrayValue &array) {
			if (!array.Nested.empty())
				return array.ElementType != ValueType::Image && array.ElementType != ValueType::Array &&
					   (array.ElementType < ValueType::Gradient ||
						(array.ElementType == ValueType::Area || array.ElementType == ValueType::Enum ||
						 array.ElementType == ValueType::Particle ||
						 array.ElementType == ValueType::Tileset || array.ElementType == ValueType::Rigid ||
						 array.ElementType == ValueType::Atlas || array.ElementType == ValueType::Strand)) &&
					   detail::ValidPayload(array, true);
			if (array.ElementType == ValueType::Any || !array.Items.empty())
				return detail::ValidPayload(array, false);
			if (!array.Nested.empty() || !WithinArrayBudget(array) || array.ElementType == ValueType::Image ||
				array.ElementType == ValueType::Array ||
				(array.ElementType >= ValueType::Gradient && array.ElementType != ValueType::Area &&
				 array.ElementType != ValueType::Enum && array.ElementType != ValueType::Particle &&
				 array.ElementType != ValueType::Tileset && array.ElementType != ValueType::Rigid &&
				 array.ElementType != ValueType::Atlas && array.ElementType != ValueType::Strand) ||
				TypeName(array.ElementType).empty())
				return false;
			for (const ElementValue &element : array.Elements)
				if (!std::visit(
						[&](const auto &item) {
							return detail::PayloadType(item) == array.ElementType &&
								   detail::ValidPayload(item, false);
						},
						element
					))
					return false;
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
			if (std::holds_alternative<DynamicSurfaceValue>(value)) return "ninecold";
			if (const auto *path = std::get_if<PathValue3D>(&value))
				return path->Data && path->Data->SourceOperation
						   ? "p3o"
						   : (path->Data && path->Data->SourcePolyline ? "p3s" : "p3");
			if (std::holds_alternative<PixelBoxValue>(value)) return "pb";
			if (std::holds_alternative<ParticleValue>(value)) return "particle2";
			if (std::holds_alternative<StrandValue>(value)) return "strand2";
			if (std::holds_alternative<TilesetValue>(value)) return "tileset";
			if (std::holds_alternative<RigidValue>(value)) return "rg";
			if (std::holds_alternative<AtlasValue>(value)) return "at";
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
				return std::get<Path2D>(value).SourceOperation ? "po"
					   : std::get<Path2D>(value).Segmented	   ? "ps"
															   : "p";
			case 12:
				return "3";
			case 13:
				return "h";
			case 14:
				return "e";
			case 16:
				return "m";
			default:
				return "u";
			}
		}

		void WriteQuoted(std::ostream &stream, std::string_view text);
		bool ReadQuoted(std::istream &stream, std::string &text);
		bool ReadQuoted(std::istream &stream, std::string &text, size_t maximum);

		uint64_t OwnedTextCapacity(const std::string &value) {
			return value.capacity() > std::string{}.capacity() ? value.capacity() : 0;
		}
		struct TokenCharge {
			detail::AllocationReservation Storage;
			std::array<std::pair<const std::string *, uint64_t>, 32> Strings{};
			size_t Count = 0;
			void Reset() {
				Storage.Reset();
				Count = 0;
			}
		};
		bool ReadToken(
			std::istream &stream,
			std::string &text,
			detail::EvaluationBudget *budget,
			TokenCharge &charge,
			bool *allocationRefused
		) {
			if (!budget) return bool(stream >> text);
			stream >> std::ws;
			const auto start = stream.tellg();
			if (start == std::istream::pos_type(-1)) return false;
			const std::locale streamLocale = stream.getloc();
			const auto &ctype = std::use_facet<std::ctype<char>>(streamLocale);
			size_t length = 0;
			int character = stream.peek();
			const size_t maximumLength =
				stream.width() > 0 ? static_cast<size_t>(stream.width()) : std::numeric_limits<size_t>::max();
			while (length < maximumLength && character != std::char_traits<char>::eof() &&
				   !ctype.is(std::ctype_base::space, static_cast<char>(character))) {
				stream.get();
				++length;
				character = stream.peek();
			}
			if (!length) return false;
			stream.clear();
			stream.seekg(start);
			if (length > text.capacity()) {
				size_t slot = 0;
				while (slot < charge.Count && charge.Strings[slot].first != &text)
					++slot;
				if (slot == charge.Count) {
					if (charge.Count == charge.Strings.size() || text.capacity() > std::string{}.capacity()) {
						*allocationRefused = true;
						return false;
					}
					charge.Strings[charge.Count++] = {&text, 0};
				}
				const uint64_t oldBytes = charge.Strings[slot].second;
				auto replacement = budget->Reserve(length);
				if (!replacement) {
					*allocationRefused = true;
					return false;
				}
				std::string sized(length, '\0');
				if (!replacement->Resize(sized.capacity())) {
					*allocationRefused = true;
					return false;
				}
				text = std::move(sized);
				text.clear();
				if (oldBytes > charge.Storage.Bytes() ||
					!charge.Storage.Resize(charge.Storage.Bytes() - oldBytes) ||
					!charge.Storage.Merge(std::move(*replacement)))
					std::terminate();
				charge.Strings[slot].second = text.capacity();
			}
			return bool(stream >> text);
		}
		struct BoundedToken {
			std::string &Text;
			detail::EvaluationBudget *Budget;
			TokenCharge &Charge;
			bool *AllocationRefused;
		};
		std::istream &operator>>(std::istream &stream, BoundedToken token) {
			if (!ReadToken(stream, token.Text, token.Budget, token.Charge, token.AllocationRefused))
				stream.setstate(std::ios::failbit);
			return stream;
		}

		void WriteValue(std::ostream &stream, const Value &value);
		void WritePathPayload(std::ostream &stream, const Path2D &path) {
			if (path.SourceOperation) {
				const auto &operation = *path.SourceOperation;
				if (operation.Kind == SourcePathOperationKind::Bake) {
					stream << "bake ";
					detail::WriteSourceBaked(stream, *operation.Baked);
					return;
				}
				if (operation.Kind == SourcePathOperationKind::Shape) {
					stream << "shape ";
					detail::WriteSourcePathShape(stream, *operation.Shape);
					return;
				}
				if (operation.Kind == SourcePathOperationKind::Spiral) {
					stream << "spiral " << operation.Inputs.size() << ' ';
					detail::WriteSourcePathSpiral(stream, *operation.Spiral);
					stream << ' ' << bool(operation.WeightInput3D);
					if (operation.WeightInput3D) {
						stream << ' ';
						PathValue3D child;
						child.Data = operation.WeightInput3D;
						WriteValue(stream, Value{std::move(child)});
					}
					for (const auto &child : operation.Inputs) {
						stream << ' ' << (child.SourceOperation ? "po" : child.Segmented ? "ps" : "p") << ' ';
						WritePathPayload(stream, child);
					}
					return;
				}
				if (operation.Kind == SourcePathOperationKind::VerletMesh) {
					stream << "verlet ";
					detail::WriteSourceVerletPath(stream, operation);
					return;
				}
				stream << (operation.Kind == SourcePathOperationKind::Reverse		 ? "reverse"
						   : operation.Kind == SourcePathOperationKind::Repeat		 ? "repeat"
						   : operation.Kind == SourcePathOperationKind::Trim		 ? "trim"
						   : operation.Kind == SourcePathOperationKind::Offset		 ? "offset"
						   : operation.Kind == SourcePathOperationKind::Blend		 ? "blend"
						   : operation.Kind == SourcePathOperationKind::Join		 ? "join"
						   : operation.Kind == SourcePathOperationKind::Redistribute ? "redistribute"
						   : operation.Kind == SourcePathOperationKind::Skew		 ? "skew"
						   : operation.Kind == SourcePathOperationKind::Transform	 ? "transform"
						   : operation.Kind == SourcePathOperationKind::AreaMap		 ? "area_map"
						   : operation.Kind == SourcePathOperationKind::Shift		 ? "shift"
						   : operation.Kind == SourcePathOperationKind::Extends		 ? "extends"
						   : operation.Kind == SourcePathOperationKind::Flatten		 ? "flatten"
						   : operation.Kind == SourcePathOperationKind::Smoothen	 ? "smoothen"
						   : operation.Kind == SourcePathOperationKind::WeightAdjust ? "weight_adjust"
																					 : "combine")
					   << ' ' << operation.Inputs.size();
				if (operation.Kind == SourcePathOperationKind::Trim)
					stream << ' ' << operation.TrimRange.X << ' ' << operation.TrimRange.Y;

				if (operation.Kind == SourcePathOperationKind::Offset)
					stream << ' ' << std::setprecision(17) << operation.Offset << ' '
						   << operation.ClampOffset;
				if (operation.Kind == SourcePathOperationKind::Blend) {
					stream << ' ' << unsigned(operation.BlendMode) << ' ' << std::setprecision(17)
						   << operation.BlendAmount << ' ' << operation.BlendInputsValid[0] << ' '
						   << operation.BlendInputsValid[1];
					stream << ' ' << operation.BlendLengths.size();
					for (size_t line = 0; line < operation.BlendLengths.size(); ++line) {
						stream << ' ' << operation.BlendLengths[line] << ' '
							   << operation.BlendAccumulated[line].size();
						for (double value : operation.BlendAccumulated[line])
							stream << ' ' << value;
					}
				}

				if (operation.Kind == SourcePathOperationKind::Redistribute)
					for (double value : *operation.RedistributeMap)
						stream << ' ' << std::setprecision(17) << value;
				if (operation.Kind == SourcePathOperationKind::Skew)
					stream << ' ' << unsigned(operation.SkewAxis) << ' ' << std::setprecision(17)
						   << operation.SkewStrength << ' ' << operation.SkewCenter.X << ' '
						   << operation.SkewCenter.Y;
				if (operation.Kind == SourcePathOperationKind::Transform)
					stream << ' ' << std::setprecision(17) << operation.TransformPosition.X << ' '
						   << operation.TransformPosition.Y << ' ' << operation.TransformAnchor.X << ' '
						   << operation.TransformAnchor.Y << ' ' << operation.TransformScale.X << ' '
						   << operation.TransformScale.Y << ' ' << operation.TransformRotation;
				if (operation.Kind == SourcePathOperationKind::AreaMap)
					stream << ' ' << std::setprecision(17) << operation.MapFrom.X << ' '
						   << operation.MapFrom.Y << ' ' << operation.MapFrom.Z << ' ' << operation.MapFrom.W
						   << ' ' << operation.MapArea.X << ' ' << operation.MapArea.Y << ' '
						   << operation.MapArea.Z << ' ' << operation.MapArea.W;
				if (operation.Kind == SourcePathOperationKind::WeightAdjust) {
					stream << ' ' << unsigned(operation.WeightType) << ' ' << unsigned(operation.WeightMode)
						   << ' ' << std::setprecision(17) << operation.WeightValue << ' '
						   << operation.WeightDirection << ' ' << operation.WeightRange.X << ' '
						   << operation.WeightRange.Y << ' ' << operation.WeightLoop << ' '
						   << operation.WeightCurve.size();
					for (double value : operation.WeightCurve)
						stream << ' ' << value;
					stream << ' ' << bool(operation.WeightInput3D);
					if (operation.WeightInput3D) {
						stream << ' ';
						PathValue3D child;
						child.Data = operation.WeightInput3D;
						WriteValue(stream, Value{std::move(child)});
					}
				}
				if (detail::SourceSequentialKind(operation.Kind)) {
					detail::WriteSourceSequential(stream, *operation.Sequential);
					stream << ' ' << bool(operation.WeightInput3D);
					if (operation.WeightInput3D) {
						stream << ' ';
						PathValue3D child;
						child.Data = operation.WeightInput3D;
						WriteValue(stream, Value{std::move(child)});
					}
				}
				if (operation.Kind == SourcePathOperationKind::Shift)
					stream << ' ' << operation.ShiftDistance << ' ' << operation.ShiftRange.X << ' '
						   << operation.ShiftRange.Y << ' ' << operation.ShiftLoop;
				if (operation.Kind == SourcePathOperationKind::Join)
					for (uint8_t reverse : operation.Reversed)
						stream << ' ' << unsigned(reverse);
				for (const auto &child : operation.Inputs) {
					stream << ' ' << (child.SourceOperation ? "po" : child.Segmented ? "ps" : "p") << ' ';
					WritePathPayload(stream, child);
				}
				return;
			}
			stream << path.Loop << ' ' << path.Anchors.size() << ' ' << path.Weights.size();
			for (const auto &anchor : path.Anchors) {
				for (double field : anchor.Controls)
					stream << ' ' << std::setprecision(17) << field;
				stream << ' ' << anchor.Index;
			}
			for (const auto &weight : path.Weights)
				stream << ' ' << std::setprecision(17) << weight.Position << ' ' << weight.Weight;
		}

		void WriteValue(std::ostream &stream, const Value &value) {
			if (detail::ContainsFontLiteral(value)) {
				stream.setstate(std::ios::failbit);
				return;
			}
			stream << ValueTag(value) << ' ';
			if (const auto *dynamic = std::get_if<DynamicSurfaceValue>(&value)) {
				if (!dynamic->Data || !dynamic->Data->NineSlice || !dynamic->Data->NineSlice->Cold ||
					!detail::ValidPixelBuilderPayload(*dynamic)) {
					stream.setstate(std::ios::failbit);
					return;
				}
				// grug native receipt records ownership, never fabricates a source surface.
				detail::WriteTilesetString(stream, dynamic->Data->OwnerNodeId);
				return;
			}
			if (const auto *strand = std::get_if<StrandValue>(&value)) {
				stream << std::setprecision(17);
				detail::WriteStrandValue(stream, *strand);
				return;
			}
			if (const auto *particle = std::get_if<ParticleValue>(&value)) {
				stream << std::setprecision(17);
				detail::WriteParticleValue(stream, *particle);
			} else if (const auto *tileset = std::get_if<TilesetValue>(&value)) {
				detail::WriteTilesetValue(stream, *tileset);
			} else if (const auto *rigid = std::get_if<RigidValue>(&value)) {
				detail::WriteSourceRigidAlias(stream, *rigid, WriteQuoted);
			} else if (const auto *atlas = std::get_if<AtlasValue>(&value)) {
				detail::WriteAtlasValue(stream, *atlas);
			} else if (const auto *boolean = std::get_if<bool>(&value)) {
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
				if (!array->Items.empty()) {
					stream << "any " << array->Items.size();
					const auto writeItem = [&](const auto &self, const SourceArrayItem &entry) -> void {
						stream << ' ';
						std::visit(
							[&](const auto &child) {
								using C = std::decay_t<decltype(child)>;
								if constexpr (std::is_same_v<C, ElementValue>)
									std::visit(
										[&](const auto &leaf) { WriteValue(stream, Value{leaf}); }, child
									);
								else if constexpr (std::is_same_v<C, std::vector<SourceArrayItem>>) {
									stream << "a any " << child.size();
									for (const auto &item : child)
										self(self, item);
								} else
									stream << "invalid";
							},
							entry.Data
						);
					};
					for (const auto &entry : array->Items)
						writeItem(writeItem, entry);
					return;
				}
				if (!array->Nested.empty()) {
					stream << "array " << array->Nested.size();
					for (const auto &row : array->Nested) {
						stream << ' ';
						WriteValue(stream, ArrayValue{array->ElementType, row});
					}
					return;
				}
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
			} else if (const auto *matrix = std::get_if<MatrixValue>(&value)) {
				stream << matrix->Columns << ' ' << matrix->Rows;
				for (const double number : matrix->Values)
					stream << ' ' << std::setprecision(17) << number;
			} else if (const auto *audio = std::get_if<AudioBit>(&value)) {
				stream << std::setprecision(17) << audio->SampleRate << ' ' << audio->Samples.size();
				for (const double sample : audio->Samples)
					stream << ' ' << sample;
			} else if (const auto *storedPath = std::get_if<PathValue3D>(&value)) {
				stream << bool(storedPath->Data);
				if (storedPath->Data) {
					const auto &path = *storedPath->Data;
					if (path.SourcePolyline) {
						stream << " source_polyline 1 source_empty_cache " << bool(path.SourceEmptyCache);
						if (path.SourceEmptyCache)
							stream << ' ' << path.SourceEmptyCache->Length << ' '
								   << path.SourceEmptyCache->SegmentCount;
						stream << " source_metadata 1 " << bool(path.SourceBounds2D);
						if (path.SourceBounds2D)
							stream << ' ' << path.SourceBounds2D->X << ' ' << path.SourceBounds2D->Y << ' '
								   << path.SourceBounds2D->Z << ' ' << path.SourceBounds2D->W;
						stream << ' '
							   << (path.SourceEmptyCache ? path.SourceEmptyCache->Accumulated.size() : 0);
						if (path.SourceEmptyCache)
							for (double value : path.SourceEmptyCache->Accumulated)
								stream << ' ' << value;
					}
					stream << ' ' << path.Loop << ' ' << path.Resolution << ' ' << path.Anchors.size() << ' '
						   << path.SourcePresent << ' ' << bool(path.Source2D) << ' '
						   << path.Transforms.size();
					for (const auto &anchor : path.Anchors) {
						for (double number : anchor.Controls)
							stream << ' ' << std::setprecision(17) << number;
						stream << ' ' << anchor.Index;
					}
					if (path.Source2D) {
						stream << ' ';
						WriteValue(stream, Value{*path.Source2D});
					}
					for (const auto &transform : path.Transforms) {
						const double fields[]{
							transform.Position.X,
							transform.Position.Y,
							transform.Position.Z,
							transform.Anchor.X,
							transform.Anchor.Y,
							transform.Anchor.Z,
							transform.Scale.X,
							transform.Scale.Y,
							transform.Scale.Z,
							transform.Rotation.X,
							transform.Rotation.Y,
							transform.Rotation.Z,
							transform.Rotation.W
						};
						for (double number : fields)
							stream << ' ' << std::setprecision(17) << number;
						stream << ' ' << transform.Projective;
						if (transform.Projective) {
							stream << ' ' << transform.DepthWeight << ' ' << std::setprecision(17)
								   << transform.ProjectionScale.X << ' ' << transform.ProjectionScale.Y;
							for (double value : transform.CameraView)
								stream << ' ' << std::setprecision(17) << value;
							for (double value : transform.CameraProjection)
								stream << ' ' << std::setprecision(17) << value;
						}
					}
					if (path.SourceOperation) {
						const auto &op = *path.SourceOperation;
						stream << ' ' << unsigned(op.Kind) << ' ' << std::setprecision(17) << op.TrimRange.X
							   << ' ' << op.TrimRange.Y << ' ' << op.Inputs.size();
						for (const auto &child : op.Inputs) {
							stream << ' ';
							WriteValue(stream, Value{child});
						}
					}
				}
			} else if (const auto *storedPath = std::get_if<Path2D>(&value)) {
				WritePathPayload(stream, *storedPath);
			} else if (const auto *pixelBox = std::get_if<PixelBoxValue>(&value)) {
				stream << bool(pixelBox->Data);
				if (pixelBox->Data) {
					const PixelBoxData &box = *pixelBox->Data;
					for (double field : box.BaseBounds)
						stream << ' ' << std::setprecision(17) << field;
					stream << ' ' << bool(box.FixedBounds);
					if (box.FixedBounds)
						for (double field : *box.FixedBounds)
							stream << ' ' << std::setprecision(17) << field;
					for (uint8_t mode : box.AnchorModes)
						stream << ' ' << unsigned(mode);
					for (uint8_t mode : box.PreviousAnchorModes)
						stream << ' ' << unsigned(mode);
					for (double anchor : box.Anchors)
						stream << ' ' << std::setprecision(17) << anchor;
					for (bool fractional : box.Fractional)
						stream << ' ' << fractional;
					for (uint8_t mode : box.DimensionBoundModes)
						stream << ' ' << unsigned(mode);
					for (double bound : box.DimensionBounds)
						stream << ' ' << std::setprecision(17) << bound;
				}
			}
		}

		bool ReadValue(
			std::istream &stream,
			Value &value,
			uint32_t version,
			bool allowArray = true,
			detail::EvaluationBudget *budget = nullptr,
			detail::AllocationReservation *outputCharge = nullptr,
			bool *allocationRefused = nullptr,
			size_t depth = 0,
			size_t *arrayCount = nullptr
		) {
			size_t localArrayCount = 0;
			if (!arrayCount) arrayCount = &localArrayCount;
			if (depth > Limits::MaximumArrayDepth) return false;
			const auto admit = [&](uint64_t bytes) {
				if (!budget) return true;
				auto charge = budget->Reserve(bytes);
				if (!charge) {
					*allocationRefused = true;
					return false;
				}
				return outputCharge->Merge(std::move(*charge));
			};
			TokenCharge tagCharge;
			std::string tag;
			if (!ReadToken(stream, tag, budget, tagCharge, allocationRefused)) return false;
			if (tag == "ninecold" && version >= 9) {
				const auto authoredBytes = DocumentRetainedPayloadBytes(Document{});
				if (!authoredBytes || !admit(detail::MeshAddBytes(sizeof(PixelBuilderData), *authoredBytes)))
					return false;
				DynamicSurfaceValue dynamic;
				auto &data = dynamic.Data.emplace();
				if (!detail::ReadTilesetString(stream, data.OwnerNodeId, admit) || data.OwnerNodeId.empty())
					return false;
				data.BaseDimension = {1, 1};
				data.NineSlice.emplace().Cold = true;
				if (!detail::ValidPixelBuilderPayload(dynamic)) return false;
				value = std::move(dynamic);
				return true;
			}
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
				if (budget) {
					stream >> std::ws;
					const auto start = stream.tellg();
					if (start == std::istream::pos_type(-1) || stream.get() != '"') return false;
					size_t length = 0;
					char character = 0;
					bool closed = false;
					while (stream.get(character)) {
						if (character == '"') {
							closed = true;
							break;
						}
						if (character == '\\' && !stream.get(character)) return false;
						++length;
					}
					if (!closed || length > Limits::MaximumTextBytes) return false;
					stream.seekg(start);
					auto storage = budget->Reserve(length);
					if (!storage) {
						*allocationRefused = true;
						return false;
					}
					stored = std::string(length, '\0');
					if (!storage->Resize(stored.capacity())) {
						*allocationRefused = true;
						return false;
					}
					if (!outputCharge->Merge(std::move(*storage))) std::terminate();
					stored.clear();
				}
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
				TokenCharge typeCharge;
				std::string typeName;
				size_t count = 0;
				if (!ReadToken(stream, typeName, budget, typeCharge, allocationRefused) ||
					!(stream >> count) || count > Limits::MaximumArrayElements)
					return false;
				if (count > Limits::MaximumArrayElements - *arrayCount) return false;
				*arrayCount += count;
				if (typeName == "array") {
					if (version < 9 || !count || !admit(count * sizeof(std::vector<ElementValue>)))
						return false;
					ArrayValue array;
					array.Nested.reserve(count);
					for (size_t index = 0; index < count; ++index) {
						Value element;
						if (!ReadValue(
								stream,
								element,
								version,
								true,
								budget,
								outputCharge,
								allocationRefused,
								depth + 1,
								arrayCount
							))
							return false;
						auto *row = std::get_if<ArrayValue>(&element);
						if (!row || !row->Nested.empty() || !row->Items.empty() || !ValidArray(*row))
							return false;
						if (index == 0)
							array.ElementType = row->ElementType;
						else if (row->ElementType != array.ElementType)
							return false;
						array.Nested.push_back(std::move(row->Elements));
					}
					if (!ValidArray(array)) return false;
					value = std::move(array);
					return true;
				}
				if (typeName == "any") {
					if (version < 9 || !admit(count * sizeof(SourceArrayItem))) return false;
					ArrayValue array{ValueType::Any, {}};
					array.Items.reserve(count);
					for (size_t index = 0; index < count; ++index) {
						Value element;
						if (!ReadValue(
								stream,
								element,
								version,
								true,
								budget,
								outputCharge,
								allocationRefused,
								depth + 1,
								arrayCount
							))
							return false;
						if (auto *nested = std::get_if<ArrayValue>(&element)) {
							if (!nested->Nested.empty()) return false;
							if (nested->ElementType == ValueType::Any)
								array.Items.push_back({std::move(nested->Items)});
							else {
								if (!admit(nested->Elements.size() * sizeof(SourceArrayItem))) return false;
								std::vector<SourceArrayItem> children;
								children.reserve(nested->Elements.size());
								for (auto &leaf : nested->Elements)
									children.push_back({std::move(leaf)});
								array.Items.push_back({std::move(children)});
							}
						} else {
							std::visit(
								[&](auto &leaf) {
									using L = std::decay_t<decltype(leaf)>;
									if constexpr (std::is_constructible_v<ElementValue, L>)
										array.Items.push_back({ElementValue{std::move(leaf)}});
								},
								element
							);
						}
					}
					value = std::move(array);
					return IsFinite(value);
				}
				const auto type = ParseType(typeName);
				if (!type || *type == ValueType::Image || *type == ValueType::Array ||
					(*type >= ValueType::Gradient &&
					 !(version >= 9 && (*type == ValueType::Area || *type == ValueType::Enum ||
										*type == ValueType::Particle || *type == ValueType::Tileset ||
										*type == ValueType::Rigid || *type == ValueType::Atlas ||
										*type == ValueType::Strand))))
					return false;
				ArrayValue array{*type, {}};
				if (!admit(count * sizeof(ElementValue))) return false;
				array.Elements.reserve(count);
				for (size_t index = 0; index < count; index++) {
					Value element;
					if (!ReadValue(
							stream, element, version, false, budget, outputCharge, allocationRefused
						) ||
						TypeOf(element) != *type)
						return false;
					std::visit(
						[&](auto &item) {
							using T = std::decay_t<decltype(item)>;
							if constexpr (std::is_constructible_v<ElementValue, T>)
								array.Elements.emplace_back(std::move(item));
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
				if (!admit(count * sizeof(GradientKey))) return false;
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
				if (!admit(count * sizeof(std::array<double, 6>))) return false;
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
			if (tag == "m") {
				MatrixValue matrix;
				if (!(stream >> matrix.Columns >> matrix.Rows) || matrix.Columns == 0 || matrix.Rows == 0 ||
					uint64_t(matrix.Columns) * matrix.Rows > Limits::MaximumArrayElements)
					return false;
				if (!admit(uint64_t(matrix.Columns) * matrix.Rows * sizeof(double))) return false;
				matrix.Values.assign(size_t(matrix.Columns) * matrix.Rows, 0.0);
				for (double &number : matrix.Values)
					if (!(stream >> number) || !std::isfinite(number)) return false;
				value = std::move(matrix);
				return true;
			}
			if (tag == "u") {
				AudioBit audio;
				size_t count = 0;
				if (!(stream >> audio.SampleRate >> count) || !std::isfinite(audio.SampleRate) ||
					audio.SampleRate <= 0.0 || count > Limits::MaximumAudioSamplesPerFrame)
					return false;
				if (!admit(count * sizeof(double))) return false;
				audio.Samples.reserve(count);
				for (size_t index = 0; index < count; ++index) {
					double sample = 0.0;
					if (!(stream >> sample) || !std::isfinite(sample)) return false;
					audio.Samples.push_back(sample);
				}
				value = std::move(audio);
				return true;
			}
			if (tag == "strand2") {
				if (version < 9) return false;
				StrandValue strand;
				if (!detail::ReadStrandValue(stream, strand, admit)) return false;
				value = std::move(strand);
				return true;
			}
			if (tag == "particle2") {
				if (version < 9) return false;
				ParticleValue particle;
				if (!detail::ReadParticleValue(stream, particle, admit)) return false;
				value = std::move(particle);
				return true;
			}
			if (tag == "rg") {
				if (version < 9) return false;
				RigidValue rigid;
				if (!detail::ReadSourceRigidAlias(
						stream, rigid, admit, [](auto &input, auto &text, size_t count) {
							return ReadQuoted(input, text, count);
						}
					))
					return false;
				value = std::move(rigid);
				return true;
			}
			if (tag == "at") {
				if (version < 9) return false;
				AtlasValue atlas;
				if (!detail::ReadAtlasValue(stream, atlas, admit)) return false;
				value = std::move(atlas);
				return true;
			}
			if (tag == "tileset") {
				if (version < 9) return false;
				TilesetValue tileset;
				if (!detail::ReadTilesetValue(stream, tileset, admit)) return false;
				value = std::move(tileset);
				return true;
			}
			if (tag == "pb") {
				if (version < 9) return false;
				unsigned present = 0;
				if (!(stream >> present) || present > 1) return false;
				PixelBoxValue stored;
				if (present) {
					if (!admit(sizeof(PixelBoxData))) return false;
					PixelBoxData &box = stored.Data.emplace();
					for (double &field : box.BaseBounds)
						if (!(stream >> field) || !std::isfinite(field)) return false;
					unsigned fixed = 0;
					if (!(stream >> fixed) || fixed > 1) return false;
					if (fixed) {
						box.FixedBounds.emplace();
						for (double &field : *box.FixedBounds)
							if (!(stream >> field) || !std::isfinite(field)) return false;
					}
					for (uint8_t &mode : box.AnchorModes) {
						unsigned storedMode = 0;
						if (!(stream >> storedMode) || storedMode > 3) return false;
						mode = static_cast<uint8_t>(storedMode);
					}
					for (uint8_t &mode : box.PreviousAnchorModes) {
						unsigned storedMode = 0;
						if (!(stream >> storedMode) || storedMode > 3) return false;
						mode = static_cast<uint8_t>(storedMode);
					}
					for (double &anchor : box.Anchors)
						if (!(stream >> anchor) || !std::isfinite(anchor)) return false;
					for (bool &fractional : box.Fractional) {
						unsigned storedFractional = 0;
						if (!(stream >> storedFractional) || storedFractional > 1) return false;
						fractional = storedFractional != 0;
					}
					for (uint8_t &mode : box.DimensionBoundModes) {
						unsigned storedMode = 0;
						if (!(stream >> storedMode) || storedMode != 0) return false;
						mode = static_cast<uint8_t>(storedMode);
					}
					for (double &bound : box.DimensionBounds)
						if (!(stream >> bound) || !std::isfinite(bound)) return false;
					if (!IsFinite(Value{stored})) return false;
				}
				value = std::move(stored);
				return true;
			}
			if ((tag == "p3" || tag == "p3o" || tag == "p3s") && version >= 9) {
				unsigned present = 0, loop = 0, sourcePresent = 1, source2d = 0;
				uint32_t resolution = 32;
				size_t count = 0, transforms = 0;
				if (!(stream >> present) || present > 1) return false;
				PathValue3D path;
				std::optional<SourcePolylineEmptyCache3D> emptyCache;
				std::optional<Vector4> sourceBounds;
				if (present) {
					if (tag == "p3s") {
						stream >> std::ws;
						for (char expected : std::string_view{"source_polyline"})
							if (stream.get() != expected) return false;
						const int next = stream.peek();
						if (next == std::char_traits<char>::eof() ||
							!std::isspace(static_cast<unsigned char>(next)))
							return false;
						unsigned marker = 0;
						if (!(stream >> marker) || marker != 1) return false;
						stream >> std::ws;
						for (char expected : std::string_view{"source_empty_cache"})
							if (stream.get() != expected) return false;
						const int boundary = stream.peek();
						if (boundary == std::char_traits<char>::eof() ||
							!std::isspace(static_cast<unsigned char>(boundary)))
							return false;
						unsigned cached = 0;
						if (!(stream >> cached) || cached > 1) return false;
						if (cached) {
							SourcePolylineEmptyCache3D cache;
							if (!(stream >> cache.Length >> cache.SegmentCount) ||
								!std::isfinite(cache.Length) || cache.Length < 0 || !cache.SegmentCount ||
								cache.SegmentCount > Limits::MaximumPathAnchors)
								return false;
							emptyCache = cache;
						}
					}
					if (tag == "p3s") {
						stream >> std::ws;
						if (stream.peek() == 's') {
							for (char expected : std::string_view{"source_metadata"})
								if (stream.get() != expected) return false;
							const int separator = stream.peek();
							if (separator == std::char_traits<char>::eof() ||
								!std::isspace(static_cast<unsigned char>(separator)))
								return false;
							unsigned revision = 0, presentBounds = 0;
							size_t accumulated = 0;
							if (!(stream >> revision >> presentBounds) || revision != 1 || presentBounds > 1)
								return false;
							if (presentBounds) {
								Vector4 bounds;
								if (!(stream >> bounds.X >> bounds.Y >> bounds.Z >> bounds.W) ||
									(!std::isfinite(bounds.X) || !std::isfinite(bounds.Y) ||
									 !std::isfinite(bounds.Z) || !std::isfinite(bounds.W)))
									return false;
								sourceBounds = bounds;
							}
							if (!(stream >> accumulated) || accumulated > Limits::MaximumArrayElements ||
								(accumulated && (!emptyCache || accumulated != emptyCache->SegmentCount)) ||
								!admit(accumulated * sizeof(double)))
								return false;
							if (emptyCache) {
								emptyCache->Accumulated.reserve(accumulated);
								for (size_t i = 0; i < accumulated; ++i) {
									double length = 0;
									if (!(stream >> length) || !std::isfinite(length)) return false;
									emptyCache->Accumulated.push_back(length);
								}
							}
						}
					}
					if (!(stream >> loop >> resolution >> count >> sourcePresent >> source2d >> transforms) ||
						loop > 1 || resolution == 0 || resolution > Limits::MaximumArrayElements ||
						count > Limits::MaximumPathAnchors || sourcePresent > 1 || source2d > 1 ||
						transforms > Limits::MaximumArrayDepth)
						return false;
					if (!admit(
							sizeof(PathData3D) + count * sizeof(PathAnchor3D) +
							transforms * sizeof(PathTransform3D)
						))
						return false;
					auto &data = path.Data.emplace();
					data.Loop = bool(loop);
					data.SourcePresent = bool(sourcePresent);
					data.SourcePolyline = tag == "p3s";
					data.SourceEmptyCache = std::move(emptyCache);
					data.SourceBounds2D = sourceBounds;
					data.Resolution = resolution;
					data.Anchors.reserve(count);
					for (size_t i = 0; i < count; ++i) {
						PathAnchor3D anchor;
						for (double &number : anchor.Controls)
							if (!(stream >> number) || !std::isfinite(number)) return false;
						if (!(stream >> anchor.Index) || !std::isfinite(anchor.Index)) return false;
						data.Anchors.push_back(anchor);
					}
					if (source2d) {
						Value source;
						if (!ReadValue(
								stream,
								source,
								version,
								false,
								budget,
								outputCharge,
								allocationRefused,
								depth + 1,
								arrayCount
							))
							return false;
						auto *planar = std::get_if<Path2D>(&source);
						if (!planar) return false;
						data.Source2D = std::move(*planar);
					}
					data.Transforms.reserve(transforms);
					for (size_t i = 0; i < transforms; ++i) {
						PathTransform3D transform;
						double *fields[]{
							&transform.Position.X,
							&transform.Position.Y,
							&transform.Position.Z,
							&transform.Anchor.X,
							&transform.Anchor.Y,
							&transform.Anchor.Z,
							&transform.Scale.X,
							&transform.Scale.Y,
							&transform.Scale.Z,
							&transform.Rotation.X,
							&transform.Rotation.Y,
							&transform.Rotation.Z,
							&transform.Rotation.W
						};
						for (double *number : fields)
							if (!(stream >> *number) || !std::isfinite(*number)) return false;
						unsigned projective = 0, depthWeight = 0;
						if (!(stream >> projective) || projective > 1) return false;
						transform.Projective = bool(projective);
						if (projective) {
							if (!(stream >> depthWeight >> transform.ProjectionScale.X >>
								  transform.ProjectionScale.Y) ||
								depthWeight > 1 || !detail::MeshFinite(transform.ProjectionScale))
								return false;
							transform.DepthWeight = bool(depthWeight);
							for (double &number : transform.CameraView)
								if (!(stream >> number) || !std::isfinite(number)) return false;
							for (double &number : transform.CameraProjection)
								if (!(stream >> number) || !std::isfinite(number)) return false;
						}
						data.Transforms.push_back(transform);
					}
					if (tag == "p3o") {
						unsigned kind = 0;
						size_t children = 0;
						Vector2 range;
						if (!(stream >> kind >> range.X >> range.Y >> children) ||
							(kind != unsigned(SourcePathOperationKind::Reverse) &&
							 kind != unsigned(SourcePathOperationKind::Combine) &&
							 kind != unsigned(SourcePathOperationKind::Trim)) ||
							!detail::MeshFinite(range) || children > Limits::MaximumArrayElements ||
							(kind != unsigned(SourcePathOperationKind::Combine) && children > 1) ||
							!admit(sizeof(SourcePathData3D) + children * sizeof(PathValue3D)))
							return false;
						auto &op = data.SourceOperation.emplace();
						op.Kind = SourcePathOperationKind(kind);
						op.TrimRange = range;
						op.Inputs.reserve(children);
						for (size_t i = 0; i < children; ++i) {
							Value child;
							if (!ReadValue(
									stream,
									child,
									version,
									false,
									budget,
									outputCharge,
									allocationRefused,
									depth + 1,
									arrayCount
								))
								return false;
							auto *spatial = std::get_if<PathValue3D>(&child);
							if (!spatial || !spatial->Data) return false;
							op.Inputs.push_back(std::move(*spatial));
						}
					}
					if (!detail::ValidSourcePath3D(data)) return false;
				} else if (tag == "p3o" || tag == "p3s")
					return false;
				value = std::move(path);
				return true;
			}

			if (tag == "po" && version >= 9) {
				TokenCharge kindCharge;
				std::string kind;
				size_t count = 0;
				if (!ReadToken(stream, kind, budget, kindCharge, allocationRefused)) return false;
				if (kind == "bake") {
					if (!admit(sizeof(SourcePathData2D) + sizeof(SourcePathBakedData2D))) return false;
					Path2D path;
					auto &operation = path.SourceOperation.emplace();
					operation.Kind = SourcePathOperationKind::Bake;
					if (!detail::ReadSourceBaked(stream, operation.Baked.emplace(), admit)) return false;
					value = std::move(path);
					return true;
				}
				if (kind == "shape") {
					if (!admit(sizeof(SourcePathData2D))) return false;
					Path2D path;
					auto &operation = path.SourceOperation.emplace();
					operation.Kind = SourcePathOperationKind::Shape;
					if (!detail::ReadSourcePathShape(stream, operation.Shape.emplace(), admit)) return false;
					value = std::move(path);
					return true;
				}
				if (kind == "spiral") {
					if (!(stream >> count) || count > 1 ||
						!admit(
							sizeof(SourcePathData2D) + sizeof(SourcePathSpiralData2D) + count * sizeof(Path2D)
						))
						return false;
					Path2D path;
					auto &operation = path.SourceOperation.emplace();
					operation.Kind = SourcePathOperationKind::Spiral;
					if (!detail::ReadSourcePathSpiral(stream, operation.Spiral.emplace(), admit))
						return false;
					unsigned spatial = 0;
					if (!(stream >> spatial) || spatial > 1 || (spatial && count)) return false;
					if (spatial) {
						Value child;
						if (!ReadValue(
								stream,
								child,
								version,
								false,
								budget,
								outputCharge,
								allocationRefused,
								depth + 1,
								arrayCount
							))
							return false;
						auto *path3d = std::get_if<PathValue3D>(&child);
						if (!path3d || !path3d->Data) return false;
						operation.WeightInput3D = std::move(path3d->Data);
					}
					operation.Inputs.reserve(count);
					for (size_t index = 0; index < count; ++index) {
						Value child;
						if (!ReadValue(
								stream,
								child,
								version,
								false,
								budget,
								outputCharge,
								allocationRefused,
								depth + 1,
								arrayCount
							))
							return false;
						auto *planar = std::get_if<Path2D>(&child);
						if (!planar) return false;
						operation.Inputs.push_back(std::move(*planar));
					}
					value = std::move(path);
					return true;
				}
				if (kind == "verlet") {
					if (!admit(sizeof(SourcePathData2D))) return false;
					Path2D path;
					auto &operation = path.SourceOperation.emplace();
					operation.Kind = SourcePathOperationKind::VerletMesh;
					if (!detail::ReadSourceVerletPath(stream, operation, admit)) return false;
					value = std::move(path);
					return true;
				}
				if (!(stream >> count) || count > Limits::MaximumArrayElements ||
					(kind != "reverse" && kind != "repeat" && kind != "combine" && kind != "trim" &&
					 kind != "offset" && kind != "blend" && kind != "join" && kind != "redistribute" &&
					 kind != "skew" && kind != "transform" && kind != "area_map" && kind != "shift" &&
					 kind != "weight_adjust" && kind != "extends" && kind != "flatten" &&
					 kind != "smoothen") ||
					(kind == "blend" ? count != 2
					 : (kind == "skew" || kind == "redistribute")
						 ? count != 1
						 : (kind != "combine" && kind != "repeat" && kind != "join" && count > 1)))
					return false;
				if (!admit(sizeof(SourcePathData2D) + count * sizeof(Path2D))) return false;
				Path2D path;
				auto &operation = path.SourceOperation.emplace();
				operation.Kind = kind == "reverse"		   ? SourcePathOperationKind::Reverse
								 : kind == "repeat"		   ? SourcePathOperationKind::Repeat
								 : kind == "trim"		   ? SourcePathOperationKind::Trim
								 : kind == "offset"		   ? SourcePathOperationKind::Offset
								 : kind == "blend"		   ? SourcePathOperationKind::Blend
								 : kind == "join"		   ? SourcePathOperationKind::Join
								 : kind == "redistribute"  ? SourcePathOperationKind::Redistribute
								 : kind == "skew"		   ? SourcePathOperationKind::Skew
								 : kind == "transform"	   ? SourcePathOperationKind::Transform
								 : kind == "area_map"	   ? SourcePathOperationKind::AreaMap
								 : kind == "shift"		   ? SourcePathOperationKind::Shift
								 : kind == "extends"	   ? SourcePathOperationKind::Extends
								 : kind == "flatten"	   ? SourcePathOperationKind::Flatten
								 : kind == "smoothen"	   ? SourcePathOperationKind::Smoothen
								 : kind == "weight_adjust" ? SourcePathOperationKind::WeightAdjust
														   : SourcePathOperationKind::Combine;
				if (kind == "trim" &&
					(!(stream >> operation.TrimRange.X >> operation.TrimRange.Y) ||
					 !std::isfinite(operation.TrimRange.X) || !std::isfinite(operation.TrimRange.Y)))
					return false;

				if (kind == "offset" && (!(stream >> operation.Offset >> operation.ClampOffset) ||
										 !std::isfinite(operation.Offset)))
					return false;
				if (kind == "blend") {
					unsigned mode = 0;
					if (!(stream >> mode >> operation.BlendAmount >> operation.BlendInputsValid[0] >>
						  operation.BlendInputsValid[1]) ||
						mode > 3 || !std::isfinite(operation.BlendAmount))
						return false;
					operation.BlendMode = uint8_t(mode);
					size_t lines = 0;
					if (!(stream >> lines) || lines > Limits::MaximumArrayElements ||
						!admit(lines * (sizeof(double) + sizeof(std::vector<double>))))
						return false;
					operation.BlendLengths.reserve(lines);
					operation.BlendAccumulated.reserve(lines);
					size_t total = lines;
					for (size_t line = 0; line < lines; ++line) {
						double length = 0;
						size_t points = 0;
						if (!(stream >> length >> points) || !std::isfinite(length) ||
							points > Limits::MaximumArrayElements - total || !admit(points * sizeof(double)))
							return false;
						total += points;
						operation.BlendLengths.push_back(length);
						auto &row = operation.BlendAccumulated.emplace_back();
						row.reserve(points);
						for (size_t i = 0; i < points; ++i) {
							double value = 0;
							if (!(stream >> value) || !std::isfinite(value)) return false;
							row.push_back(value);
						}
					}
				}

				if (kind == "redistribute") {
					operation.RedistributeMap.emplace();
					for (double &value : *operation.RedistributeMap)
						if (!(stream >> value) || !std::isfinite(value)) return false;
				}
				if (kind == "skew") {
					unsigned axis = 0;
					if (!(stream >> axis >> operation.SkewStrength >> operation.SkewCenter.X >>
						  operation.SkewCenter.Y) ||
						axis > 1 || !std::isfinite(operation.SkewStrength) ||
						!std::isfinite(operation.SkewCenter.X) || !std::isfinite(operation.SkewCenter.Y))
						return false;
					operation.SkewAxis = uint8_t(axis);
				}
				if (kind == "transform") {
					if (!(stream >> operation.TransformPosition.X >> operation.TransformPosition.Y >>
						  operation.TransformAnchor.X >> operation.TransformAnchor.Y >>
						  operation.TransformScale.X >> operation.TransformScale.Y >>
						  operation.TransformRotation))
						return false;
				}
				if (kind == "area_map") {
					if (!(stream >> operation.MapFrom.X >> operation.MapFrom.Y >> operation.MapFrom.Z >>
						  operation.MapFrom.W >> operation.MapArea.X >> operation.MapArea.Y >>
						  operation.MapArea.Z >> operation.MapArea.W))
						return false;
				}
				if (kind == "weight_adjust") {
					unsigned type = 0, mode = 0;
					size_t samples = 0;
					if (!(stream >> type >> mode >> operation.WeightValue >> operation.WeightDirection >>
						  operation.WeightRange.X >> operation.WeightRange.Y >> operation.WeightLoop >>
						  samples) ||
						type > 2 || mode > 2 || samples < 2 || samples > Limits::MaximumArrayElements ||
						!admit(samples * sizeof(double)))
						return false;
					operation.WeightType = uint8_t(type);
					operation.WeightMode = uint8_t(mode);
					operation.WeightCurve.reserve(samples);
					for (size_t i = 0; i < samples; ++i) {
						double value = 0;
						if (!(stream >> value) || !std::isfinite(value)) return false;
						operation.WeightCurve.push_back(value);
					}
					unsigned spatial = 0;
					if (!(stream >> spatial) || spatial > 1 || (spatial && count)) return false;
					if (spatial) {
						Value child;
						if (!ReadValue(
								stream,
								child,
								version,
								false,
								budget,
								outputCharge,
								allocationRefused,
								depth + 1,
								arrayCount
							))
							return false;
						auto *path3d = std::get_if<PathValue3D>(&child);
						if (!path3d || !path3d->Data) return false;
						operation.WeightInput3D = std::move(path3d->Data);
					}
				}
				if (detail::SourceSequentialKind(operation.Kind)) {
					if (count || !admit(sizeof(SourcePathSequentialData2D)) ||
						!detail::ReadSourceSequential(
							stream, operation.Sequential.emplace(), operation.Kind, admit
						))
						return false;
					unsigned present = 0;
					if (!(stream >> present) || present > 1) return false;
					if (present) {
						Value child;
						if (!ReadValue(
								stream,
								child,
								version,
								false,
								budget,
								outputCharge,
								allocationRefused,
								depth + 1,
								arrayCount
							))
							return false;
						auto *spatial = std::get_if<PathValue3D>(&child);
						if (!spatial || !spatial->Data) return false;
						operation.WeightInput3D = std::move(spatial->Data);
					}
				}
				if (kind == "shift" && !(stream >> operation.ShiftDistance >> operation.ShiftRange.X >>
										 operation.ShiftRange.Y >> operation.ShiftLoop))
					return false;
				if (kind == "join") {
					if (!admit(count * sizeof(uint8_t))) return false;
					operation.Reversed.reserve(count);
					for (size_t i = 0; i < count; ++i) {
						unsigned reverse = 0;
						if (!(stream >> reverse) || reverse > 1) return false;
						operation.Reversed.push_back(uint8_t(reverse));
					}
				}
				operation.Inputs.reserve(count);
				for (size_t index = 0; index < count; ++index) {
					Value child;
					if (!ReadValue(
							stream,
							child,
							version,
							true,
							budget,
							outputCharge,
							allocationRefused,
							depth + 1,
							arrayCount
						))
						return false;
					const auto *typed = std::get_if<Path2D>(&child);
					if (!typed) return false;
					operation.Inputs.push_back(std::move(std::get<Path2D>(child)));
				}
				if (operation.Kind == SourcePathOperationKind::Repeat && !detail::ValidSourcePath2D(path))
					return false;
				value = std::move(path);
				return true;
			}
			if (tag == "p" || (tag == "ps" && version >= 9)) {
				unsigned loop = 0;
				size_t anchorCount = 0, weightCount = 0;
				if (!(stream >> loop >> anchorCount >> weightCount) || loop > 1 ||
					anchorCount > Limits::MaximumPathAnchors || weightCount > Limits::MaximumPathWeights)
					return false;
				Path2D path;
				path.Segmented = tag == "ps";
				path.Loop = loop != 0;
				if (!admit(anchorCount * sizeof(PathAnchor) + weightCount * sizeof(PathWeight))) return false;
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
			return ReadQuoted(stream, text, std::numeric_limits<size_t>::max());
		}

		bool ReadQuoted(std::istream &stream, std::string &text, size_t maximum) {
			stream >> std::ws;
			if (stream.get() != '"') return false;
			text.clear();
			const auto append = [&](char value) {
				if (text.size() == maximum) return false;
				text.push_back(value);
				return true;
			};
			char character = 0;
			while (stream.get(character)) {
				if (character == '"') return true;
				if (character != '\\') {
					if (!append(character)) return false;
					continue;
				}
				if (!stream.get(character)) return false;
				switch (character) {
				case '\\':
					if (!append('\\')) return false;
					break;
				case '"':
					if (!append('"')) return false;
					break;
				case 'n':
					if (!append('\n')) return false;
					break;
				case 'r':
					if (!append('\r')) return false;
					break;
				case 't':
					if (!append('\t')) return false;
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
	} // namespace

	bool CatalogueJunctionCompatible(ValueType from, ValueType to) {
		return JunctionCompatible(from, to);
	}

	const NodeSchema *FindSchema(std::string_view type) {
		if (type == GROUP_CALLBACK_OPAQUE_SCHEMA.Type) return &GROUP_CALLBACK_OPAQUE_SCHEMA;
		if (type == CAPTURED_IMAGE_SCHEMA.Type) return &CAPTURED_IMAGE_SCHEMA;
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
		if (const CatalogueEntry *entry = FindCatalogueEntry(type)) return &entry->Schema;
		return nullptr;
	}

	std::string_view ValueTypeName(ValueType type) {
		return TypeName(type);
	}

	std::optional<ValueType> ParseValueTypeName(std::string_view name) {
		return ParseType(name);
	}

	bool IsAuthoredValueType(ValueType type) {
		return (type != ValueType::Image && type != ValueType::Mesh && type < ValueType::AudioBit) ||
			   type == ValueType::Matrix || type == ValueType::Path3D;
	}

	void detail::WriteValueText(std::ostream &stream, const Value &value) {
		WriteValue(stream, value);
	}

	bool detail::ReadValueText(std::string_view text, Value &value) {
		std::istringstream stream{std::string(text)};
		stream.imbue(std::locale::classic());
		// Recursive source arrays use the version 9 value encoding.
		return ReadValue(stream, value, 9) && !HasTrailing(stream);
	}

	Status detail::ReadValueText(
		std::string_view text, Value &value, EvaluationBudget &budget, AllocationReservation &charge
	) {
		// The copied stream text and lexical tag/type scratch coexist with the parsed
		// value.
		auto workspace = budget.Reserve(5 * text.size() + 4 * std::string{}.capacity());
		if (!workspace) return Status::LimitExceeded;
		auto parsedCharge = budget.Reserve(0);
		if (!parsedCharge) return Status::LimitExceeded;
		std::istringstream stream{std::string(text)};
		stream.imbue(std::locale::classic());
		Value parsed;
		bool allocationRefused = false;
		if (!ReadValue(stream, parsed, 9, true, &budget, &*parsedCharge, &allocationRefused) ||
			HasTrailing(stream))
			return allocationRefused ? Status::LimitExceeded : Status::InvalidValue;
		value = std::move(parsed);
		if (!charge.Merge(std::move(*parsedCharge))) std::terminate();
		return Status::Ok;
	}

	bool detail::WriteKeyframeText(std::ostream &stream, std::span<const Keyframe> keys, uint32_t version) {
		for (const Keyframe &keyframe : keys) {
			if (keyframe.SourceKeyId.size() > Limits::MaximumSourceKeyIdBytes ||
				(version < 9 && !keyframe.SourceKeyId.empty()))
				return false;
			stream << "keyframe ";
			WriteQuoted(stream, keyframe.NodeId);
			stream << ' ';
			WriteQuoted(stream, keyframe.Port);
			stream << ' ' << keyframe.Tick << ' ';
			WriteQuoted(stream, keyframe.Interpolation);
			stream << ' ';
			WriteValue(stream, keyframe.Data);
			stream << '\n';
			if (!keyframe.SourceKeyId.empty()) {
				stream << "key_source_id ";
				WriteQuoted(stream, keyframe.NodeId);
				stream << ' ';
				WriteQuoted(stream, keyframe.Port);
				stream << ' ' << keyframe.Tick << ' ';
				WriteQuoted(stream, keyframe.SourceKeyId);
				stream << '\n';
			}
			if (version >= 9 && (keyframe.Subframe != 0 || keyframe.NegativeFrame)) {
				stream << "key_time ";
				WriteQuoted(stream, keyframe.NodeId);
				stream << ' ';
				WriteQuoted(stream, keyframe.Port);
				stream << ' ' << keyframe.Tick << ' ' << (keyframe.NegativeFrame ? "negative" : "positive")
					   << ' ' << keyframe.Subframe << '\n';
			}
			if (version >= 9 && keyframe.Kind != KeyframeKind::Normal) {
				stream << "key_kind ";
				WriteQuoted(stream, keyframe.NodeId);
				stream << ' ';
				WriteQuoted(stream, keyframe.Port);
				stream << ' ' << keyframe.Tick << ' '
					   << (keyframe.Kind == KeyframeKind::Adder ? "adder" : "invalid") << '\n';
			}
			if (version >= 4 && keyframe.Ease) {
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
			if (version >= 6 && keyframe.SineDriver) {
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
			if (version >= 8 && keyframe.SourceDriver) {
				stream << "key_source_driver ";
				WriteQuoted(stream, keyframe.NodeId);
				stream << ' ';
				WriteQuoted(stream, keyframe.Port);
				stream << ' ' << keyframe.Tick << ' ';
				std::visit(
					[&](const auto &driver) {
						using T = std::decay_t<decltype(driver)>;
						stream << std::setprecision(17);
						if constexpr (std::is_same_v<T, KeyframeLinearDriver>) {
							WriteQuoted(stream, "linear");
							stream << ' ' << driver.Speed;
						} else if constexpr (std::is_same_v<T, KeyframeSnapDriver>) {
							WriteQuoted(stream, "snap");
							stream << ' ' << driver.Size;
						} else if constexpr (std::is_same_v<T, KeyframeSineDriver>) {
							WriteQuoted(stream, "sine");
							stream << ' ' << driver.Frequency << ' ' << driver.Amplitude << ' '
								   << driver.Phase << ' ' << driver.Smooth;
						} else if constexpr (std::is_same_v<T, KeyframeAudioDriver>) {
							WriteQuoted(stream, "native_audio");
							stream << ' ';
							WriteQuoted(stream, driver.SourceId);
							stream << ' ';
							WriteQuoted(stream, driver.Metric);
							stream << ' ' << driver.Channel << ' ' << driver.Gain << ' ' << driver.Bias;
						} else if constexpr (std::is_same_v<T, KeyframeCurveDriver>) {
							WriteQuoted(stream, "curve");
							stream << ' ';
							WriteValue(stream, driver.Data);
						} else {
							WriteQuoted(
								stream, std::is_same_v<T, KeyframeBounceDriver> ? "bounce" : "elastic"
							);
							stream << ' ' << driver.Amount << ' ' << driver.Spacing << ' ' << driver.Curve;
						}
					},
					*keyframe.SourceDriver
				);
				stream << '\n';
			}
		}
		return true;
	}

	std::string Write(const Document &document) {
		if (document.FormatVersion < 10 &&
			std::any_of(
				document.Groups.begin(),
				document.Groups.end(),
				[](const auto &group) { return !group.RenderActive || !group.PureFunction; }
			))
			return {};
		Diagnostic sourceAnimatorDiagnostic;
		if (detail::ValidateSourceAnimatorState(document, sourceAnimatorDiagnostic) != Status::Ok) return {};
		for (const auto &node : document.Nodes) {
			if (!node.SourceParentInputBase.empty() && document.FormatVersion < 10) return {};
			if (detail::ValidateNativeSamplerBindings(node, document.FormatVersion)) return {};
			Diagnostic defaultsDiagnostic;
			if ((node.SourceVec2Defaults && document.FormatVersion < 9) ||
				detail::ValidateSourceVec2Defaults(node, defaultsDiagnostic) != Status::Ok)
				return {};
		}
		if (document.Timeline && document.Timeline->SourceBounds &&
			(document.FormatVersion < 9 ||
			 !ValidSourceAuthoringFrameBounds(*document.Timeline->SourceBounds)))
			return {};
		if (document.Project && (!ValidProjectAnimationRegions(*document.Project) ||
								 (document.FormatVersion < 9 && !document.Project->AnimationRegions.empty())))
			return {};
		std::ostringstream stream;
		stream.imbue(std::locale::classic());
		stream << "imagegraph " << document.FormatVersion << '\n';
		if (document.FormatVersion < 9 && !document.SliceStackActions.empty()) return {};
		for (const auto &action : document.SliceStackActions) {
			stream << "slice_stack_action ";
			WriteQuoted(stream, action.NodeId);
			stream << ' ' << action.Time.Tick << ' ' << std::setprecision(17) << action.Time.Subframe << ' '
				   << action.Time.NegativeFrame << ' ' << action.WorkPixels << '\n';
		}
		for (size_t nodeIndex = 0; nodeIndex < document.Nodes.size(); nodeIndex++) {
			const Node &node = document.Nodes[nodeIndex];
			stream << "node ";
			WriteQuoted(stream, node.Id);
			stream << ' ';
			WriteQuoted(stream, node.Type);
			stream << ' ';
			WriteQuoted(stream, node.GroupId);
			stream << ' ' << std::setprecision(17) << node.Position.X << ' ' << node.Position.Y << '\n';
			if (document.FormatVersion >= 10 && !node.SourceParentInputBase.empty()) {
				stream << "source_parent_input_base ";
				WriteQuoted(stream, node.Id);
				stream << ' ';
				WriteQuoted(stream, node.SourceParentInputBase);
				stream << '\n';
			}
			if (document.FormatVersion >= 9 && !node.InstanceBase.empty()) {
				stream << "node_instance ";
				WriteQuoted(stream, node.Id);
				stream << ' ';
				WriteQuoted(stream, node.InstanceBase);
				stream << '\n';
			}
			if (document.FormatVersion >= 9)
				for (const auto &port : node.InstanceOverrides) {
					stream << "node_instance_override ";
					WriteQuoted(stream, node.Id);
					stream << ' ';
					WriteQuoted(stream, port);
					stream << '\n';
				}
			if (document.FormatVersion >= 9)
				for (const auto &port : node.SourceAnimatedInputs) {
					stream << "source_anim ";
					WriteQuoted(stream, node.Id);
					stream << ' ';
					WriteQuoted(stream, port);
					stream << '\n';
				}
			if (document.FormatVersion >= 9)
				for (const auto &port : node.SourceStaticInputs) {
					stream << "source_static ";
					WriteQuoted(stream, node.Id);
					stream << ' ';
					WriteQuoted(stream, port);
					stream << '\n';
				}
			if (!node.SourceDisplayName.empty()) {
				stream << "node_name " << nodeIndex << ' ';
				WriteQuoted(stream, node.Id);
				stream << ' ';
				WriteQuoted(stream, node.SourceDisplayName);
				stream << '\n';
			}
			if (!node.SourceInternalName.empty()) {
				stream << "node_internal_name " << nodeIndex << ' ';
				WriteQuoted(stream, node.Id);
				stream << ' ';
				WriteQuoted(stream, node.SourceInternalName);
				stream << '\n';
			}
			for (const AuthoredValue &value : node.Values) {
				stream << "value " << nodeIndex << ' ';
				WriteQuoted(stream, node.Id);
				stream << ' ';
				WriteQuoted(stream, value.Port);
				stream << ' ';
				WriteValue(stream, value.Data);
				stream << '\n';
			}
			for (const auto &binding : node.NativeSamplerBindings) {
				stream << "native_sampler " << nodeIndex << ' ';
				WriteQuoted(stream, node.Id);
				stream << ' ';
				WriteQuoted(stream, binding.Argument);
				stream << ' ';
				WriteQuoted(stream, binding.Texture);
				stream << '\n';
			}
			for (const AuthoredValue &property : node.SourceProperties) {
				stream << "source_property " << nodeIndex << ' ';
				WriteQuoted(stream, node.Id);
				stream << ' ';
				WriteQuoted(stream, property.Port);
				stream << ' ';
				WriteValue(stream, property.Data);
				stream << '\n';
			}
			if (document.FormatVersion >= 9)
				for (const DynamicOutput &output : node.DynamicOutputs) {
					stream << "dynamic_output " << nodeIndex << ' ';
					WriteQuoted(stream, node.Id);
					stream << ' ';
					WriteQuoted(stream, output.Id);
					stream << ' ' << TypeName(output.Type) << '\n';
				}
			if (document.FormatVersion >= 2) {
				for (const DynamicInput &input : node.DynamicInputs) {
					if (!input.SourceInputId.empty() &&
						(document.FormatVersion < 9 || !detail::SourceInputOrdinal(input.SourceInputId)))
						return {};
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
					if (!input.SourceInputId.empty()) {
						stream << "source_input_origin " << nodeIndex << ' ';
						WriteQuoted(stream, node.Id);
						stream << ' ';
						WriteQuoted(stream, input.Id);
						stream << ' ';
						WriteQuoted(stream, input.SourceInputId);
						stream << '\n';
					}
					if (document.FormatVersion >= 9 && !input.SourceLayerName.empty()) {
						stream << "dynamic_layer " << nodeIndex << ' ';
						WriteQuoted(stream, node.Id);
						stream << ' ';
						WriteQuoted(stream, input.Id);
						stream << ' ';
						WriteQuoted(stream, input.SourceLayerName);
						stream << '\n';
					}
				}
			}
			if (document.FormatVersion >= 9)
				for (const SourceInputExpression &expression : node.SourceInputExpressions) {
					stream << "node_expression " << nodeIndex << ' ';
					WriteQuoted(stream, node.Id);
					stream << ' ';
					WriteQuoted(stream, expression.Port);
					stream << ' ' << (expression.Enabled ? 1 : 0) << ' ';
					WriteQuoted(stream, expression.Code);
					stream << '\n';
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
		for (const auto &node : document.Nodes)
			if (node.SourceVec2Defaults)
				for (const auto &input : node.SourceVec2Defaults->Inputs) {
					stream << "source_vec2_default ";
					WriteQuoted(stream, node.Id);
					stream << ' ';
					WriteQuoted(stream, input.Port);
					stream << ' ' << std::setprecision(17) << input.Data.X << ' ' << input.Data.Y << '\n';
				}
		for (size_t groupIndex = 0; groupIndex < document.Groups.size(); ++groupIndex) {
			const Group &group = document.Groups[groupIndex];
			stream << "group ";
			WriteQuoted(stream, group.Id);
			stream << ' ';
			WriteQuoted(stream, group.Name);
			if (document.FormatVersion >= 2) {
				stream << ' ';
				WriteQuoted(stream, group.ParentId);
			}
			stream << '\n';
			if (document.FormatVersion >= 11) {
				stream << "group_source_position " << groupIndex << ' ';
				WriteQuoted(stream, group.Id);
				stream << ' ' << std::setprecision(17) << group.SourcePosition.X << ' '
					   << group.SourcePosition.Y << '\n';
			}
			if (!group.SourceInternalName.empty()) {
				if (document.FormatVersion < 11) return {};
				stream << "group_source_internal_name " << groupIndex << ' ';
				WriteQuoted(stream, group.Id);
				stream << ' ';
				WriteQuoted(stream, group.SourceInternalName);
				stream << '\n';
			}

			if (document.FormatVersion >= 9 && !group.OwnerNodeId.empty()) {
				stream << "group_owner ";
				WriteQuoted(stream, group.Id);
				stream << ' ';
				WriteQuoted(stream, group.OwnerNodeId);
				stream << '\n';
			}
			if (!group.PureFunction) {
				stream << "group_pure ";
				WriteQuoted(stream, group.Id);
				stream << " 0\n";
			}
			if (!group.RenderActive) {
				stream << "group_render ";
				WriteQuoted(stream, group.Id);
				stream << " 0\n";
			}
			if (document.FormatVersion >= 9 && !group.InstanceBase.empty()) {
				stream << "group_instance ";
				WriteQuoted(stream, group.Id);
				stream << ' ';
				WriteQuoted(stream, group.InstanceBase);
				stream << '\n';
			}
			if (document.FormatVersion >= 9 && group.ColorDepth != 1) {
				stream << "group_depth ";
				WriteQuoted(stream, group.Id);
				stream << ' '
					   << (group.ColorDepth >= 0 && group.ColorDepth < 9 ? DEPTH_NAMES[group.ColorDepth]
																		 : "invalid")
					   << '\n';
			}
			if (document.FormatVersion >= 2) {
				if (document.FormatVersion >= 9 && (group.Interpolation != 0 || group.Oversample != 0)) {
					stream << "group_sampling ";
					WriteQuoted(stream, group.Id);
					stream << ' ' << group.Interpolation << ' ' << group.Oversample << '\n';
				}
				for (const GroupPort &port : group.Ports) {
					stream << "group_port ";
					WriteQuoted(stream, group.Id);
					stream << ' ';
					WriteQuoted(stream, port.Id);
					stream << ' ';
					WriteQuoted(stream, port.JunctionId);
					stream << ' ' << DirectionName(port.Direction) << '\n';
					if (document.FormatVersion >= 9 && !port.ControlNodeId.empty()) {
						stream << "group_boundary ";
						WriteQuoted(stream, group.Id);
						stream << ' ';
						WriteQuoted(stream, port.Id);
						stream << ' ';
						WriteQuoted(stream, port.ControlNodeId);
						stream << '\n';
					}
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

		if (!detail::WriteKeyframeText(stream, document.Keyframes, document.FormatVersion)) return {};
		size_t aggregateKeys = document.Keyframes.size();
		Diagnostic axesDiagnostic;
		for (const auto &node : document.Nodes) {
			if (!node.SourceSeparatedVec2Animators) continue;
			if (document.FormatVersion < 9 ||
				detail::ValidateSeparatedVec2(node, aggregateKeys, axesDiagnostic) != Status::Ok)
				return {};
			for (const auto &input : node.SourceSeparatedVec2Animators->Inputs) {
				if (!input.Initialized) {
					stream << "source_vec2_axis_cold ";
					WriteQuoted(stream, node.Id);
					stream << ' ';
					WriteQuoted(stream, input.Port);
					stream << ' ' << input.Separated << '\n';
					continue;
				}
				for (size_t axis = 0; axis < 2; ++axis) {
					stream << "source_vec2_axis ";
					WriteQuoted(stream, node.Id);
					stream << ' ';
					WriteQuoted(stream, input.Port);
					stream << ' ' << (axis == 0 ? "x" : "y");
					if (!input.Separated) stream << " 0";
					stream << '\n';
					if (!detail::WriteKeyframeText(stream, input.Axes[axis].Keys, document.FormatVersion))
						return {};
					stream << "source_vec2_axis_end\n";
				}
			}
		}

		for (const auto &owner : document.SourceCommonOwners) {
			stream << "source_common_owner ";
			for (const auto *name :
				 {&owner.SourceOwnerId,
				  &owner.SourceType,
				  &owner.NativeOwnerId,
				  &owner.InstanceBase,
				  &owner.UpdateAnimatorOwnerId,
				  &owner.UpdateAnimatorPort}) {
				WriteQuoted(stream, *name);
				stream << ' ';
			}
			stream << (owner.NativeOwnerKind == SourceCommonNativeOwnerKind::Node ? "node" : "group") << ' '
				   << owner.Active << ' ' << owner.ShowUpdateTrigger << ' ' << owner.OutMeta << ' '
				   << owner.DisplayNamePresent << ' ' << owner.UpdateOverrideInstance << ' '
				   << owner.UpdateGraph << '\n';
			if (owner.UpdateExpression) {
				stream << "source_common_expression ";
				WriteQuoted(stream, owner.SourceOwnerId);
				stream << ' ';
				WriteQuoted(stream, owner.UpdateExpression->Port);
				stream << ' ';
				WriteQuoted(stream, owner.UpdateExpression->Code);
				stream << ' ' << owner.UpdateExpression->Enabled << '\n';
			}
		}
		if (document.SourceAnimators) {
			stream << "source_animators\n";
			const auto mode = [](GroupSubtypeAnimator value) {
				return value == GroupSubtypeAnimator::Static ? "static" : "animated";
			};
			const auto quoted = [&](std::string_view value) {
				WriteQuoted(stream, std::string(value));
				stream << ' ';
			};
			for (const auto &binding : document.SourceAnimators->Bindings) {
				stream << "source_binding ";
				quoted(binding.NodeId);
				quoted(binding.OwnerId);
				quoted(binding.Port);
				quoted(binding.AnimatorPort);
				stream << mode(binding.Getter) << ' ' << mode(binding.Writer) << ' ';
				constexpr std::array storageNames{"none", "cold", "local", "shared"};
				stream << storageNames[static_cast<size_t>(binding.Axes.Storage)] << ' ';
				quoted(binding.Axes.OwnerId);
				quoted(binding.Axes.Port);
				quoted(binding.Axes.InstanceBase);
				stream << mode(binding.Axes.Writer) << '\n';
			}
			for (const auto &metadata : document.SourceAnimators->Detached) {
				stream << "source_detached ";
				quoted(metadata.OwnerId);
				quoted(metadata.Id);
				quoted(metadata.OriginalPort);
				stream << mode(metadata.Writer) << ' ' << TypeName(metadata.Type) << ' ';
				stream << (!metadata.ArrayClassification   ? "unspecified"
						   : *metadata.ArrayClassification ? "array"
														   : "scalar")
					   << '\n';
				if (metadata.Track) {
					stream << "source_detached_track ";
					quoted(metadata.OwnerId);
					quoted(metadata.Id);
					quoted(metadata.Track->NodeId);
					quoted(metadata.Track->Port);
					quoted(metadata.Track->End);
					stream << metadata.Track->LoopRange << ' ' << metadata.Track->QuaternionMode.value_or(-1)
						   << '\n';
				}
			}
			for (const auto &payload : document.SourceAnimators->DetachedValues) {
				if (payload.Fixed) {
					stream << "source_detached_value ";
					quoted(payload.NodeId);
					quoted(payload.Port);
					WriteValue(stream, *payload.Fixed);
					stream << '\n';
				}
				if (!payload.Keys.empty()) {
					stream << "source_detached_keys ";
					quoted(payload.NodeId);
					quoted(payload.Port);
					stream << '\n';
					if (!detail::WriteKeyframeText(stream, payload.Keys, document.FormatVersion)) return {};
					stream << "source_animator_keys_end\n";
				}
				if (payload.SeparatedVec2) {
					const auto &axes = *payload.SeparatedVec2;
					if (!axes.Initialized) {
						stream << "source_detached_axes_cold ";
						quoted(payload.NodeId);
						quoted(payload.Port);
						stream << axes.Separated << '\n';
					} else
						for (size_t axis = 0; axis < 2; ++axis) {
							stream << "source_detached_axis ";
							quoted(payload.NodeId);
							quoted(payload.Port);
							stream << (axis ? "y" : "x") << ' ' << axes.Separated << '\n';
							if (!detail::WriteKeyframeText(
									stream, axes.Axes[axis].Keys, document.FormatVersion
								))
								return {};
							stream << "source_animator_keys_end\n";
						}
				}
			}
		}

		if (!document.ProjectGlobalNodeId.empty()) {
			stream << "project_global_node ";
			WriteQuoted(stream, document.ProjectGlobalNodeId);
			stream << '\n';
		}
		if (document.FormatVersion >= 7 && document.Project) {
			const ProjectSettings &project = *document.Project;
			stream << "project " << project.SurfaceWidth << ' ' << project.SurfaceHeight << ' '
				   << project.Interpolation << ' ' << project.Oversample << ' ' << project.Palette.size();
			for (const Colour &colour : project.Palette)
				stream << ' ' << unsigned(colour.Red) << ' ' << unsigned(colour.Green) << ' '
					   << unsigned(colour.Blue) << ' ' << unsigned(colour.Alpha);
			stream << '\n';
			if (document.FormatVersion >= 9) {
				if (project.Shader3D != 0)
					stream << "project_shader " << (project.Shader3D == 1 ? "pbr" : "invalid") << '\n';
				if (project.ColorDepth != 1)
					stream << "project_depth "
						   << (project.ColorDepth >= 0 && project.ColorDepth <= 6
								   ? DEPTH_NAMES[project.ColorDepth + 2]
								   : "invalid")
						   << '\n';
				stream << "preview_grid " << project.PreviewGrid.Show << ' ' << project.PreviewGrid.Snap
					   << ' ' << project.PreviewGrid.Size.X << ' ' << project.PreviewGrid.Size.Y << '\n';
				stream << "preview_rulers " << project.ShowPreviewRulers << ' '
					   << project.PreviewRulers.size();
				for (const auto &guide : project.PreviewRulers)
					stream << ' '
						   << (guide.Axis == PreviewRulerAxis::Horizontal ? "horizontal"
							   : guide.Axis == PreviewRulerAxis::Vertical ? "vertical"
																		  : "invalid")
						   << ' ' << guide.Position;
				stream << '\n';
				if (!project.AnimationRegions.empty()) {
					stream << "project_regions " << project.AnimationRegions.size() << '\n';
					for (const AnimationRegion &region : project.AnimationRegions) {
						stream << "animation_region ";
						WriteQuoted(stream, region.Label);
						stream << ' ' << region.Start.Tick << ' '
							   << (region.Start.NegativeFrame ? "negative" : "positive") << ' '
							   << std::setprecision(17) << region.Start.Subframe << ' ' << region.End.Tick
							   << ' ' << (region.End.NegativeFrame ? "negative" : "positive") << ' '
							   << region.End.Subframe << ' ';
						WriteValue(stream, Value{region.Color});
						stream << '\n';
					}
					for (size_t index = 0; index < project.AnimationRegions.size(); ++index) {
						const auto &origin = project.AnimationRegions[index].SourceRegionId;
						if (origin.empty()) continue;
						stream << "source_region_origin " << index << ' ';
						WriteQuoted(stream, origin);
						stream << '\n';
					}
				}
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
				if (document.Timeline->SourceBounds) {
					const auto writeBound = [&](const SourceAuthoringFrameBound &bound) {
						if (bound.Presence == SourceFrameBoundPresence::Missing)
							stream << "missing";
						else if (bound.Presence == SourceFrameBoundPresence::Null)
							stream << "null";
						else
							stream << "explicit " << bound.Value.Tick << ' ' << std::setprecision(17)
								   << bound.Value.Subframe << ' ' << bound.Value.NegativeFrame;
					};
					stream << "source_timeline_bounds ";
					writeBound(document.Timeline->SourceBounds->Start);
					stream << ' ';
					writeBound(document.Timeline->SourceBounds->End);
					stream << '\n';
				}
			}
			for (const AnimationTrack &track : document.Tracks) {
				stream << "track ";
				WriteQuoted(stream, track.NodeId);
				stream << ' ';
				WriteQuoted(stream, track.Port);
				stream << ' ';
				WriteQuoted(stream, track.End);
				stream << ' ' << track.LoopRange << '\n';
				if (document.FormatVersion >= 8 && track.QuaternionMode) {
					stream << "track_quaternion ";
					WriteQuoted(stream, track.NodeId);
					stream << ' ';
					WriteQuoted(stream, track.Port);
					stream << ' ';
					WriteQuoted(stream, *track.QuaternionMode == 0 ? "raw" : "euler");
					stream << '\n';
				}
			}
		}
		return stream ? stream.str() : std::string{};
	}

	static Status ReadDocumentText(
		const std::string &text,
		Document &document,
		Diagnostic &diagnostic,
		detail::EvaluationBudget *budget,
		detail::AllocationReservation *candidateCharge
	) {
		if (text.size() > Limits::MaximumDocumentBytes) {
			SetDiagnostic(diagnostic, Status::LimitExceeded, "document text exceeds the byte limit");
			return diagnostic.Code;
		}
		bool allocationRefused = false;
		size_t cursor = 0;
		std::string line;
		detail::AllocationReservation lineCharge;
		const auto nextLine = [&]() {
			if (cursor == text.size()) return false;
			const size_t end = text.find('\n', cursor);
			const size_t length = (end == std::string::npos ? text.size() : end) - cursor;
			if (budget) {
				auto replacementCharge = budget->Reserve(length);
				if (!replacementCharge) {
					allocationRefused = true;
					return false;
				}
				std::string replacement(length, '\0');
				if (!replacementCharge->Resize(OwnedTextCapacity(replacement))) {
					allocationRefused = true;
					return false;
				}
				replacement.assign(text.data() + cursor, length);
				line = std::move(replacement);
				lineCharge = std::move(*replacementCharge);
			} else {
				line.assign(text.data() + cursor, length);
			}
			cursor = end == std::string::npos ? text.size() : end + 1;
			return true;
		};
		if (!nextLine()) {
			SetDiagnostic(
				diagnostic,
				allocationRefused ? Status::LimitExceeded : Status::Malformed,
				allocationRefused ? "document line exceeds the byte limit" : "missing imagegraph header"
			);
			return diagnostic.Code;
		}
		auto headerCharge =
			budget ? budget->Reserve(line.size()) : std::optional<detail::AllocationReservation>{};
		if (budget && !headerCharge) {
			SetDiagnostic(diagnostic, Status::LimitExceeded, "document header exceeds the byte limit");
			return diagnostic.Code;
		}
		TokenCharge headerTokenCharge;
		std::string headerText;
		if (budget) {
			headerText = std::string(line.size(), '\0');
			if (!headerCharge->Resize(OwnedTextCapacity(headerText))) {
				SetDiagnostic(diagnostic, Status::LimitExceeded, "document header exceeds the byte limit");
				return diagnostic.Code;
			}
			headerText.assign(line);
		} else
			headerText = line;
		std::istringstream header(std::move(headerText));
		std::string headerMarker;
		detail::AllocationReservation rowQuoteCharge;
		auto persistedTextCharge =
			budget ? budget->Reserve(0) : std::optional<detail::AllocationReservation>{};
		Document parsed;
		const auto reserveSlots = [&](auto &items, size_t required) {
			if (!budget || required <= items.capacity()) return true;
			using Item = typename std::remove_reference_t<decltype(items)>::value_type;
			static_assert(std::is_nothrow_move_constructible_v<Item>);
			if (required > items.max_size()) return false;
			const size_t oldCapacity = items.capacity();
			const size_t doubled = oldCapacity <= items.max_size() / 2 ? oldCapacity * 2 : items.max_size();
			const size_t newCapacity = std::max(required, doubled);
			if (newCapacity > UINT64_MAX / sizeof(Item)) return false;
			auto replacement = budget->Reserve(uint64_t(newCapacity) * sizeof(Item));
			if (!replacement) return false;
			items.reserve(newCapacity);
			if (items.capacity() > UINT64_MAX / sizeof(Item) ||
				!replacement->Resize(uint64_t(items.capacity()) * sizeof(Item)))
				return false;
			const uint64_t oldBytes = uint64_t(oldCapacity) * sizeof(Item);
			if (oldBytes > candidateCharge->Bytes() ||
				!candidateCharge->Resize(candidateCharge->Bytes() - oldBytes))
				std::terminate();
			if (!candidateCharge->Merge(std::move(*replacement))) std::terminate();
			return true;
		};
		const auto readQuoted = [&](std::istream &stream,
									std::string &destination,
									size_t maximum = std::numeric_limits<size_t>::max()) {
			if (!budget) return ReadQuoted(stream, destination, maximum);
			stream >> std::ws;
			const auto start = stream.tellg();
			if (start == std::istream::pos_type(-1) || stream.get() != '"') return false;
			size_t length = 0;
			char character = 0;
			bool closed = false;
			while (stream.get(character)) {
				if (character == '"') {
					closed = true;
					break;
				}
				if (character == '\\') {
					if (!stream.get(character) || (character != '\\' && character != '"' &&
												   character != 'n' && character != 'r' && character != 't'))
						return false;
				}
				if (length == maximum) return false;
				++length;
			}
			if (!closed) return false;
			stream.seekg(start);
			if (length > destination.capacity()) {
				auto storage = budget->Reserve(length);
				if (!storage) {
					allocationRefused = true;
					return false;
				}
				std::string sized(length, '\0');
				if (!storage->Resize(sized.capacity())) {
					allocationRefused = true;
					return false;
				}
				if (!rowQuoteCharge.Merge(std::move(*storage))) std::terminate();
				destination = std::move(sized);
				destination.clear();
			}
			return ReadQuoted(stream, destination, maximum);
		};
		if (!(header >> BoundedToken{headerMarker, budget, headerTokenCharge, &allocationRefused} >>
			  parsed.FormatVersion) ||
			headerMarker != "imagegraph" || HasTrailing(header)) {
			SetDiagnostic(
				diagnostic,
				allocationRefused ? Status::LimitExceeded : Status::Malformed,
				allocationRefused ? "document header token exceeds the byte limit"
								  : "invalid imagegraph header"
			);
			return diagnostic.Code;
		}
		if (parsed.FormatVersion < 1 || parsed.FormatVersion > 11) {
			SetDiagnostic(diagnostic, Status::UnsupportedVersion, "unsupported imagegraph document version");
			return diagnostic.Code;
		}

		std::optional<PreviewGridSettings> previewGrid;
		std::optional<int64_t> projectDepth;
		std::optional<int64_t> projectShader;
		detail::EvaluationBudget unboundedSetBudget(UINT64_MAX);
		auto &setBudget = budget ? *budget : unboundedSetBudget;
		auto setStringCharge = budget ? budget->Reserve(0) : std::optional<detail::AllocationReservation>{};
		auto groupDepths = detail::MakeEvaluationSet<std::string>(setBudget);
		auto groupRenderFlags = detail::MakeEvaluationSet<std::string>(setBudget);
		auto groupPureFlags = detail::MakeEvaluationSet<std::string>(setBudget);
		auto groupSampling = detail::MakeEvaluationSet<std::string>(setBudget);
		auto groupSourcePositions = detail::MakeEvaluationSet<std::string>(setBudget);
		auto keyTimes = detail::MakeEvaluationSet<size_t>(setBudget);
		auto keyKinds = detail::MakeEvaluationSet<size_t>(setBudget);
		auto keySourceIds = detail::MakeEvaluationSet<size_t>(setBudget);
		std::optional<std::pair<bool, std::vector<PreviewRulerGuide>>> previewRulers;
		std::optional<size_t> projectRegionCount;
		size_t projectRegionTextBytes = 0;
		std::vector<Keyframe> *axisKeys = nullptr;
		std::string axisNode, axisPort;
		auto axesSeen =
			detail::MakeEvaluationSet<std::tuple<std::string, std::string, std::string>>(setBudget);
		const auto insertString = [&](auto &items, const std::string &key) {
			auto keyCharge =
				budget ? budget->Reserve(key.size()) : std::optional<detail::AllocationReservation>{};
			if (budget && !keyCharge) {
				allocationRefused = true;
				return false;
			}
			std::string copied;
			if (budget) {
				copied = std::string(key.size(), '\0');
				if (!keyCharge->Resize(OwnedTextCapacity(copied))) {
					allocationRefused = true;
					return false;
				}
				copied.assign(key);
			} else
				copied = key;
			const bool inserted = items.insert(std::move(copied)).second;
			if (inserted && budget && !setStringCharge->Merge(std::move(*keyCharge))) std::terminate();
			return inserted;
		};
		const auto insertAxis = [&](const std::string &node,
									const std::string &port,
									const std::string &axis) {
			if (node.size() > UINT64_MAX - port.size() ||
				node.size() + port.size() > UINT64_MAX - axis.size()) {
				allocationRefused = true;
				return false;
			}
			auto keyCharge = budget ? budget->Reserve(node.size() + port.size() + axis.size())
									: std::optional<detail::AllocationReservation>{};
			if (budget && !keyCharge) {
				allocationRefused = true;
				return false;
			}
			std::string nodeCopy, portCopy, axisCopy;
			if (budget) {
				nodeCopy = std::string(node.size(), '\0');
				portCopy = std::string(port.size(), '\0');
				axisCopy = std::string(axis.size(), '\0');
				const uint64_t nodeBytes = OwnedTextCapacity(nodeCopy);
				const uint64_t portBytes = OwnedTextCapacity(portCopy);
				const uint64_t axisBytes = OwnedTextCapacity(axisCopy);
				if (nodeBytes > UINT64_MAX - portBytes || nodeBytes + portBytes > UINT64_MAX - axisBytes ||
					!keyCharge->Resize(nodeBytes + portBytes + axisBytes)) {
					allocationRefused = true;
					return false;
				}
				nodeCopy.assign(node);
				portCopy.assign(port);
				axisCopy.assign(axis);
			} else {
				nodeCopy = node;
				portCopy = port;
				axisCopy = axis;
			}
			const bool inserted =
				axesSeen.emplace(std::move(nodeCopy), std::move(portCopy), std::move(axisCopy)).second;
			if (inserted && budget && !setStringCharge->Merge(std::move(*keyCharge))) std::terminate();
			return inserted;
		};
		std::array<const std::string *, 32> rowPersistentStrings{};
		size_t rowPersistentCount = 0;
		uint64_t rowReleasedTextBytes = 0;
		const auto ownedTextBytes = [](const std::string &value) -> uint64_t {
			return OwnedTextCapacity(value);
		};
		const auto remember = [&](const std::string &value) {
			if (rowPersistentCount == rowPersistentStrings.size()) std::terminate();
			rowPersistentStrings[rowPersistentCount++] = &value;
		};
		const auto release = [&](const std::string &value) {
			if (ownedTextBytes(value) > UINT64_MAX - rowReleasedTextBytes) std::terminate();
			rowReleasedTextBytes += ownedTextBytes(value);
		};
		const auto settleQuotedText = [&]() {
			if (!budget) {
				rowPersistentCount = 0;
				rowReleasedTextBytes = 0;
				return true;
			}
			uint64_t added = 0;
			for (size_t index = 0; index < rowPersistentCount; ++index) {
				const uint64_t amount = ownedTextBytes(*rowPersistentStrings[index]);
				if (amount > UINT64_MAX - added) return false;
				added += amount;
			}
			if (added > rowQuoteCharge.Bytes() || rowReleasedTextBytes > persistedTextCharge->Bytes())
				return false;
			if (!persistedTextCharge->Resize(persistedTextCharge->Bytes() - rowReleasedTextBytes))
				return false;
			if (added != 0) {
				auto transferred = rowQuoteCharge.Split(added);
				if (!transferred || !persistedTextCharge->Merge(std::move(*transferred))) return false;
			}
			rowQuoteCharge.Reset();
			rowPersistentCount = 0;
			rowReleasedTextBytes = 0;
			return true;
		};
		bool detachedKeyBlock = false;
		uint64_t sourceAnimatorLookupWork = 0;
		size_t totalKeys = 0;
		const auto keyframes = [&]() -> std::vector<Keyframe> & {
			return axisKeys ? *axisKeys : parsed.Keyframes;
		};
		size_t lineNumber = 1;
		TokenCharge rowTokenCharge;
		const auto token = [&](std::string &value) {
			return BoundedToken{value, budget, rowTokenCharge, &allocationRefused};
		};
		while (true) {
			rowTokenCharge.Reset();
			if (!nextLine()) break;
			lineNumber++;
			if (line.empty()) continue;
			auto rowCharge =
				budget ? budget->Reserve(line.size()) : std::optional<detail::AllocationReservation>{};
			if (budget && !rowCharge) goto limited;
			std::string rowText;
			if (budget) {
				rowText = std::string(line.size(), '\0');
				if (!rowCharge->Resize(OwnedTextCapacity(rowText))) goto limited;
				rowText.assign(line);
			} else
				rowText = line;
			std::istringstream row(std::move(rowText));
			row.imbue(std::locale::classic());
			std::string marker;
			if (!(row >> token(marker))) continue;
			if (axisKeys && marker != "source_animator_keys_end" && marker != "source_vec2_axis_end" &&
				marker != "keyframe" && marker != "key_source_id" && marker != "key_time" &&
				marker != "key_kind" && marker != "key_ease" && marker != "key_driver" &&
				marker != "key_source_driver")
				goto malformed;
			if (marker == "node") {
				Node node;
				if (!readQuoted(row, node.Id) || !readQuoted(row, node.Type) ||
					!readQuoted(row, node.GroupId) || !(row >> node.Position.X >> node.Position.Y) ||
					!std::isfinite(node.Position.X) || !std::isfinite(node.Position.Y) || HasTrailing(row))
					goto malformed;
				if (parsed.Nodes.size() == Limits::MaximumNodes) {
					SetDiagnostic(
						diagnostic, Status::LimitExceeded, "document exceeds the node limit", node.Id
					);
					return diagnostic.Code;
				}
				if (!reserveSlots(parsed.Nodes, parsed.Nodes.size() + 1)) goto limited;
				parsed.Nodes.push_back(std::move(node));
				remember(parsed.Nodes.back().Id);
				remember(parsed.Nodes.back().Type);
				remember(parsed.Nodes.back().GroupId);
			} else if (marker == "node_name" || marker == "node_internal_name") {
				size_t index = 0;
				std::string id, name;
				if (parsed.FormatVersion < 9 || !(row >> index) ||
					!readQuoted(row, id, Limits::MaximumTextBytes) ||
					!readQuoted(row, name, Limits::MaximumTextBytes) || name.empty() || HasTrailing(row) ||
					index >= parsed.Nodes.size() || parsed.Nodes[index].Id != id ||
					!(marker == "node_name" ? parsed.Nodes[index].SourceDisplayName
											: parsed.Nodes[index].SourceInternalName)
						 .empty())
					goto malformed;
				(marker == "node_name" ? parsed.Nodes[index].SourceDisplayName
									   : parsed.Nodes[index].SourceInternalName) = std::move(name);
				remember(
					marker == "node_name" ? parsed.Nodes[index].SourceDisplayName
										  : parsed.Nodes[index].SourceInternalName
				);
			} else if (marker == "native_sampler" && parsed.FormatVersion >= 9) {
				size_t nodeIndex = 0;
				if (!(row >> nodeIndex) || nodeIndex >= parsed.Nodes.size()) goto malformed;
				auto &node = parsed.Nodes[nodeIndex];
				if (node.Type != "pc.hlsl") goto malformed;
				if (node.NativeSamplerBindings.size() >= Limits::MaximumNativeSamplerBindingsPerNode) {
					SetDiagnostic(
						diagnostic,
						Status::LimitExceeded,
						"native sampler binding count exceeds its limit",
						node.Id
					);
					return diagnostic.Code;
				}
				std::string nodeId;
				if (!readQuoted(row, nodeId, Limits::MaximumTextBytes) || node.Id != nodeId) goto malformed;
				NativeSamplerBinding binding;
				if (!readQuoted(row, binding.Argument, Limits::MaximumNativeSamplerArgumentBytes))
					goto malformed;
				if (std::any_of(
						node.NativeSamplerBindings.begin(),
						node.NativeSamplerBindings.end(),
						[&](const auto &existing) { return existing.Argument == binding.Argument; }
					)) {
					SetDiagnostic(
						diagnostic,
						Status::DuplicateId,
						"native sampler argument binding repeats",
						nodeId,
						binding.Argument
					);
					return diagnostic.Code;
				}
				if (!readQuoted(row, binding.Texture, Limits::MaximumNativeSamplerTextureBytes) ||
					HasTrailing(row))
					goto malformed;
				if (!reserveSlots(node.NativeSamplerBindings, node.NativeSamplerBindings.size() + 1))
					goto limited;
				node.NativeSamplerBindings.push_back(std::move(binding));
				remember(node.NativeSamplerBindings.back().Argument);
				remember(node.NativeSamplerBindings.back().Texture);
				if (const auto fault = detail::ValidateNativeSamplerBindings(node, parsed.FormatVersion)) {
					SetDiagnostic(
						diagnostic,
						fault->Code,
						std::string(fault->Message),
						nodeId,
						std::string(fault->Argument)
					);
					return diagnostic.Code;
				}
			} else if (marker == "value" || (marker == "source_property" && parsed.FormatVersion >= 9)) {
				size_t nodeIndex = 0;
				std::string nodeId, port;
				Value value;
				if (!(row >> nodeIndex) || !readQuoted(row, nodeId) || !readQuoted(row, port) ||
					!ReadValue(
						row, value, parsed.FormatVersion, true, budget, candidateCharge, &allocationRefused
					) ||
					HasTrailing(row))
					goto malformed;
				if (nodeIndex >= parsed.Nodes.size() || parsed.Nodes[nodeIndex].Id != nodeId) {
					SetDiagnostic(diagnostic, Status::Malformed, "value precedes its node", nodeId, port);
					return diagnostic.Code;
				}
				Node &node = parsed.Nodes[nodeIndex];
				auto &properties = marker == "value" ? node.Values : node.SourceProperties;
				if (properties.size() == Limits::MaximumPropertiesPerNode) {
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
				if (!reserveSlots(properties, properties.size() + 1)) goto limited;
				properties.push_back({std::move(port), std::move(value)});
				remember(properties.back().Port);
			} else if (marker == "dynamic" && parsed.FormatVersion >= 2) {
				size_t nodeIndex = 0;
				std::string nodeId, inputId, typeName;
				int hasDefault = 0;
				if (!(row >> nodeIndex) || !readQuoted(row, nodeId) || !readQuoted(row, inputId) ||
					!(row >> token(typeName) >> hasDefault) || (hasDefault != 0 && hasDefault != 1))
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
					if (!ReadValue(
							row,
							value,
							parsed.FormatVersion,
							true,
							budget,
							candidateCharge,
							&allocationRefused
						))
						goto malformed;
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
				if (parsed.Nodes[nodeIndex].DynamicInputs.size() ==
					MaximumDynamicInputsForType(parsed.Nodes[nodeIndex].Type)) {
					SetDiagnostic(
						diagnostic,
						Status::LimitExceeded,
						"node exceeds the dynamic input limit",
						nodeId,
						dynamic.Id
					);
					return diagnostic.Code;
				}
				if (!reserveSlots(
						parsed.Nodes[nodeIndex].DynamicInputs,
						parsed.Nodes[nodeIndex].DynamicInputs.size() + 1
					))
					goto limited;
				parsed.Nodes[nodeIndex].DynamicInputs.push_back(std::move(dynamic));
				remember(parsed.Nodes[nodeIndex].DynamicInputs.back().Id);
			} else if (marker == "source_input_origin" && parsed.FormatVersion >= 9) {
				size_t nodeIndex = 0;
				std::string nodeId, inputId, origin;
				if (!(row >> nodeIndex) || !readQuoted(row, nodeId) || !readQuoted(row, inputId) ||
					!readQuoted(row, origin, Limits::MaximumSourceInputIdBytes) ||
					!detail::SourceInputOrdinal(origin) || HasTrailing(row) ||
					nodeIndex >= parsed.Nodes.size() || parsed.Nodes[nodeIndex].Id != nodeId)
					goto malformed;
				auto &inputs = parsed.Nodes[nodeIndex].DynamicInputs;
				// Canonical writers put origin metadata directly after its
				// dynamic declaration.
				auto found = inputs.empty() ? inputs.end() : inputs.end() - 1;
				if (found == inputs.end() || found->Id != inputId)
					found = std::find_if(inputs.begin(), inputs.end(), [&](const auto &input) {
						return input.Id == inputId;
					});
				if (found == inputs.end() || !found->SourceInputId.empty()) goto malformed;
				found->SourceInputId = std::move(origin);
				remember(found->SourceInputId);
			} else if (marker == "dynamic_layer" && parsed.FormatVersion >= 9) {
				size_t nodeIndex = 0;
				std::string nodeId, inputId, name;
				if (!(row >> nodeIndex) || !readQuoted(row, nodeId) || !readQuoted(row, inputId) ||
					!readQuoted(row, name) || name.empty() || HasTrailing(row) ||
					nodeIndex >= parsed.Nodes.size() || parsed.Nodes[nodeIndex].Id != nodeId)
					goto malformed;
				auto &inputs = parsed.Nodes[nodeIndex].DynamicInputs;
				auto found = std::find_if(inputs.begin(), inputs.end(), [&](const auto &input) {
					return input.Id == inputId;
				});
				if (found == inputs.end() || !found->SourceLayerName.empty()) goto malformed;
				found->SourceLayerName = std::move(name);
				remember(found->SourceLayerName);
			} else if (marker == "node_expression" && parsed.FormatVersion >= 9) {
				size_t nodeIndex = 0;
				std::string nodeId, port, code;
				int enabled = 0;
				if (!(row >> nodeIndex) || !readQuoted(row, nodeId, Limits::MaximumTextBytes) ||
					!readQuoted(row, port, Limits::MaximumTextBytes) || !(row >> enabled) ||
					(enabled != 0 && enabled != 1) || !readQuoted(row, code, Limits::MaximumTextBytes) ||
					HasTrailing(row) || port.empty() || port.size() > Limits::MaximumTextBytes ||
					code.size() > Limits::MaximumTextBytes || nodeIndex >= parsed.Nodes.size() ||
					parsed.Nodes[nodeIndex].Id != nodeId)
					goto malformed;
				auto &expressions = parsed.Nodes[nodeIndex].SourceInputExpressions;
				if (expressions.size() == Limits::MaximumSourceInputExpressionsPerNode ||
					std::any_of(expressions.begin(), expressions.end(), [&](const auto &entry) {
						return entry.Port == port;
					}))
					goto malformed;
				if (!reserveSlots(expressions, expressions.size() + 1)) goto limited;
				expressions.push_back({std::move(port), std::move(code), enabled != 0});
				remember(expressions.back().Port);
				remember(expressions.back().Code);
			} else if (marker == "dynamic_output" && parsed.FormatVersion >= 9) {
				size_t nodeIndex = 0;
				std::string nodeId, outputId, typeName;
				if (!(row >> nodeIndex) || !readQuoted(row, nodeId) || !readQuoted(row, outputId) ||
					!(row >> token(typeName)) || HasTrailing(row))
					goto malformed;
				const auto type = ParseType(typeName);
				if (!type || nodeIndex >= parsed.Nodes.size() || parsed.Nodes[nodeIndex].Id != nodeId)
					goto malformed;
				if (parsed.Nodes[nodeIndex].DynamicOutputs.size() >= Limits::MaximumDynamicOutputsPerNode) {
					SetDiagnostic(
						diagnostic,
						Status::LimitExceeded,
						"node exceeds the dynamic output limit",
						nodeId,
						outputId
					);
					return diagnostic.Code;
				}
				if (!reserveSlots(
						parsed.Nodes[nodeIndex].DynamicOutputs,
						parsed.Nodes[nodeIndex].DynamicOutputs.size() + 1
					))
					goto limited;
				parsed.Nodes[nodeIndex].DynamicOutputs.push_back({std::move(outputId), *type});
				remember(parsed.Nodes[nodeIndex].DynamicOutputs.back().Id);
			} else if (marker == "link") {
				Link link;
				if (!readQuoted(row, link.FromNode) || !readQuoted(row, link.FromPort) ||
					!readQuoted(row, link.ToNode) || !readQuoted(row, link.ToPort) || HasTrailing(row))
					goto malformed;
				if (parsed.Links.size() == Limits::MaximumLinks) {
					SetDiagnostic(diagnostic, Status::LimitExceeded, "document exceeds the link limit");
					return diagnostic.Code;
				}
				if (!reserveSlots(parsed.Links, parsed.Links.size() + 1)) goto limited;
				parsed.Links.push_back(std::move(link));
				remember(parsed.Links.back().FromNode);
				remember(parsed.Links.back().FromPort);
				remember(parsed.Links.back().ToNode);
				remember(parsed.Links.back().ToPort);
			} else if (marker == "group") {
				Group group;
				if (!readQuoted(row, group.Id) || !readQuoted(row, group.Name) ||
					(parsed.FormatVersion >= 2 && !readQuoted(row, group.ParentId)) || HasTrailing(row))
					goto malformed;
				if (parsed.Groups.size() == Limits::MaximumGroups) {
					SetDiagnostic(
						diagnostic, Status::LimitExceeded, "document exceeds the group limit", group.Id
					);
					return diagnostic.Code;
				}
				if (!reserveSlots(parsed.Groups, parsed.Groups.size() + 1)) goto limited;
				parsed.Groups.push_back(std::move(group));
				remember(parsed.Groups.back().Id);
				remember(parsed.Groups.back().Name);
				remember(parsed.Groups.back().ParentId);
			} else if ((marker == "node_instance_override" || marker == "source_anim" ||
						marker == "source_static") &&
					   parsed.FormatVersion >= 9) {
				std::string nodeId, port;
				if (!readQuoted(row, nodeId) || !readQuoted(row, port) || port.empty() || HasTrailing(row))
					goto malformed;
				auto node = std::find_if(parsed.Nodes.begin(), parsed.Nodes.end(), [&](const Node &n) {
					return n.Id == nodeId;
				});
				if (node == parsed.Nodes.end()) goto malformed;
				auto &ports = marker == "source_anim"	  ? node->SourceAnimatedInputs
							  : marker == "source_static" ? node->SourceStaticInputs
														  : node->InstanceOverrides;
				if (ports.size() == Limits::MaximumArrayElements ||
					std::find(ports.begin(), ports.end(), port) != ports.end())
					goto malformed;
				if (!reserveSlots(ports, ports.size() + 1)) goto limited;
				ports.push_back(std::move(port));
				remember(ports.back());
			} else if (marker == "source_parent_input_base" && parsed.FormatVersion >= 10) {
				std::string id, base;
				if (!readQuoted(row, id) || !readQuoted(row, base) || base.empty() || HasTrailing(row))
					goto malformed;
				const auto found =
					std::find_if(parsed.Nodes.begin(), parsed.Nodes.end(), [&](const Node &node) {
						return node.Id == id;
					});
				if (found == parsed.Nodes.end() || !found->SourceParentInputBase.empty()) goto malformed;
				found->SourceParentInputBase = std::move(base);
				remember(found->SourceParentInputBase);
			} else if ((marker == "node_instance" || marker == "group_instance") &&
					   parsed.FormatVersion >= 9) {
				std::string id, base;
				if (!readQuoted(row, id) || !readQuoted(row, base) || base.empty() || HasTrailing(row))
					goto malformed;
				if (marker == "node_instance") {
					const auto found =
						std::find_if(parsed.Nodes.begin(), parsed.Nodes.end(), [&](const Node &n) {
							return n.Id == id;
						});
					if (found == parsed.Nodes.end() || !found->InstanceBase.empty()) goto malformed;
					found->InstanceBase = std::move(base);
					remember(found->InstanceBase);
				} else {
					const auto found =
						std::find_if(parsed.Groups.begin(), parsed.Groups.end(), [&](const Group &g) {
							return g.Id == id;
						});
					if (found == parsed.Groups.end() || !found->InstanceBase.empty()) goto malformed;
					found->InstanceBase = std::move(base);
					remember(found->InstanceBase);
				}
			} else if ((marker == "group_render" || marker == "group_pure") && parsed.FormatVersion >= 10) {
				std::string id;
				int active = -1;
				if (!readQuoted(row, id) || !(row >> active) || (active != 0 && active != 1) ||
					HasTrailing(row) ||
					!insertString(marker == "group_render" ? groupRenderFlags : groupPureFlags, id))
					goto malformed;
				const auto group =
					std::find_if(parsed.Groups.begin(), parsed.Groups.end(), [&](const Group &candidate) {
						return candidate.Id == id;
					});
				if (group == parsed.Groups.end()) goto malformed;
				if (marker == "group_render")
					group->RenderActive = active != 0;
				else
					group->PureFunction = active != 0;
			} else if (marker == "group_owner" && parsed.FormatVersion >= 9) {
				std::string id, owner;
				if (!readQuoted(row, id) || !readQuoted(row, owner) || owner.empty() || HasTrailing(row))
					goto malformed;
				const auto group =
					std::find_if(parsed.Groups.begin(), parsed.Groups.end(), [&](const Group &candidate) {
						return candidate.Id == id;
					});
				if (group == parsed.Groups.end() || !group->OwnerNodeId.empty()) goto malformed;
				group->OwnerNodeId = std::move(owner);
				remember(group->OwnerNodeId);
			} else if (marker == "group_depth" && parsed.FormatVersion >= 9) {
				std::string id, name;
				if (!readQuoted(row, id) || !(row >> token(name)) || HasTrailing(row) ||
					!insertString(groupDepths, id))
					goto malformed;
				const auto depth = ParseDepth(name);
				const auto group =
					std::find_if(parsed.Groups.begin(), parsed.Groups.end(), [&](const Group &candidate) {
						return candidate.Id == id;
					});
				if (!depth || group == parsed.Groups.end()) goto malformed;
				group->ColorDepth = *depth;
			} else if (marker == "group_port" && parsed.FormatVersion >= 2) {
				std::string groupId, portId, junctionId, direction;
				if (!readQuoted(row, groupId) || !readQuoted(row, portId) || !readQuoted(row, junctionId) ||
					!(row >> token(direction)) || HasTrailing(row) ||
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
				if (!reserveSlots(group->Ports, group->Ports.size() + 1)) goto limited;
				group->Ports.push_back(
					{std::move(portId),
					 std::move(junctionId),
					 direction == "input" ? PortDirection::Input : PortDirection::Output}
				);
				remember(group->Ports.back().Id);
				remember(group->Ports.back().JunctionId);
			} else if (marker == "group_sampling" && parsed.FormatVersion >= 9) {
				std::string groupId;
				int64_t interpolation, oversample;
				if (!readQuoted(row, groupId) || !(row >> interpolation >> oversample) || HasTrailing(row))
					goto malformed;
				auto group = std::find_if(parsed.Groups.begin(), parsed.Groups.end(), [&](const Group &g) {
					return g.Id == groupId;
				});
				if (group == parsed.Groups.end() || !insertString(groupSampling, groupId)) goto malformed;
				group->Interpolation = interpolation;
				group->Oversample = oversample;
			} else if (marker == "group_boundary" && parsed.FormatVersion >= 9) {
				std::string groupId, portId, controlId;
				if (!readQuoted(row, groupId) || !readQuoted(row, portId) || !readQuoted(row, controlId) ||
					controlId.empty() || HasTrailing(row))
					goto malformed;
				const auto group =
					std::find_if(parsed.Groups.begin(), parsed.Groups.end(), [&](const Group &g) {
						return g.Id == groupId;
					});
				if (group == parsed.Groups.end()) goto malformed;
				const auto port =
					std::find_if(group->Ports.begin(), group->Ports.end(), [&](const GroupPort &p) {
						return p.Id == portId;
					});
				if (port == group->Ports.end() || !port->ControlNodeId.empty()) goto malformed;
				port->ControlNodeId = std::move(controlId);
				remember(port->ControlNodeId);
			} else if (marker == "junction" && parsed.FormatVersion >= 2) {
				Junction junction;
				std::string typeName;
				int hasDefault = 0;
				if (!readQuoted(row, junction.Id) || !readQuoted(row, junction.GroupId) ||
					!(row >> token(typeName) >> hasDefault) || (hasDefault != 0 && hasDefault != 1))
					goto malformed;
				const auto type = ParseType(typeName);
				if (!type || (parsed.FormatVersion < 3 && *type >= ValueType::Gradient)) goto malformed;
				junction.Type = *type;
				if (hasDefault) {
					Value value;
					if (!ReadValue(
							row,
							value,
							parsed.FormatVersion,
							true,
							budget,
							candidateCharge,
							&allocationRefused
						))
						goto malformed;
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
				if (!reserveSlots(parsed.Junctions, parsed.Junctions.size() + 1)) goto limited;
				parsed.Junctions.push_back(std::move(junction));
				remember(parsed.Junctions.back().Id);
				remember(parsed.Junctions.back().GroupId);
			} else if (marker == "output") {
				Output output;
				if (!readQuoted(row, output.Id) || !readQuoted(row, output.NodeId) ||
					!readQuoted(row, output.Port) || HasTrailing(row))
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
				if (!reserveSlots(parsed.Outputs, parsed.Outputs.size() + 1)) goto limited;
				parsed.Outputs.push_back(std::move(output));
				remember(parsed.Outputs.back().Id);
				remember(parsed.Outputs.back().NodeId);
				remember(parsed.Outputs.back().Port);
			} else if (marker == "source_vec2_default" && parsed.FormatVersion >= 9) {
				std::string id;
				SourceVec2Default value;
				if (axisKeys || !readQuoted(row, id) || !readQuoted(row, value.Port) ||
					!(row >> value.Data.X >> value.Data.Y) || !std::isfinite(value.Data.X) ||
					!std::isfinite(value.Data.Y) || HasTrailing(row))
					goto malformed;
				const auto node = std::find_if(parsed.Nodes.begin(), parsed.Nodes.end(), [&](const auto &n) {
					return n.Id == id;
				});
				if (node == parsed.Nodes.end() || !detail::SourceSeparatedVec2Input(*node, value.Port))
					goto malformed;
				if (!node->SourceVec2Defaults) {
					if (budget) {
						auto storage = budget->Reserve(sizeof(SourceVec2DefaultsData));
						if (!storage) goto limited;
						if (!candidateCharge->Merge(std::move(*storage))) std::terminate();
					}
					node->SourceVec2Defaults.emplace();
				}
				auto &inputs = node->SourceVec2Defaults->Inputs;
				if (inputs.size() >= Limits::MaximumArrayElements ||
					inputs.size() >= detail::SourceSeparatedVec2InputCount(*node))
					goto limited;
				if (!reserveSlots(inputs, inputs.size() + 1)) goto limited;
				inputs.push_back(std::move(value));
				remember(inputs.back().Port);
			} else if (marker == "source_vec2_axis_cold" && parsed.FormatVersion >= 9) {
				std::string nodeId, port;
				int separated = 0;
				if (axisKeys || !readQuoted(row, nodeId) || !readQuoted(row, port) || !(row >> separated) ||
					(separated != 0 && separated != 1) || HasTrailing(row) ||
					!insertAxis(nodeId, port, "cold"))
					goto malformed;
				const auto node = std::find_if(parsed.Nodes.begin(), parsed.Nodes.end(), [&](const auto &n) {
					return n.Id == nodeId;
				});
				if (node == parsed.Nodes.end() || !detail::SourceSeparatedVec2Input(*node, port))
					goto malformed;
				if (!node->SourceSeparatedVec2Animators) {
					if (budget) {
						auto storage = budget->Reserve(sizeof(SourceSeparatedVec2Data));
						if (!storage) goto limited;
						if (!candidateCharge->Merge(std::move(*storage))) std::terminate();
					}
					node->SourceSeparatedVec2Animators.emplace();
				}
				auto &inputs = node->SourceSeparatedVec2Animators->Inputs;
				if (std::any_of(inputs.begin(), inputs.end(), [&](const auto &input) {
						return input.Port == port;
					}))
					goto malformed;
				if (inputs.size() >= Limits::MaximumArrayElements ||
					inputs.size() >= detail::SourceSeparatedVec2InputCount(*node))
					goto limited;
				if (!reserveSlots(inputs, inputs.size() + 1)) goto limited;
				inputs.push_back({std::move(port), {}, separated != 0, false});
				remember(inputs.back().Port);
			} else if (marker == "source_vec2_axis" && parsed.FormatVersion >= 9) {
				std::string nodeId, port, axis;
				if (axisKeys || !readQuoted(row, nodeId) || !readQuoted(row, port) || !(row >> token(axis)) ||
					(axis != "x" && axis != "y") || !insertAxis(nodeId, port, axis))
					goto malformed;
				int separated = 1;
				row >> std::ws;
				if (!row.eof() && (!(row >> separated) || HasTrailing(row))) goto malformed;
				if (separated != 0 && separated != 1) goto malformed;
				auto found = std::find_if(parsed.Nodes.begin(), parsed.Nodes.end(), [&](const auto &node) {
					return node.Id == nodeId;
				});
				if (found == parsed.Nodes.end() || !detail::SourceSeparatedVec2Input(*found, port))
					goto malformed;
				if (!found->SourceSeparatedVec2Animators) {
					if (budget) {
						auto storage = budget->Reserve(sizeof(SourceSeparatedVec2Data));
						if (!storage) goto limited;
						if (!candidateCharge->Merge(std::move(*storage))) std::terminate();
					}
					found->SourceSeparatedVec2Animators.emplace();
				}
				auto &inputs = found->SourceSeparatedVec2Animators->Inputs;
				auto input = std::find_if(inputs.begin(), inputs.end(), [&](const auto &value) {
					return value.Port == port;
				});
				if (input == inputs.end()) {
					auto copyCharge = budget ? budget->Reserve(port.size())
											 : std::optional<detail::AllocationReservation>{};
					if (budget && !copyCharge) goto limited;
					std::string copiedPort;
					if (budget) {
						copiedPort = std::string(port.size(), '\0');
						if (!copyCharge->Resize(ownedTextBytes(copiedPort))) goto limited;
						copiedPort.assign(port);
					} else
						copiedPort = port;
					if (!reserveSlots(inputs, inputs.size() + 1)) goto limited;
					inputs.push_back({std::move(copiedPort), {}, separated != 0});
					input = inputs.end() - 1;
					if (budget && !rowQuoteCharge.Merge(std::move(*copyCharge))) std::terminate();
					remember(input->Port);
				}
				if (!input->Initialized || input->Separated != (separated != 0)) goto malformed;
				axisKeys = &input->Axes[axis == "x" ? 0 : 1].Keys;
				release(axisNode);
				release(axisPort);
				axisNode = std::move(nodeId);
				axisPort = std::move(port);
				remember(axisNode);
				remember(axisPort);
			} else if (marker == "source_vec2_axis_end" && parsed.FormatVersion >= 9) {
				if (!axisKeys || detachedKeyBlock || HasTrailing(row)) goto malformed;
				axisKeys = nullptr;
			} else if (marker == "source_common_owner" && parsed.FormatVersion >= 11) {
				SourceCommonOwnerRecord owner;
				std::string kind;
				int active = 0, show = 0, meta = 0, named = 0, override = 0, updateGraph = 0;
				if (!readQuoted(row, owner.SourceOwnerId, Limits::MaximumTextBytes) ||
					!readQuoted(row, owner.SourceType, Limits::MaximumTextBytes) ||
					!readQuoted(row, owner.NativeOwnerId, Limits::MaximumTextBytes) ||
					!readQuoted(row, owner.InstanceBase, Limits::MaximumTextBytes) ||
					!readQuoted(row, owner.UpdateAnimatorOwnerId, Limits::MaximumTextBytes) ||
					!readQuoted(row, owner.UpdateAnimatorPort, Limits::MaximumTextBytes) ||
					!(row >> token(kind) >> active >> show >> meta >> named >> override >> updateGraph) ||
					HasTrailing(row) || (kind != "node" && kind != "group") || (active != 0 && active != 1) ||
					(show != 0 && show != 1) || (meta != 0 && meta != 1) || (named != 0 && named != 1) ||
					(override != 0 && override != 1) || (updateGraph != 0 && updateGraph != 1))
					goto malformed;
				owner.NativeOwnerKind =
					kind == "node" ? SourceCommonNativeOwnerKind::Node : SourceCommonNativeOwnerKind::Group;
				owner.Active = active;
				owner.ShowUpdateTrigger = show;
				owner.OutMeta = meta;
				owner.DisplayNamePresent = named;
				owner.UpdateOverrideInstance = override;
				owner.UpdateGraph = updateGraph;
				if (parsed.SourceCommonOwners.size() >= Limits::MaximumSourceCommonOwners ||
					!reserveSlots(parsed.SourceCommonOwners, parsed.SourceCommonOwners.size() + 1))
					goto limited;
				parsed.SourceCommonOwners.push_back(std::move(owner));
				const auto &saved = parsed.SourceCommonOwners.back();
				for (const auto *name :
					 {&saved.SourceOwnerId,
					  &saved.SourceType,
					  &saved.NativeOwnerId,
					  &saved.InstanceBase,
					  &saved.UpdateAnimatorOwnerId,
					  &saved.UpdateAnimatorPort})
					remember(*name);
			} else if (marker == "source_common_expression" && parsed.FormatVersion >= 11) {
				std::string id;
				SourceInputExpression expression;
				int enabled = 0;
				if (!readQuoted(row, id, Limits::MaximumTextBytes) ||
					!readQuoted(row, expression.Port, Limits::MaximumTextBytes) ||
					!readQuoted(row, expression.Code, Limits::MaximumTextBytes) || !(row >> enabled) ||
					(enabled != 0 && enabled != 1) || HasTrailing(row) || parsed.SourceCommonOwners.empty() ||
					parsed.SourceCommonOwners.back().SourceOwnerId != id ||
					parsed.SourceCommonOwners.back().UpdateExpression)
					goto malformed;
				expression.Enabled = enabled;
				parsed.SourceCommonOwners.back().UpdateExpression = std::move(expression);
				remember(parsed.SourceCommonOwners.back().UpdateExpression->Port);
				remember(parsed.SourceCommonOwners.back().UpdateExpression->Code);
			} else if (marker == "group_source_internal_name" && parsed.FormatVersion >= 11) {
				size_t index = 0;
				std::string id, name;
				if (!(row >> index) || !readQuoted(row, id, Limits::MaximumTextBytes) ||
					!readQuoted(row, name, Limits::MaximumTextBytes) || HasTrailing(row) || name.empty() ||
					index >= parsed.Groups.size() || parsed.Groups[index].Id != id ||
					!parsed.Groups[index].SourceInternalName.empty())
					goto malformed;
				parsed.Groups[index].SourceInternalName = std::move(name);
				remember(parsed.Groups[index].SourceInternalName);
			} else if (marker == "group_source_position" && parsed.FormatVersion >= 11) {
				size_t index = 0;
				std::string id;
				Vector2 position;
				if (!(row >> index) || !readQuoted(row, id, Limits::MaximumTextBytes) ||
					!(row >> position.X >> position.Y) || HasTrailing(row) || index >= parsed.Groups.size() ||
					parsed.Groups[index].Id != id || !std::isfinite(position.X) ||
					!std::isfinite(position.Y) || !insertString(groupSourcePositions, id))
					goto malformed;
				parsed.Groups[index].SourcePosition = position;
			} else if (marker == "source_animators" && parsed.FormatVersion >= 10) {
				if (parsed.SourceAnimators || HasTrailing(row)) goto malformed;
				if (budget &&
					!candidateCharge->Resize(candidateCharge->Bytes() + sizeof(SourceAnimatorState)))
					goto limited;
				parsed.SourceAnimators.emplace();
			} else if (marker == "source_binding" && parsed.FormatVersion >= 10) {
				if (!parsed.SourceAnimators) goto malformed;
				GroupSubtypeBinding binding;
				std::string getter, writer, storage, axisWriter;
				if (!readQuoted(row, binding.NodeId) || !readQuoted(row, binding.OwnerId) ||
					!readQuoted(row, binding.Port) || !readQuoted(row, binding.AnimatorPort) ||
					!(row >> token(getter) >> token(writer) >> token(storage)) ||
					!readQuoted(row, binding.Axes.OwnerId) || !readQuoted(row, binding.Axes.Port) ||
					!readQuoted(row, binding.Axes.InstanceBase) || !(row >> token(axisWriter)) ||
					HasTrailing(row))
					goto malformed;
				const auto mode = [](const std::string &name, GroupSubtypeAnimator &value) {
					if (name != "static" && name != "animated") return false;
					value = name == "static" ? GroupSubtypeAnimator::Static : GroupSubtypeAnimator::Animated;
					return true;
				};
				if (!mode(getter, binding.Getter) || !mode(writer, binding.Writer) ||
					!mode(axisWriter, binding.Axes.Writer))
					goto malformed;
				if (storage == "none")
					binding.Axes.Storage = GroupAxisStorage::None;
				else if (storage == "cold")
					binding.Axes.Storage = GroupAxisStorage::Uninitialized;
				else if (storage == "local")
					binding.Axes.Storage = GroupAxisStorage::Local;
				else if (storage == "shared")
					binding.Axes.Storage = GroupAxisStorage::Shared;
				else
					goto malformed;
				auto &bindings = parsed.SourceAnimators->Bindings;
				if (bindings.size() >= Limits::MaximumEvaluationBytes / sizeof(GroupSubtypeBinding) ||
					!reserveSlots(bindings, bindings.size() + 1))
					goto limited;
				bindings.push_back(std::move(binding));
				const auto &saved = bindings.back();
				for (const auto *name :
					 {&saved.NodeId,
					  &saved.OwnerId,
					  &saved.Port,
					  &saved.AnimatorPort,
					  &saved.Axes.OwnerId,
					  &saved.Axes.Port,
					  &saved.Axes.InstanceBase})
					remember(*name);
			} else if (marker == "source_detached" && parsed.FormatVersion >= 10) {
				if (!parsed.SourceAnimators) goto malformed;
				DetachedSourceAnimator metadata;
				std::string writer, type, classification;
				if (!readQuoted(row, metadata.OwnerId) || !readQuoted(row, metadata.Id) ||
					!readQuoted(row, metadata.OriginalPort) ||
					!(row >> token(writer) >> token(type) >> token(classification)) || HasTrailing(row))
					goto malformed;
				if (writer != "static" && writer != "animated") goto malformed;
				metadata.Writer =
					writer == "static" ? GroupSubtypeAnimator::Static : GroupSubtypeAnimator::Animated;
				const auto parsedType = ParseType(type);
				if (!parsedType) goto malformed;
				metadata.Type = *parsedType;
				if (classification == "array")
					metadata.ArrayClassification = true;
				else if (classification == "scalar")
					metadata.ArrayClassification = false;
				else if (classification != "unspecified")
					goto malformed;
				auto &state = *parsed.SourceAnimators;
				if (state.Detached.size() >= Limits::MaximumArrayElements ||
					!reserveSlots(state.Detached, state.Detached.size() + 1) ||
					!reserveSlots(state.DetachedValues, state.DetachedValues.size() + 1))
					goto limited;
				auto copiedNames = budget ? budget->Reserve(
												std::max<size_t>(metadata.OwnerId.size(), 15) +
												std::max<size_t>(metadata.Id.size(), 15)
											)
										  : std::optional<detail::AllocationReservation>{};
				if (budget && !copiedNames) goto limited;
				GroupSubtypeOverlay payload;
				payload.NodeId = metadata.OwnerId;
				payload.Port = metadata.Id;
				if (budget && (!copiedNames->Resize(
								   OwnedTextCapacity(payload.NodeId) + OwnedTextCapacity(payload.Port)
							   ) ||
							   !rowQuoteCharge.Merge(std::move(*copiedNames))))
					goto limited;
				state.Detached.push_back(std::move(metadata));
				state.DetachedValues.push_back(std::move(payload));
				remember(state.Detached.back().OwnerId);
				remember(state.Detached.back().Id);
				remember(state.Detached.back().OriginalPort);
				remember(state.DetachedValues.back().NodeId);
				remember(state.DetachedValues.back().Port);
			} else if ((marker == "source_detached_value" || marker == "source_detached_track" ||
						marker == "source_detached_keys" || marker == "source_detached_axis" ||
						marker == "source_detached_axes_cold") &&
					   parsed.FormatVersion >= 10) {
				if (!parsed.SourceAnimators) goto malformed;
				std::string owner, port;
				if (!readQuoted(row, owner) || !readQuoted(row, port)) goto malformed;
				auto &state = *parsed.SourceAnimators;
				const uint64_t visits = state.Detached.size() * (1 + owner.size() + port.size());
				if (visits > 64'000'000 - sourceAnimatorLookupWork) goto limited;
				sourceAnimatorLookupWork += visits;
				const auto found =
					std::find_if(state.Detached.begin(), state.Detached.end(), [&](const auto &metadata) {
						return metadata.OwnerId == owner && metadata.Id == port;
					});
				if (found == state.Detached.end()) goto malformed;
				auto &payload = state.DetachedValues[size_t(found - state.Detached.begin())];
				if (marker == "source_detached_value") {
					if (payload.Fixed || !payload.Keys.empty()) goto malformed;
					Value fixed;
					if (!ReadValue(
							row,
							fixed,
							parsed.FormatVersion,
							true,
							budget,
							candidateCharge,
							&allocationRefused
						) ||
						HasTrailing(row))
						goto malformed;
					payload.Fixed = std::move(fixed);
				} else if (marker == "source_detached_track") {
					if (found->Track) goto malformed;
					AnimationTrack track;
					int64_t quaternion;
					if (!readQuoted(row, track.NodeId) || !readQuoted(row, track.Port) ||
						!readQuoted(row, track.End) || !(row >> track.LoopRange >> quaternion) ||
						HasTrailing(row) || quaternion < -1 || quaternion > 1)
						goto malformed;
					if (quaternion >= 0) track.QuaternionMode = quaternion;
					found->Track = std::move(track);
					remember(found->Track->NodeId);
					remember(found->Track->Port);
					remember(found->Track->End);
				} else {
					if (marker == "source_detached_keys") {
						if (payload.Fixed || !payload.Keys.empty() || !insertAxis(owner, port, "combined") ||
							HasTrailing(row))
							goto malformed;
						axisKeys = &payload.Keys;
					} else {
						std::string axis;
						int separated;
						if ((marker == "source_detached_axis" && !(row >> token(axis))) ||
							!(row >> separated) || (separated != 0 && separated != 1) || HasTrailing(row))
							goto malformed;
						if (marker == "source_detached_axes_cold") {
							if (payload.SeparatedVec2 || !insertAxis(owner, port, "cold")) goto malformed;
						} else if ((axis != "x" && axis != "y") || !insertAxis(owner, port, axis))
							goto malformed;
						if (!payload.SeparatedVec2) {
							auto names = budget ? budget->Reserve(std::max<size_t>(port.size(), 15))
												: std::optional<detail::AllocationReservation>{};
							if (budget &&
								(!names || !candidateCharge->Resize(
											   candidateCharge->Bytes() + sizeof(SourceSeparatedVec2Animator)
										   )))
								goto limited;
							payload.SeparatedVec2.emplace();
							payload.SeparatedVec2->Port = port;
							payload.SeparatedVec2->Separated = separated;
							payload.SeparatedVec2->Initialized = marker != "source_detached_axes_cold";
							if (budget && (!names->Resize(OwnedTextCapacity(payload.SeparatedVec2->Port)) ||
										   !rowQuoteCharge.Merge(std::move(*names))))
								goto limited;
							remember(payload.SeparatedVec2->Port);
						}
						if (payload.SeparatedVec2->Separated != (separated != 0) ||
							payload.SeparatedVec2->Initialized != (marker != "source_detached_axes_cold"))
							goto malformed;
						if (marker == "source_detached_axis")
							axisKeys = &payload.SeparatedVec2->Axes[axis == "x" ? 0 : 1].Keys;
					}
					if (axisKeys) {
						release(axisNode);
						release(axisPort);
						axisNode = std::move(owner);
						axisPort = std::move(port);
						remember(axisNode);
						remember(axisPort);
						detachedKeyBlock = true;
					}
				}
			} else if (marker == "source_animator_keys_end" && parsed.FormatVersion >= 10) {
				if (!axisKeys || !detachedKeyBlock || HasTrailing(row)) goto malformed;
				axisKeys = nullptr;
				detachedKeyBlock = false;
			} else if (marker == "keyframe") {
				Keyframe keyframe;
				if (!readQuoted(row, keyframe.NodeId) || !readQuoted(row, keyframe.Port) ||
					!(row >> keyframe.Tick) || !readQuoted(row, keyframe.Interpolation) ||
					!ReadValue(
						row,
						keyframe.Data,
						parsed.FormatVersion,
						true,
						budget,
						candidateCharge,
						&allocationRefused
					) ||
					HasTrailing(row))
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
				if (totalKeys == Limits::MaximumKeyframes) {
					SetDiagnostic(
						diagnostic,
						Status::LimitExceeded,
						"document exceeds the keyframe limit",
						keyframe.NodeId,
						keyframe.Port
					);
					return diagnostic.Code;
				}
				if (axisKeys && (keyframe.NodeId != axisNode || keyframe.Port != axisPort)) goto malformed;
				if (!reserveSlots(keyframes(), keyframes().size() + 1)) goto limited;
				keyframes().push_back(std::move(keyframe));
				remember(keyframes().back().NodeId);
				remember(keyframes().back().Port);
				remember(keyframes().back().Interpolation);
				++totalKeys;
			} else if (marker == "key_source_id" && parsed.FormatVersion >= 9) {
				std::string nodeId, port, id;
				uint64_t tick = 0;
				if (!readQuoted(row, nodeId) || !readQuoted(row, port) || !(row >> tick) ||
					!readQuoted(row, id, Limits::MaximumSourceKeyIdBytes) || id.empty() || HasTrailing(row) ||
					keyframes().empty())
					goto malformed;
				auto &key = keyframes().back();
				if (key.NodeId != nodeId || key.Port != port || key.Tick != tick ||
					!keySourceIds.emplace(totalKeys - 1).second)
					goto malformed;
				key.SourceKeyId = std::move(id);
				remember(key.SourceKeyId);
			} else if (marker == "key_time" && parsed.FormatVersion >= 9) {
				std::string nodeId, port, sign;
				FrameTime time;
				if (!readQuoted(row, nodeId) || !readQuoted(row, port) ||
					!(row >> time.Tick >> token(sign) >> time.Subframe) || HasTrailing(row) ||
					(sign != "positive" && sign != "negative") || keyframes().empty())
					goto malformed;
				time.NegativeFrame = sign == "negative";
				auto &key = keyframes().back();
				if (key.NodeId != nodeId || key.Port != port || key.Tick != time.Tick ||
					!ValidFrameTime(time) || !keyTimes.emplace(totalKeys - 1).second)
					goto malformed;
				SetFrameTime(key, time);
			} else if (marker == "key_kind" && parsed.FormatVersion >= 9) {
				std::string nodeId, port, kind;
				uint64_t tick = 0;
				if (!readQuoted(row, nodeId) || !readQuoted(row, port) || !(row >> tick >> token(kind)) ||
					HasTrailing(row) || keyframes().empty() || (kind != "normal" && kind != "adder"))
					goto malformed;
				auto &key = keyframes().back();
				if (key.NodeId != nodeId || key.Port != port || key.Tick != tick ||
					!keyKinds.emplace(totalKeys - 1).second)
					goto malformed;
				key.Kind = kind == "adder" ? KeyframeKind::Adder : KeyframeKind::Normal;
			} else if (marker == "key_ease" && parsed.FormatVersion >= 4) {
				std::string nodeId, port;
				uint64_t tick = 0;
				KeyframeEase ease;
				if (!readQuoted(row, nodeId) || !readQuoted(row, port) || !(row >> tick) ||
					!readQuoted(row, ease.InType) || !readQuoted(row, ease.OutType) ||
					!(row >> ease.In.X >> ease.In.Y >> ease.Out.X >> ease.Out.Y) || HasTrailing(row))
					goto malformed;
				if (keyframes().empty() || keyframes().back().NodeId != nodeId ||
					keyframes().back().Port != port || keyframes().back().Tick != tick ||
					keyframes().back().Ease)
					goto malformed;
				if (!std::isfinite(ease.In.X) || !std::isfinite(ease.In.Y) || !std::isfinite(ease.Out.X) ||
					!std::isfinite(ease.Out.Y))
					goto malformed;
				keyframes().back().Ease = std::move(ease);
				remember(keyframes().back().Ease->InType);
				remember(keyframes().back().Ease->OutType);
			} else if ((marker == "key_driver" && parsed.FormatVersion >= 6) ||
					   (marker == "key_source_driver" && parsed.FormatVersion >= 8)) {
				std::string nodeId, port, type;
				uint64_t tick = 0;
				if (!readQuoted(row, nodeId) || !readQuoted(row, port) || !(row >> tick) ||
					!readQuoted(row, type))
					goto malformed;
				if (keyframes().empty() || keyframes().back().NodeId != nodeId ||
					keyframes().back().Port != port || keyframes().back().Tick != tick ||
					keyframes().back().SineDriver || keyframes().back().SourceDriver)
					goto malformed;
				auto &key = keyframes().back();
				if (type == "sine") {
					KeyframeSineDriver driver;
					if (!(row >> driver.Frequency >> driver.Amplitude >> driver.Phase >> driver.Smooth) ||
						!std::isfinite(driver.Frequency) || !std::isfinite(driver.Amplitude) ||
						!std::isfinite(driver.Phase) || !std::isfinite(driver.Smooth))
						goto malformed;
					if (marker == "key_source_driver")
						key.SourceDriver = driver;
					else
						key.SineDriver = driver;
				} else {
					if (marker != "key_source_driver") goto malformed;
					if (type == "linear") {
						KeyframeLinearDriver driver;
						if (!(row >> driver.Speed)) goto malformed;
						key.SourceDriver = driver;
					} else if (type == "snap") {
						KeyframeSnapDriver driver;
						if (!(row >> driver.Size)) goto malformed;
						key.SourceDriver = driver;
					} else if (type == "bounce") {
						KeyframeBounceDriver driver;
						if (!(row >> driver.Amount >> driver.Spacing >> driver.Curve)) goto malformed;
						key.SourceDriver = driver;
					} else if (type == "elastic") {
						KeyframeElasticDriver driver;
						if (!(row >> driver.Amount >> driver.Spacing >> driver.Curve)) goto malformed;
						key.SourceDriver = driver;
					} else if (type == "native_audio") {
						KeyframeAudioDriver driver;
						if (!readQuoted(row, driver.SourceId) || !readQuoted(row, driver.Metric) ||
							!(row >> driver.Channel >> driver.Gain >> driver.Bias))
							goto malformed;
						key.SourceDriver = std::move(driver);
						if (const auto *audio = std::get_if<KeyframeAudioDriver>(&*key.SourceDriver)) {
							remember(audio->SourceId);
							remember(audio->Metric);
						}
					} else if (type == "curve") {
						std::string tag;
						size_t count;
						Curve curve;
						if (!(row >> token(tag) >> count) || tag != "q" ||
							count > Limits::MaximumCurveAnchors)
							goto malformed;
						for (double &field : curve.Header)
							if (!(row >> field) || !std::isfinite(field)) goto malformed;
						if (!reserveSlots(curve.Anchors, count)) goto limited;
						curve.Anchors.resize(count);
						for (auto &anchor : curve.Anchors)
							for (double &field : anchor)
								if (!(row >> field) || !std::isfinite(field)) goto malformed;
						key.SourceDriver = KeyframeCurveDriver{std::move(curve)};
					} else
						goto malformed;
					if (!detail::ValidSourceDriver(*key.SourceDriver)) goto malformed;
				}
				if (HasTrailing(row)) goto malformed;
			} else if (marker == "timeline" && parsed.FormatVersion >= 4) {
				TimelineSettings timeline;
				if (parsed.Timeline || !(row >> timeline.Frames >> timeline.First >> timeline.Last) ||
					!readQuoted(row, timeline.Playback))
					goto malformed;
				if (parsed.FormatVersion >= 5 && !(row >> timeline.FramesPerSecond)) goto malformed;
				if (HasTrailing(row) || !std::isfinite(timeline.FramesPerSecond) ||
					timeline.FramesPerSecond <= 0 || !std::isfinite(1.0 / timeline.FramesPerSecond) ||
					1.0 / timeline.FramesPerSecond <= 0)
					goto malformed;
				parsed.Timeline = std::move(timeline);
				remember(parsed.Timeline->Playback);
			} else if (marker == "source_timeline_bounds" && parsed.FormatVersion >= 9) {
				if (!parsed.Timeline || parsed.Timeline->SourceBounds) goto malformed;
				SourceAuthoringFrameBounds bounds;
				const auto readBound = [&](SourceAuthoringFrameBound &bound) {
					std::string tag;
					TokenCharge tagCharge;
					row >> std::setw(9);
					if (!ReadToken(row, tag, budget, tagCharge, &allocationRefused)) return false;
					if (tag == "missing")
						bound.Presence = SourceFrameBoundPresence::Missing;
					else if (tag == "null")
						bound.Presence = SourceFrameBoundPresence::Null;
					else if (tag == "explicit") {
						bound.Presence = SourceFrameBoundPresence::Explicit;
						if (!(row >> bound.Value.Tick >> bound.Value.Subframe >> bound.Value.NegativeFrame))
							return false;
					} else
						return false;
					return ValidSourceAuthoringFrameBound(bound);
				};
				if (!readBound(bounds.Start) || !readBound(bounds.End) || HasTrailing(row)) goto malformed;
				parsed.Timeline->SourceBounds = bounds;
			} else if (marker == "project_global_node" && parsed.FormatVersion >= 9) {
				if (!parsed.ProjectGlobalNodeId.empty() ||
					!readQuoted(row, parsed.ProjectGlobalNodeId, Limits::MaximumTextBytes) ||
					parsed.ProjectGlobalNodeId.empty() || HasTrailing(row))
					goto malformed;
				remember(parsed.ProjectGlobalNodeId);
			} else if (marker == "project" && parsed.FormatVersion >= 7) {
				if (budget) {
					auto paletteStorage = budget->Reserve(2 * sizeof(Colour));
					if (!paletteStorage) goto limited;
					if (!candidateCharge->Merge(std::move(*paletteStorage))) std::terminate();
				}
				ProjectSettings project;
				size_t count = 0;
				if (parsed.Project ||
					!(row >> project.SurfaceWidth >> project.SurfaceHeight >> project.Interpolation >>
					  project.Oversample >> count) ||
					count > Limits::MaximumProjectPaletteEntries)
					goto malformed;
				project.Palette.clear();
				for (size_t index = 0; index < count; index++) {
					unsigned red = 0, green = 0, blue = 0, alpha = 0;
					if (!(row >> red >> green >> blue >> alpha) || red > 255 || green > 255 || blue > 255 ||
						alpha > 255)
						goto malformed;
					if (!reserveSlots(project.Palette, project.Palette.size() + 1)) goto limited;
					project.Palette.push_back(
						{static_cast<uint8_t>(red),
						 static_cast<uint8_t>(green),
						 static_cast<uint8_t>(blue),
						 static_cast<uint8_t>(alpha)}
					);
				}
				if (HasTrailing(row)) goto malformed;
				parsed.Project = std::move(project);
			} else if (marker == "slice_stack_action" && parsed.FormatVersion >= 9) {
				SliceStackAction action;
				if (parsed.SliceStackActions.size() >= Limits::MaximumNodes ||
					!readQuoted(row, action.NodeId) ||
					!(row >> action.Time.Tick >> action.Time.Subframe >> action.Time.NegativeFrame >>
					  action.WorkPixels) ||
					HasTrailing(row) || action.NodeId.empty() || !ValidFrameTime(action.Time) ||
					action.WorkPixels > Limits::MaximumArrayElements)
					goto malformed;
				if (!reserveSlots(parsed.SliceStackActions, parsed.SliceStackActions.size() + 1))
					goto limited;
				parsed.SliceStackActions.push_back(std::move(action));
				remember(parsed.SliceStackActions.back().NodeId);
			} else if (marker == "project_shader" && parsed.FormatVersion >= 9) {
				std::string name;
				if (projectShader || !(row >> token(name)) || HasTrailing(row) ||
					(name != "phong" && name != "pbr"))
					goto malformed;
				projectShader = name == "pbr" ? 1 : 0;
			} else if (marker == "project_depth" && parsed.FormatVersion >= 9) {
				std::string name;
				if (projectDepth || !(row >> token(name)) || HasTrailing(row)) goto malformed;
				const auto depth = ParseDepth(name);
				if (!depth || *depth < 2) goto malformed;
				projectDepth = *depth - 2;
			} else if (marker == "preview_grid" && parsed.FormatVersion >= 9) {
				PreviewGridSettings grid;
				if (previewGrid || !(row >> grid.Show >> grid.Snap >> grid.Size.X >> grid.Size.Y) ||
					HasTrailing(row) || !std::isfinite(grid.Size.X) || !std::isfinite(grid.Size.Y) ||
					grid.Size.X < 0 || grid.Size.Y < 0)
					goto malformed;
				previewGrid = grid;
			} else if (marker == "preview_rulers" && parsed.FormatVersion >= 9) {
				bool show = false;
				size_t count = 0;
				if (previewRulers || !(row >> show >> count)) goto malformed;
				if (count > Limits::MaximumArrayElements) {
					SetDiagnostic(
						diagnostic, Status::LimitExceeded, "preview guides exceed the array element budget"
					);
					return diagnostic.Code;
				}
				std::vector<PreviewRulerGuide> guides;
				if (!reserveSlots(guides, count)) goto limited;
				for (size_t index = 0; index < count; ++index) {
					std::string axis;
					double position = 0;
					if (!(row >> token(axis) >> position) || !std::isfinite(position) ||
						(axis != "horizontal" && axis != "vertical"))
						goto malformed;
					guides.push_back(
						{axis == "horizontal" ? PreviewRulerAxis::Horizontal : PreviewRulerAxis::Vertical,
						 position}
					);
				}
				if (HasTrailing(row)) goto malformed;
				previewRulers = std::pair{show, std::move(guides)};
			} else if (marker == "project_regions" && parsed.FormatVersion >= 9) {
				size_t count = 0;
				if (!parsed.Project || projectRegionCount || !(row >> count) || HasTrailing(row))
					goto malformed;
				if (count > Limits::MaximumAnimationRegions) {
					SetDiagnostic(
						diagnostic,
						Status::LimitExceeded,
						"project animation regions exceed their count limit"
					);
					return diagnostic.Code;
				}
				projectRegionCount = count;
				if (!reserveSlots(parsed.Project->AnimationRegions, count)) goto limited;
			} else if (marker == "animation_region" && parsed.FormatVersion >= 9) {
				AnimationRegion region;
				std::string startSign, endSign;
				Value color;
				if (!parsed.Project || !projectRegionCount ||
					parsed.Project->AnimationRegions.size() >= *projectRegionCount ||
					!readQuoted(row, region.Label, Limits::MaximumTextBytes) ||
					!(row >> region.Start.Tick >> token(startSign) >> region.Start.Subframe >>
					  region.End.Tick >> token(endSign) >> region.End.Subframe) ||
					(startSign != "positive" && startSign != "negative") ||
					(endSign != "positive" && endSign != "negative") || !ValidFrameTime(region.Start) ||
					!ValidFrameTime(region.End) ||
					!ReadValue(
						row, color, parsed.FormatVersion, true, budget, candidateCharge, &allocationRefused
					) ||
					!std::holds_alternative<Colour>(color) || HasTrailing(row))
					goto malformed;
				region.Start.NegativeFrame = startSign == "negative";
				region.End.NegativeFrame = endSign == "negative";
				if (!ValidFrameTime(region.Start) || !ValidFrameTime(region.End)) goto malformed;
				if (region.Label.size() > Limits::MaximumArrayBytes - projectRegionTextBytes) {
					SetDiagnostic(
						diagnostic,
						Status::LimitExceeded,
						"project animation region labels exceed their byte limit"
					);
					return diagnostic.Code;
				}
				projectRegionTextBytes += region.Label.size();
				region.Color = std::get<Colour>(std::move(color));
				if (!reserveSlots(
						parsed.Project->AnimationRegions, parsed.Project->AnimationRegions.size() + 1
					))
					goto limited;
				parsed.Project->AnimationRegions.push_back(std::move(region));
				remember(parsed.Project->AnimationRegions.back().Label);
			} else if (marker == "source_region_origin" && parsed.FormatVersion >= 9) {
				size_t index = 0;
				std::string origin;
				if (!(row >> index) || !parsed.Project || !projectRegionCount ||
					index >= parsed.Project->AnimationRegions.size() ||
					!parsed.Project->AnimationRegions[index].SourceRegionId.empty() ||
					!readQuoted(row, origin, Limits::MaximumSourceRegionIdBytes) ||
					!detail::SourceRegionOrdinal(origin) || HasTrailing(row))
					goto malformed;
				parsed.Project->AnimationRegions[index].SourceRegionId = std::move(origin);
				remember(parsed.Project->AnimationRegions[index].SourceRegionId);
			} else if (marker == "track_quaternion" && parsed.FormatVersion >= 8) {
				std::string nodeId, port, mode;
				if (!readQuoted(row, nodeId) || !readQuoted(row, port) || !readQuoted(row, mode) ||
					HasTrailing(row) || (mode != "raw" && mode != "euler") || parsed.Tracks.empty() ||
					parsed.Tracks.back().NodeId != nodeId || parsed.Tracks.back().Port != port ||
					parsed.Tracks.back().QuaternionMode)
					goto malformed;
				parsed.Tracks.back().QuaternionMode = mode == "raw" ? 0 : 1;
			} else if (marker == "track" && parsed.FormatVersion >= 4) {
				AnimationTrack track;
				if (!readQuoted(row, track.NodeId) || !readQuoted(row, track.Port) ||
					!readQuoted(row, track.End) || !(row >> track.LoopRange) || HasTrailing(row))
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
				if (!reserveSlots(parsed.Tracks, parsed.Tracks.size() + 1)) goto limited;
				parsed.Tracks.push_back(std::move(track));
				remember(parsed.Tracks.back().NodeId);
				remember(parsed.Tracks.back().Port);
				remember(parsed.Tracks.back().End);
			} else {
				goto malformed;
			}
			if (!settleQuotedText()) goto limited;
		}
		if (allocationRefused) goto limited;
		if ((previewGrid || previewRulers || projectDepth || projectShader || projectRegionCount) &&
			!parsed.Project)
			goto malformed;
		if (projectRegionCount && parsed.Project->AnimationRegions.size() != *projectRegionCount)
			goto malformed;
		if (parsed.Project && !detail::UniqueSourceRegionOrigins(parsed.Project->AnimationRegions))
			goto malformed;
		if (projectDepth) parsed.Project->ColorDepth = *projectDepth;
		if (projectShader) parsed.Project->Shader3D = *projectShader;
		if (previewGrid) parsed.Project->PreviewGrid = *previewGrid;
		if (previewRulers) {
			parsed.Project->ShowPreviewRulers = previewRulers->first;
			parsed.Project->PreviewRulers = std::move(previewRulers->second);
		}
		{
			// One fixed bounded scratch array validates all nodes without
			// per-origin allocation.
			std::array<uint64_t, Limits::MaximumPixelBuilderDynamicInputsPerNode> origins{};
			std::string_view failedPort;
			for (const auto &node : parsed.Nodes)
				if (detail::ValidateSourceInputOrigins(node.DynamicInputs, origins, failedPort) != Status::Ok)
					goto malformed;
		}
		if (axisKeys) goto malformed;
		{
			size_t aggregateKeys = parsed.Keyframes.size();
			for (const auto &node : parsed.Nodes) {
				if (node.SourceSeparatedVec2Animators)
					for (const auto &input : node.SourceSeparatedVec2Animators->Inputs) {
						if (!input.Initialized) continue;
						if (node.Id.size() > UINT64_MAX - input.Port.size()) goto limited;
						auto lookupCharge = budget ? budget->Reserve(node.Id.size() + input.Port.size())
												   : std::optional<detail::AllocationReservation>{};
						if (budget && !lookupCharge) goto limited;
						std::string lookupNode, lookupPort;
						if (budget) {
							lookupNode = std::string(node.Id.size(), '\0');
							lookupPort = std::string(input.Port.size(), '\0');
							const uint64_t nodeBytes = OwnedTextCapacity(lookupNode);
							const uint64_t portBytes = OwnedTextCapacity(lookupPort);
							if (nodeBytes > UINT64_MAX - portBytes ||
								!lookupCharge->Resize(nodeBytes + portBytes))
								goto limited;
							lookupNode.assign(node.Id);
							lookupPort.assign(input.Port);
						} else {
							lookupNode = node.Id;
							lookupPort = input.Port;
						}
						auto lookup =
							std::make_tuple(std::move(lookupNode), std::move(lookupPort), std::string("x"));
						if (!axesSeen.contains(lookup)) goto malformed;
						std::get<2>(lookup) = "y";
						if (!axesSeen.contains(lookup)) goto malformed;
					}
				const auto defaultsStatus = detail::ValidateSourceVec2Defaults(node, diagnostic);
				if (defaultsStatus != Status::Ok) return defaultsStatus;
				const Status status = detail::ValidateSeparatedVec2(node, aggregateKeys, diagnostic);
				if (status != Status::Ok) return status;
			}
		}
		if (budget) {
			const auto retained = DocumentRetainedPayloadBytes(parsed);
			const uint64_t axisScratchBytes = ownedTextBytes(axisNode) + ownedTextBytes(axisPort);
			if (!retained || axisScratchBytes > persistedTextCharge->Bytes() ||
				*retained < persistedTextCharge->Bytes() - axisScratchBytes)
				goto limited;
			const uint64_t remaining = *retained - (persistedTextCharge->Bytes() - axisScratchBytes);
			if (remaining > candidateCharge->Bytes() && !candidateCharge->Resize(remaining)) goto limited;
		}
		if (detail::ValidateSourceAnimatorState(parsed, diagnostic) != Status::Ok) return diagnostic.Code;
		document = std::move(parsed);
		diagnostic = {};
		return Status::Ok;

	limited:
		allocationRefused = true;
	malformed:
		SetDiagnostic(
			diagnostic,
			allocationRefused ? Status::LimitExceeded : Status::Malformed,
			allocationRefused ? "document allocation exceeds the byte limit"
							  : "malformed imagegraph record at line " + std::to_string(lineNumber)
		);
		return diagnostic.Code;
	}

	Status Read(const std::string &text, Document &document, Diagnostic &diagnostic) {
		return ReadDocumentText(text, document, diagnostic, nullptr, nullptr);
	}

	Status
	Read(std::string_view text, Document &document, Diagnostic &diagnostic, uint64_t maximumOperationBytes) {
		try {
			if (text.size() > Limits::MaximumDocumentBytes) {
				SetDiagnostic(diagnostic, Status::LimitExceeded, "document text exceeds the byte limit");
				return diagnostic.Code;
			}
			const auto previousBytes = DocumentRetainedPayloadBytes(document);
			if (!previousBytes || *previousBytes > maximumOperationBytes ||
				text.size() > maximumOperationBytes - *previousBytes) {
				SetDiagnostic(diagnostic, Status::LimitExceeded, "document read exceeds the byte limit");
				return diagnostic.Code;
			}
			detail::EvaluationBudget budget(maximumOperationBytes);
			auto previousCharge = budget.Reserve(*previousBytes);
			auto borrowedCharge = budget.Reserve(text.size());
			auto ownedCopyCharge = budget.Reserve(text.size());
			if (!previousCharge || !borrowedCharge || !ownedCopyCharge) {
				SetDiagnostic(diagnostic, Status::LimitExceeded, "document read exceeds the byte limit");
				return diagnostic.Code;
			}
			std::string ownedText(text.size(), '\0');
			if (!ownedCopyCharge->Resize(OwnedTextCapacity(ownedText))) {
				SetDiagnostic(diagnostic, Status::LimitExceeded, "document read exceeds the byte limit");
				return diagnostic.Code;
			}
			ownedText.assign(text.data(), text.size());
			auto candidateCharge = budget.Reserve(sizeof(Document));
			if (!candidateCharge) {
				SetDiagnostic(diagnostic, Status::LimitExceeded, "document read exceeds the byte limit");
				return diagnostic.Code;
			}
			return ReadDocumentText(ownedText, document, diagnostic, &budget, &*candidateCharge);
		} catch (const std::bad_alloc &) {
			SetDiagnostic(diagnostic, Status::LimitExceeded, "document allocation exceeds the byte limit");
			return diagnostic.Code;
		} catch (const std::length_error &) {
			SetDiagnostic(diagnostic, Status::LimitExceeded, "document allocation exceeds the byte limit");
			return diagnostic.Code;
		}
	}

	bool ValidProjectPreviewSettings(const ProjectSettings &project) {
		const Vector2 size = project.PreviewGrid.Size;
		if (!std::isfinite(size.X) || !std::isfinite(size.Y) || size.X < 0 || size.Y < 0 ||
			project.PreviewRulers.size() > Limits::MaximumArrayElements)
			return false;
		for (const auto &guide : project.PreviewRulers)
			if (!std::isfinite(guide.Position) ||
				(guide.Axis != PreviewRulerAxis::Horizontal && guide.Axis != PreviewRulerAxis::Vertical))
				return false;
		return true;
	}

	bool ValidProjectAnimationRegions(const ProjectSettings &project) {
		if (!detail::UniqueSourceRegionOrigins(project.AnimationRegions)) return false;
		size_t textBytes = 0;
		for (const AnimationRegion &region : project.AnimationRegions)
			if (region.Label.size() > Limits::MaximumTextBytes ||
				region.Label.size() > Limits::MaximumArrayBytes - textBytes ||
				!ValidFrameTime(region.Start) || !ValidFrameTime(region.End))
				return false;
			else
				textBytes += region.Label.size();
		return true;
	}

	bool ValidKeyframeSourceDriver(const KeyframeSourceDriver &driver) {
		return detail::ValidSourceDriver(driver);
	}

	bool ConvertSourceQuaternion(const Quaternion &tuple, int64_t mode, Quaternion &result) {
		if (!detail::ValidRuntimeValue(tuple) || mode < 0 || mode > 1) return false;
		if (mode == 1) return detail::SourceQuaternionFromEuler(tuple.X, tuple.Y, tuple.Z, result);
		result = tuple;
		return true;
	}

	Status Migrate(Document &document, Diagnostic &diagnostic) {
		if (document.FormatVersion < 1 || document.FormatVersion > 11) {
			SetDiagnostic(diagnostic, Status::UnsupportedVersion, "unsupported imagegraph document version");
			return diagnostic.Code;
		}
		for (const Node &node : document.Nodes) {
			for (const auto *modes : {&node.SourceAnimatedInputs, &node.SourceStaticInputs}) {
				if (modes->size() > Limits::MaximumArrayElements ||
					(!modes->empty() && document.FormatVersion < 9)) {
					SetDiagnostic(
						diagnostic,
						Status::InvalidValue,
						"source animation modes require bounded v9 inputs",
						node.Id
					);
					return diagnostic.Code;
				}
				for (size_t index = 0; index < modes->size(); ++index) {
					const auto &port = (*modes)[index];
					if (port.empty() || port.size() > Limits::MaximumTextBytes ||
						!SourceModePort(node, port) ||
						std::find(modes->begin(), modes->begin() + index, port) != modes->begin() + index ||
						(modes == &node.SourceStaticInputs &&
						 std::find(
							 node.SourceAnimatedInputs.begin(), node.SourceAnimatedInputs.end(), port
						 ) != node.SourceAnimatedInputs.end())) {
						SetDiagnostic(
							diagnostic,
							Status::InvalidValue,
							"source animation mode must name one source input with "
							"disjoint modes",
							node.Id,
							port
						);
						return diagnostic.Code;
					}
				}
			}
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
		if (document.FormatVersion == 6) document.FormatVersion = 7;
		if (document.FormatVersion == 7) document.FormatVersion = 8;
		if (document.FormatVersion == 8) document.FormatVersion = 9;
		diagnostic = {};
		return Status::Ok;
	}

	static const Node *
	EffectiveInputOwner(const Document &document, const Node &start, std::string_view port);
#include "SourceCommonGetters.inc"

	static bool AddBytes(uint64_t &total, uint64_t bytes);

	static Status CompileWithBudget(
		const Document &document,
		Plan &plan,
		Diagnostic &diagnostic,
		detail::EvaluationBudget &budget,
		detail::AllocationReservation &planCharge,
		bool allowNoDeclaredOutputs = false
	) try {
		if (document.FormatVersion < 1 || document.FormatVersion > 11) {
			SetDiagnostic(diagnostic, Status::UnsupportedVersion, "unsupported imagegraph document version");
			return diagnostic.Code;
		}
		if (detail::ValidateSourceAnimatorState(document, diagnostic) != Status::Ok) return diagnostic.Code;
		if (detail::ValidateSourceCommonOwners(document, diagnostic) != Status::Ok) return diagnostic.Code;
		if (document.Nodes.size() > Limits::MaximumNodes || document.Links.size() > Limits::MaximumLinks ||
			document.Groups.size() > Limits::MaximumGroups ||
			document.Junctions.size() > Limits::MaximumJunctions ||
			document.Outputs.size() > Limits::MaximumOutputs ||
			document.Keyframes.size() > Limits::MaximumKeyframes ||
			document.Tracks.size() > Limits::MaximumTracks) {
			SetDiagnostic(diagnostic, Status::LimitExceeded, "document exceeds a graph count limit");
			return diagnostic.Code;
		}
		{
			size_t aggregateKeys = document.Keyframes.size();
			for (const auto &node : document.Nodes) {
				if (node.SourceVec2Defaults && document.FormatVersion < 9) {
					SetDiagnostic(
						diagnostic,
						Status::UnsupportedVersion,
						"source constructor defaults require document version 9",
						node.Id
					);
					return diagnostic.Code;
				}
				if (node.SourceSeparatedVec2Animators && document.FormatVersion < 9) {
					SetDiagnostic(
						diagnostic,
						Status::UnsupportedVersion,
						"separated Vec2 animators require document version 9",
						node.Id
					);
					return diagnostic.Code;
				}
				const auto defaultsStatus = detail::ValidateSourceVec2Defaults(node, diagnostic);
				if (defaultsStatus != Status::Ok) return defaultsStatus;
				const Status status = detail::ValidateSeparatedVec2(node, aggregateKeys, diagnostic);
				if (status != Status::Ok) return status;
			}
		}
		if (document.SliceStackActions.size() > Limits::MaximumNodes) {
			SetDiagnostic(diagnostic, Status::LimitExceeded, "slice actions exceed node cap");
			return diagnostic.Code;
		}
		if (document.FormatVersion < 9 && !document.SliceStackActions.empty()) {
			SetDiagnostic(diagnostic, Status::UnsupportedVersion, "slice actions require document version 9");
			return diagnostic.Code;
		}
		for (size_t index = 0; index < document.SliceStackActions.size(); ++index) {
			const auto &action = document.SliceStackActions[index];
			const auto node = std::find_if(document.Nodes.begin(), document.Nodes.end(), [&](const auto &n) {
				return n.Id == action.NodeId;
			});
			if (node == document.Nodes.end() || node->Type != "pc.3_d_mesh_stack_slice" ||
				action.NodeId.size() > Limits::MaximumTextBytes || !ValidFrameTime(action.Time) ||
				action.WorkPixels > Limits::MaximumArrayElements) {
				SetDiagnostic(
					diagnostic,
					Status::InvalidValue,
					"slice action requires a valid Slice Stack identity, time and pixel budget",
					action.NodeId
				);
				return diagnostic.Code;
			}
			for (size_t earlier = 0; earlier < index; ++earlier)
				if (document.SliceStackActions[earlier].NodeId == action.NodeId &&
					document.SliceStackActions[earlier].Time == action.Time) {
					SetDiagnostic(
						diagnostic, Status::DuplicateId, "slice action identity is duplicated", action.NodeId
					);
					return diagnostic.Code;
				}
		}
		if (document.FormatVersion < 9) {
			const auto recursive = [](const Value &value) {
				const auto *array = std::get_if<ArrayValue>(&value);
				const auto *path = std::get_if<Path2D>(&value);
				if (path && (path->Segmented || path->SourceOperation)) return true;
				if (array) {
					const auto segmented = [](const ElementValue &element) {
						const auto *p = std::get_if<Path2D>(&element);
						return p && (p->Segmented || p->SourceOperation);
					};
					for (const auto &element : array->Elements)
						if (segmented(element)) return true;
					for (const auto &row : array->Nested)
						for (const auto &element : row)
							if (segmented(element)) return true;
				}
				return std::holds_alternative<PathValue3D>(value) ||
					   std::holds_alternative<ParticleValue>(value) ||
					   std::holds_alternative<StrandValue>(value) ||
					   std::holds_alternative<TilesetValue>(value) ||
					   std::holds_alternative<RigidValue>(value) ||
					   std::holds_alternative<AtlasValue>(value) ||
					   (array &&
						(array->ElementType == ValueType::Any || array->ElementType == ValueType::Path3D ||
						 array->ElementType == ValueType::Particle ||
						 array->ElementType == ValueType::Strand ||
						 array->ElementType == ValueType::Tileset || array->ElementType == ValueType::Rigid ||
						 array->ElementType == ValueType::Atlas || !array->Items.empty()));
			};
			for (const auto &node : document.Nodes) {
				for (const auto &value : node.Values)
					if (recursive(value.Data)) {
						SetDiagnostic(
							diagnostic,
							Status::UnsupportedVersion,
							"recursive arrays and spatial paths need document version 9",
							node.Id,
							value.Port
						);
						return diagnostic.Code;
					}
				for (const auto &input : node.DynamicInputs)
					if (input.Default && recursive(*input.Default)) {
						SetDiagnostic(
							diagnostic,
							Status::UnsupportedVersion,
							"recursive arrays and spatial paths need document version 9",
							node.Id,
							input.Id
						);
						return diagnostic.Code;
					}
			}
			for (const auto &junction : document.Junctions)
				if (junction.Type == ValueType::Path3D ||
					(junction.Default && recursive(*junction.Default))) {
					SetDiagnostic(
						diagnostic,
						Status::UnsupportedVersion,
						"recursive arrays and spatial paths need document version 9",
						junction.Id
					);
					return diagnostic.Code;
				}
			for (const auto &key : document.Keyframes)
				if (recursive(key.Data)) {
					SetDiagnostic(
						diagnostic,
						Status::UnsupportedVersion,
						"recursive arrays and spatial paths need document version 9",
						key.NodeId,
						key.Port
					);
					return diagnostic.Code;
				}
		}

		if (document.Project) {
			const ProjectSettings &project = *document.Project;
			if (document.FormatVersion < 7) {
				SetDiagnostic(diagnostic, Status::InvalidValue, "project settings need document version 7");
				return diagnostic.Code;
			}
			if (project.Shader3D < 0 || project.Shader3D > 1 ||
				(document.FormatVersion < 9 && project.Shader3D != 0)) {
				SetDiagnostic(
					diagnostic,
					Status::InvalidValue,
					"project shader requires a supported choice and format version 9"
				);
				return diagnostic.Code;
			}
			if (document.FormatVersion < 9 && !project.AnimationRegions.empty()) {
				SetDiagnostic(
					diagnostic,
					Status::UnsupportedVersion,
					"project animation regions need document version 9"
				);
				return diagnostic.Code;
			}
			if (!ValidProjectAnimationRegions(project)) {
				SetDiagnostic(diagnostic, Status::InvalidValue, "project animation regions are invalid");
				return diagnostic.Code;
			}
			if (project.ColorDepth < 0 || project.ColorDepth > 6) {
				SetDiagnostic(diagnostic, Status::InvalidValue, "project color depth is outside its range");
				return diagnostic.Code;
			}
			if (document.FormatVersion < 9 && project.ColorDepth != 1) {
				SetDiagnostic(
					diagnostic, Status::UnsupportedVersion, "project color depth needs document version 9"
				);
				return diagnostic.Code;
			}
			if (!ValidProjectPreviewSettings(project)) {
				SetDiagnostic(
					diagnostic, Status::InvalidValue, "project preview settings are outside their ranges"
				);
				return diagnostic.Code;
			}
			if (document.FormatVersion < 9 && (project.PreviewGrid != PreviewGridSettings{} ||
											   !project.PreviewRulers.empty() || project.ShowPreviewRulers)) {
				SetDiagnostic(
					diagnostic,
					Status::UnsupportedVersion,
					"authored preview settings need document version 9"
				);
				return diagnostic.Code;
			}
			if (project.SurfaceWidth < 1 || project.SurfaceHeight < 1 ||
				project.SurfaceWidth > Limits::MaximumDimension ||
				project.SurfaceHeight > Limits::MaximumDimension || project.Interpolation < 0 ||
				project.Interpolation > 6 || project.Oversample < 0 || project.Oversample > 12 ||
				project.Palette.size() > Limits::MaximumProjectPaletteEntries) {
				SetDiagnostic(diagnostic, Status::InvalidValue, "project settings are outside their ranges");
				return diagnostic.Code;
			}
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
		if (document.FormatVersion < 8 &&
			(std::any_of(
				 document.Keyframes.begin(),
				 document.Keyframes.end(),
				 [](const Keyframe &key) { return key.SourceDriver.has_value(); }
			 ) ||
			 std::any_of(
				 document.Tracks.begin(),
				 document.Tracks.end(),
				 [](const AnimationTrack &track) { return track.QuaternionMode.has_value(); }
			 ))) {
			SetDiagnostic(
				diagnostic, Status::UnsupportedVersion, "source driver metadata needs imagegraph v8"
			);
			return diagnostic.Code;
		}
		if (document.Timeline) {
			const TimelineSettings &timeline = *document.Timeline;
			if (timeline.SourceBounds && document.FormatVersion < 9) {
				SetDiagnostic(
					diagnostic,
					Status::UnsupportedVersion,
					"source timeline bounds need imagegraph v9",
					{},
					"timeline"
				);
				return diagnostic.Code;
			}
			if (timeline.SourceBounds && !ValidSourceAuthoringFrameBounds(*timeline.SourceBounds)) {
				SetDiagnostic(
					diagnostic,
					Status::InvalidValue,
					"source timeline bounds must be canonical finite signed coordinates",
					{},
					"timeline"
				);
				return diagnostic.Code;
			}
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

		for (const auto &node : document.Nodes) {
			if (const auto fault = detail::ValidateNativeSamplerBindings(node, document.FormatVersion)) {
				SetDiagnostic(
					diagnostic,
					fault->Code,
					std::string(fault->Message),
					node.Id,
					std::string(fault->Argument)
				);
				return diagnostic.Code;
			}
			if (node.SourceDisplayName.size() > Limits::MaximumTextBytes ||
				node.SourceInternalName.size() > Limits::MaximumTextBytes ||
				((!node.SourceDisplayName.empty() || !node.SourceInternalName.empty()) &&
				 document.FormatVersion < 9)) {
				SetDiagnostic(
					diagnostic,
					std::max(node.SourceDisplayName.size(), node.SourceInternalName.size()) >
							Limits::MaximumTextBytes
						? Status::LimitExceeded
						: Status::UnsupportedVersion,
					"source node display name requires bounded imagegraph v9 storage",
					node.Id
				);
				return diagnostic.Code;
			}
		}
		auto nodeIndices = detail::MakeEvaluationHashMap<std::string_view, size_t>(budget);
		auto junctionIndices = detail::MakeEvaluationHashMap<std::string_view, size_t>(budget);
		auto groupIds = detail::MakeEvaluationHashSet<std::string_view>(budget);
		auto outputIds = detail::MakeEvaluationHashSet<std::string_view>(budget);
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
			if (node.Type == "value.noise_field" || node.Type == "value.sample_noise" ||
				IsNoiseImageGenerator(node.Type)) {
				NoiseNodeChoices choices;
				std::string_view failedPort;
				if (!ResolveNoiseNodeChoices(node, choices, failedPort)) {
					SetDiagnostic(
						diagnostic,
						Status::InvalidValue,
						"noise selector is outside its declared choices",
						node.Id,
						failedPort
					);
					return diagnostic.Code;
				}
				if (node.Type == "value.noise_field" && choices.Mode == 1)
					for (const auto &property : node.Values)
						if (property.Port == "position") {
							const auto position = NoiseNodePort(node, "position", PortDirection::Input);
							if (!position || TypeOf(property.Data) != position->Type) {
								SetDiagnostic(
									diagnostic,
									Status::TypeMismatch,
									"noise coordinates must match the selected dimension",
									node.Id,
									"position"
								);
								return diagnostic.Code;
							}
						}
				for (const auto &port : node.SourceAnimatedInputs)
					if (IsNoisePortSelector(node, port)) {
						SetDiagnostic(
							diagnostic,
							Status::InvalidValue,
							"noise port selectors cannot be animated",
							node.Id,
							port
						);
						return diagnostic.Code;
					}
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
			if (node.DynamicInputs.size() > MaximumDynamicInputsForNode(node)) {
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
			auto portIds = detail::MakeEvaluationHashSet<std::string_view>(budget);
			auto sourceInputIds = detail::MakeEvaluationHashSet<std::string_view>(budget);
			for (const PortSchema &port : FindSchema(node.Type)->Ports)
				portIds.insert(port.Id);
			for (const DynamicInput &input : node.DynamicInputs) {
				if (const auto selected = detail::SourceLuaArgumentType(node, input.Id);
					selected && *selected != input.Type) {
					SetDiagnostic(
						diagnostic,
						Status::TypeMismatch,
						"Lua argument socket type does not match its argument type selector",
						node.Id,
						input.Id
					);
					return diagnostic.Code;
				}
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
				if (!input.SourceInputId.empty()) {
					if (document.FormatVersion < 9 || !detail::SourceInputOrdinal(input.SourceInputId)) {
						SetDiagnostic(
							diagnostic,
							Status::InvalidValue,
							"source input origin needs a bounded canonical v9 name",
							node.Id,
							input.Id
						);
						return diagnostic.Code;
					}
					if (!sourceInputIds.insert(input.SourceInputId).second) {
						SetDiagnostic(
							diagnostic,
							Status::DuplicateId,
							"source input origin is duplicated within its node",
							node.Id,
							input.Id
						);
						return diagnostic.Code;
					}
				}
				if (input.SourceLayerName.size() > Limits::MaximumTextBytes ||
					(document.FormatVersion < 9 && !input.SourceLayerName.empty())) {
					SetDiagnostic(
						diagnostic,
						Status::InvalidValue,
						"source layer binding needs bounded imagegraph v9 text",
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
					const auto *dynamicCatalogue = FindCatalogueEntry(node.Type);
					size_t dynamicGroup = 0;
					const auto *dynamicTemplate =
						dynamicCatalogue ? FindDynamicTemplate(*dynamicCatalogue, input.Id, dynamicGroup)
										 : nullptr;
					const auto *dynamicArray = std::get_if<ArrayValue>(&*input.Default);
					const bool sourceDefault =
						dynamicTemplate &&
						(CatalogueSourceRawValue(*dynamicTemplate, *input.Default) ||
						 CatalogueSourceEnumValue(*dynamicTemplate, *input.Default) ||
						 (dynamicArray &&
						  CatalogueAuthoredArray(*dynamicCatalogue, *dynamicTemplate, *dynamicArray)));
					if (input.Type != ValueType::Any && TypeOf(*input.Default) != input.Type &&
						!detail::SourceLuaArgumentType(node, input.Id) &&
						!(node.Type == "pc.hlsl" && detail::SourceArgumentType(node, input.Id)) &&
						!detail::PuppetControlValue(node, input.Id, input.Type, *input.Default) &&
						!sourceDefault) {
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
			if (!node.DynamicOutputs.empty() &&
				(document.FormatVersion < 9 || !FindSchema(node.Type)->DynamicOutputs)) {
				SetDiagnostic(
					diagnostic,
					Status::InvalidValue,
					"node type does not declare dynamic outputs",
					node.Id,
					node.DynamicOutputs.front().Id
				);
				return diagnostic.Code;
			}
			if (node.DynamicOutputs.size() > Limits::MaximumDynamicOutputsPerNode) {
				SetDiagnostic(
					diagnostic, Status::LimitExceeded, "node exceeds the dynamic output limit", node.Id
				);
				return diagnostic.Code;
			}
			for (const DynamicOutput &output : node.DynamicOutputs) {
				if (TypeName(output.Type).empty() || output.Id.empty() ||
					output.Id.size() > Limits::MaximumTextBytes) {
					SetDiagnostic(
						diagnostic,
						Status::InvalidValue,
						"dynamic output declaration is invalid",
						node.Id,
						output.Id
					);
					return diagnostic.Code;
				}
				if (!portIds.insert(output.Id).second) {
					SetDiagnostic(
						diagnostic, Status::DuplicateId, "dynamic output id is duplicated", node.Id, output.Id
					);
					return diagnostic.Code;
				}
			}
			if (!node.SourceProperties.empty() && document.FormatVersion < 9) {
				SetDiagnostic(
					diagnostic, Status::UnsupportedVersion, "source properties require imagegraph v9", node.Id
				);
				return diagnostic.Code;
			}
			if (node.SourceProperties.size() > Limits::MaximumPropertiesPerNode) {
				SetDiagnostic(
					diagnostic, Status::LimitExceeded, "source property count exceeds its limit", node.Id
				);
				return diagnostic.Code;
			}
			auto sourcePropertyIds = detail::MakeEvaluationHashSet<std::string_view>(budget);
			for (const AuthoredValue &property : node.SourceProperties) {
				if (property.Port.size() > Limits::MaximumTextBytes || !WithinValueBudget(property.Data)) {
					SetDiagnostic(
						diagnostic,
						Status::LimitExceeded,
						"source property exceeds its limit",
						node.Id,
						property.Port
					);
					return diagnostic.Code;
				}
				if (property.Port.empty() || !IsAuthoredValueType(TypeOf(property.Data)) ||
					!detail::ValidRuntimeValue(property.Data)) {
					SetDiagnostic(
						diagnostic,
						Status::InvalidValue,
						"source property is not a valid authored value",
						node.Id,
						property.Port
					);
					return diagnostic.Code;
				}
				if (!sourcePropertyIds.insert(property.Port).second) {
					SetDiagnostic(
						diagnostic, Status::DuplicateId, "duplicate source property", node.Id, property.Port
					);
					return diagnostic.Code;
				}
			}
			auto valueIds = detail::MakeEvaluationHashSet<std::string_view>(budget);
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
				const CatalogueEntry *catalogue = FindCatalogueEntry(node.Type);
				const CatalogueInput *sourceInput =
					catalogue ? FindCatalogueInput(*catalogue, value.Port) : nullptr;
				const auto *sourceArray = std::get_if<ArrayValue>(&value.Data);
				const bool sourceArrayMatches =
					sourceInput && sourceArray &&
					CatalogueAuthoredArray(*catalogue, *sourceInput, *sourceArray);
				const bool sourceEnumMatches =
					sourceInput && CatalogueSourceEnumValue(*sourceInput, value.Data);
				const bool emptyGroupAnimator =
					sourceInput && (node.Type == "pc.group_input" || node.Type == "pc.group_output") &&
					std::find(
						node.SourceAnimatedInputs.begin(), node.SourceAnimatedInputs.end(), value.Port
					) != node.SourceAnimatedInputs.end() &&
					std::any_of(
						document.Tracks.begin(),
						document.Tracks.end(),
						[&](const AnimationTrack &track) {
							return track.NodeId == node.Id && track.Port == value.Port;
						}
					) &&
					std::none_of(
						document.Keyframes.begin(), document.Keyframes.end(), [&](const Keyframe &key) {
							return key.NodeId == node.Id && key.Port == value.Port;
						}
					);
				const auto *emptyScalar = std::get_if<double>(&value.Data);
				const bool sourceStaticEmptyVector =
					sourceInput && emptyScalar && *emptyScalar == 0.0 && node.Type == "pc.group_input" &&
					sourceInput->Type == ValueType::Vector2 &&
					(sourceInput->SourceKind == "Range" || sourceInput->SourceKind == "Vec2") &&
					std::find(
						node.SourceAnimatedInputs.begin(), node.SourceAnimatedInputs.end(), value.Port
					) == node.SourceAnimatedInputs.end() &&
					std::none_of(
						document.Keyframes.begin(), document.Keyframes.end(), [&](const Keyframe &key) {
							return key.NodeId == node.Id && key.Port == value.Port;
						}
					);
				const bool sourceEmptyKeyMatches =
					EmptySourceScalarArray(value.Data) &&
					SourceEmptyGroupVectorKey(document, node, sourceInput, value.Port);
				const bool sourceEmptyMatches =
					emptyGroupAnimator &&
					((emptyScalar && *emptyScalar == 0.0 && sourceInput->SourceKind != "Range" &&
					  sourceInput->SourceKind != "Vec2") ||
					 (sourceArray && sourceArray->ElementType == ValueType::Scalar &&
					  sourceArray->Elements.empty() && sourceArray->Nested.empty() &&
					  sourceArray->Items.empty() &&
					  (sourceInput->SourceKind == "Range" || sourceInput->SourceKind == "Vec2")));
				const auto *unionProperty = FindPort(node.Type, value.Port, PortDirection::Input);
				const bool unionValue = unionProperty && !unionProperty->Alternatives.empty() &&
										std::find(
											unionProperty->Alternatives.begin(),
											unionProperty->Alternatives.end(),
											TypeOf(value.Data)
										) != unionProperty->Alternatives.end();
				const bool tunnelLiteral = node.Type == "pc.tunnel_in" && value.Port == "value_in" &&
										   property->Type == ValueType::Any &&
										   detail::ValidValuePayload(value.Data, false);
				if (TypeOf(value.Data) != property->Type && !unionValue && !tunnelLiteral &&
					!(node.Type == "pc.group_input" && value.Port == "parent_value") &&
					!detail::SourceArgumentAuthoredDefault(node, value.Port, value.Data) &&
					!sourceArrayMatches && !sourceEnumMatches && !sourceEmptyMatches &&
					!sourceEmptyKeyMatches && !sourceStaticEmptyVector &&
					!(sourceInput && CatalogueSourceRawValue(*sourceInput, value.Data))) {
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
			if (const CatalogueEntry *entry = FindCatalogueEntry(node.Type)) {
				for (const AuthoredValue &value : node.Values) {
					const auto *choice = std::get_if<EnumValue>(&value.Data);
					const CatalogueInput *input = choice ? FindCatalogueInput(*entry, value.Port) : nullptr;
					const size_t count = input && !CatalogueSourceEnumValue(*input, value.Data)
											 ? CatalogueChoiceCount(*input)
											 : 0;
					if (count > 0 && (choice->Value < 0 || static_cast<size_t>(choice->Value) >= count)) {
						SetDiagnostic(
							diagnostic,
							Status::InvalidValue,
							"enum value is outside its declared choices",
							node.Id,
							value.Port
						);
						return diagnostic.Code;
					}
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
			if ((!group.RenderActive || !group.PureFunction) && document.FormatVersion < 10) {
				SetDiagnostic(
					diagnostic,
					Status::UnsupportedVersion,
					"group rendering and purity flags need document version 10",
					group.Id
				);
				return diagnostic.Code;
			}
			if (group.Interpolation < 0 || group.Interpolation > 7 || group.Oversample < 0 ||
				group.Oversample > 13) {
				SetDiagnostic(
					diagnostic,
					Status::InvalidGroup,
					"group sampling attribute is outside its source range",
					group.Id
				);
				return diagnostic.Code;
			}
			if (document.FormatVersion < 9 && (group.Interpolation != 0 || group.Oversample != 0)) {
				SetDiagnostic(
					diagnostic,
					Status::UnsupportedVersion,
					"group sampling needs document version 9",
					group.Id
				);
				return diagnostic.Code;
			}
			if (group.ColorDepth < 0 || group.ColorDepth > 8) {
				SetDiagnostic(
					diagnostic, Status::InvalidGroup, "group color depth is outside its range", group.Id
				);
				return diagnostic.Code;
			}
			if (document.FormatVersion < 9 && group.ColorDepth != 1) {
				SetDiagnostic(
					diagnostic,
					Status::UnsupportedVersion,
					"group color depth needs document version 9",
					group.Id
				);
				return diagnostic.Code;
			}
			if (group.Id.size() > Limits::MaximumTextBytes || group.Name.size() > Limits::MaximumTextBytes ||
				group.ParentId.size() > Limits::MaximumTextBytes ||
				group.OwnerNodeId.size() > Limits::MaximumTextBytes) {
				SetDiagnostic(
					diagnostic, Status::LimitExceeded, "group text exceeds the byte limit", group.Id
				);
				return diagnostic.Code;
			}
			if (!group.OwnerNodeId.empty()) {
				if (document.FormatVersion < 9) {
					SetDiagnostic(
						diagnostic,
						Status::UnsupportedVersion,
						"inline collection ownership needs document version 9",
						group.Id
					);
					return diagnostic.Code;
				}
				const auto owner = nodeIndices.find(group.OwnerNodeId);
				if (owner == nodeIndices.end()) {
					SetDiagnostic(
						diagnostic,
						Status::InvalidGroup,
						"inline collection owner node is missing",
						group.Id,
						group.OwnerNodeId
					);
					return diagnostic.Code;
				}
				const Node &node = document.Nodes[owner->second];
				const bool collection =
					node.Type == "pc.verlet_sim_inline" || node.Type == "pc.rigid_group_inline" ||
					node.Type == "pc.flip_group_inline" || node.Type == "pc.strand_group_inline" ||
					node.Type == "pc.vfx_group_inline" || node.Type == "pc.pixel_builder";
				if (!collection || node.GroupId != group.ParentId ||
					(node.Type != "pc.pixel_builder" && !group.Ports.empty())) {
					SetDiagnostic(
						diagnostic,
						Status::InvalidGroup,
						"collection owner must share its parent canvas; only Pixel Builder accepts boundary "
						"ports",
						group.Id,
						group.OwnerNodeId
					);
					return diagnostic.Code;
				}
				for (const Group &other : document.Groups)
					if (&other != &group && other.OwnerNodeId == group.OwnerNodeId) {
						SetDiagnostic(
							diagnostic,
							Status::InvalidGroup,
							"collection node owns more than one inline scope",
							group.Id,
							group.OwnerNodeId
						);
						return diagnostic.Code;
					}
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
				if (junction.Type != ValueType::Any && TypeOf(*junction.Default) != junction.Type) {
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
		for (const Node &node : document.Nodes) {
			for (const auto *modes : {&node.SourceAnimatedInputs, &node.SourceStaticInputs}) {
				if (modes->size() > Limits::MaximumArrayElements ||
					(!modes->empty() && document.FormatVersion < 9)) {
					SetDiagnostic(
						diagnostic,
						Status::InvalidValue,
						"source animation modes require bounded v9 inputs",
						node.Id
					);
					return diagnostic.Code;
				}
				for (size_t index = 0; index < modes->size(); ++index) {
					const auto &port = (*modes)[index];
					if (port.empty() || port.size() > Limits::MaximumTextBytes ||
						!SourceModePort(node, port) ||
						std::find(modes->begin(), modes->begin() + index, port) != modes->begin() + index ||
						(modes == &node.SourceStaticInputs &&
						 std::find(
							 node.SourceAnimatedInputs.begin(), node.SourceAnimatedInputs.end(), port
						 ) != node.SourceAnimatedInputs.end())) {
						SetDiagnostic(
							diagnostic,
							Status::InvalidValue,
							"source animation mode must name one source input with "
							"disjoint modes",
							node.Id,
							port
						);
						return diagnostic.Code;
					}
				}
			}
		}

		for (const Node &node : document.Nodes) {
			if (node.InstanceOverrides.size() > Limits::MaximumArrayElements ||
				(!node.InstanceOverrides.empty() && document.FormatVersion < 9)) {
				SetDiagnostic(
					diagnostic,
					Status::LimitExceeded,
					"instance override controls exceed the durable bound",
					node.Id
				);
				return diagnostic.Code;
			}
			for (size_t index = 0; index < node.InstanceOverrides.size(); ++index) {
				const auto &port = node.InstanceOverrides[index];
				const auto type = IsNoiseImageGenerator(node.Type) && port == "output_type"
									  ? std::optional<ValueType>{ValueType::Enum}
									  : FindPortType(node, port, PortDirection::Input);
				if (port.size() > Limits::MaximumTextBytes || !type ||
					std::find(node.InstanceOverrides.begin(), node.InstanceOverrides.begin() + index, port) !=
						node.InstanceOverrides.begin() + index) {
					SetDiagnostic(
						diagnostic,
						Status::InvalidValue,
						"instance override must name one declared input",
						node.Id,
						port
					);
					return diagnostic.Code;
				}
			}
			const Node *current = &node;
			for (size_t hop = 0; !current->InstanceBase.empty(); ++hop) {
				if (document.FormatVersion < 9 || current->InstanceBase.size() > Limits::MaximumTextBytes ||
					hop >= document.Nodes.size()) {
					SetDiagnostic(
						diagnostic, Status::InvalidGroup, "node instance base is invalid or cyclic", node.Id
					);
					return diagnostic.Code;
				}
				const auto base = nodeIndices.find(current->InstanceBase);
				if (base == nodeIndices.end() || document.Nodes[base->second].Type != node.Type) {
					SetDiagnostic(
						diagnostic,
						Status::InvalidGroup,
						"node instance base must have the same represented class",
						node.Id
					);
					return diagnostic.Code;
				}
				current = &document.Nodes[base->second];
			}
		}
		for (const Node &node : document.Nodes) {
			const Node *current = &node;
			for (size_t hop = 0; !current->SourceParentInputBase.empty(); ++hop) {
				if (document.FormatVersion < 10 || current->Type != "pc.group_input" ||
					current->SourceParentInputBase.size() > Limits::MaximumTextBytes ||
					hop >= document.Nodes.size()) {
					SetDiagnostic(
						diagnostic,
						Status::InvalidGroup,
						"parent input base is invalid or cyclic",
						node.Id,
						"parent_value"
					);
					return diagnostic.Code;
				}
				const auto base = nodeIndices.find(current->SourceParentInputBase);
				if (base == nodeIndices.end() || document.Nodes[base->second].Type != "pc.group_input") {
					SetDiagnostic(
						diagnostic,
						Status::InvalidGroup,
						"parent input base must name a Group input",
						node.Id,
						"parent_value"
					);
					return diagnostic.Code;
				}
				current = &document.Nodes[base->second];
			}
		}
		for (const Group &group : document.Groups) {
			const Group *current = &group;
			for (size_t hop = 0; !current->InstanceBase.empty(); ++hop) {
				const auto base =
					std::find_if(document.Groups.begin(), document.Groups.end(), [&](const Group &g) {
						return g.Id == current->InstanceBase;
					});
				if (document.FormatVersion < 9 || current->InstanceBase.size() > Limits::MaximumTextBytes ||
					hop >= document.Groups.size() || base == document.Groups.end()) {
					SetDiagnostic(
						diagnostic, Status::InvalidGroup, "group instance base is invalid or cyclic", group.Id
					);
					return diagnostic.Code;
				}
				current = &*base;
			}
		}
		for (const Group &group : document.Groups) {
			auto portIds = detail::MakeEvaluationHashSet<std::string_view>(budget);
			auto routedJunctions = detail::MakeEvaluationHashSet<std::string_view>(budget);
			for (const GroupPort &port : group.Ports) {
				if (port.Direction != PortDirection::Input && port.Direction != PortDirection::Output) {
					SetDiagnostic(
						diagnostic, Status::InvalidValue, "group port direction is invalid", group.Id, port.Id
					);
					return diagnostic.Code;
				}
				if (port.Id.size() > Limits::MaximumTextBytes ||
					port.ControlNodeId.size() > Limits::MaximumTextBytes ||
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
				if (!port.ControlNodeId.empty()) {
					const auto control = nodeIndices.find(port.ControlNodeId);
					const std::string_view expected =
						port.Direction == PortDirection::Input ? "pc.group_input" : "pc.group_output";
					if (document.FormatVersion < 9 || control == nodeIndices.end() ||
						document.Nodes[control->second].GroupId != group.Id ||
						document.Nodes[control->second].Type != expected) {
						SetDiagnostic(
							diagnostic,
							Status::InvalidGroup,
							"group boundary must reference its own declared control node",
							group.Id,
							port.Id
						);
						return diagnostic.Code;
					}
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
		auto keyframeIds =
			detail::MakeEvaluationSet<std::tuple<std::string_view, std::string_view, bool, uint64_t, double>>(
				budget
			);
		auto sourceKeyIds =
			detail::MakeEvaluationSet<std::tuple<std::string_view, std::string_view, std::string_view>>(
				budget
			);
		for (const auto &node : document.Nodes)
			if (node.SourceSeparatedVec2Animators)
				for (const auto &input : node.SourceSeparatedVec2Animators->Inputs)
					for (const auto &axis : input.Axes)
						for (const auto &key : axis.Keys)
							if (!key.SourceKeyId.empty())
								sourceKeyIds.emplace(node.Id, input.Port, key.SourceKeyId);
		for (const Keyframe &keyframe : document.Keyframes) {
			if (keyframe.SourceKeyId.size() > Limits::MaximumSourceKeyIdBytes) {
				SetDiagnostic(
					diagnostic,
					Status::LimitExceeded,
					"source key identity exceeds its byte limit",
					keyframe.NodeId,
					keyframe.Port
				);
				return diagnostic.Code;
			}
			if (!keyframe.SourceKeyId.empty()) {
				if (document.FormatVersion < 9) {
					SetDiagnostic(
						diagnostic,
						Status::UnsupportedVersion,
						"source key identity needs document version 9",
						keyframe.NodeId,
						keyframe.Port
					);
					return diagnostic.Code;
				}
				if (!sourceKeyIds.emplace(keyframe.NodeId, keyframe.Port, keyframe.SourceKeyId).second) {
					SetDiagnostic(
						diagnostic,
						Status::InvalidValue,
						"duplicate source key identity",
						keyframe.NodeId,
						keyframe.Port
					);
					return diagnostic.Code;
				}
			}
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
			if (keyframe.Kind != KeyframeKind::Normal && keyframe.Kind != KeyframeKind::Adder) {
				SetDiagnostic(
					diagnostic,
					Status::InvalidValue,
					"keyframe kind must be normal or adder",
					keyframe.NodeId,
					keyframe.Port
				);
				return diagnostic.Code;
			}
			if (document.FormatVersion < 9 && keyframe.Kind != KeyframeKind::Normal) {
				SetDiagnostic(
					diagnostic,
					Status::UnsupportedVersion,
					"source key kind metadata needs document version 9",
					keyframe.NodeId,
					keyframe.Port
				);
				return diagnostic.Code;
			}
			if (!ValidFrameTime(GetFrameTime(keyframe))) {
				SetDiagnostic(
					diagnostic,
					Status::InvalidValue,
					"keyframe time must be canonical and bounded",
					keyframe.NodeId,
					keyframe.Port
				);
				return diagnostic.Code;
			}
			if (document.FormatVersion < 9 && (keyframe.Subframe != 0 || keyframe.NegativeFrame)) {
				SetDiagnostic(
					diagnostic,
					Status::UnsupportedVersion,
					"signed fractional keys need document version 9",
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
			const auto &keyNode = document.Nodes[node->second];
			if (IsNoisePortSelector(keyNode, keyframe.Port)) {
				SetDiagnostic(
					diagnostic,
					Status::InvalidValue,
					"noise port selectors cannot be animated",
					keyframe.NodeId,
					keyframe.Port
				);
				return diagnostic.Code;
			}
			const PropertySchema *property = FindProperty(document.Nodes[node->second].Type, keyframe.Port);
			// A dynamic input with an authored type animates like a property.
			std::optional<ValueType> keyedType = property ? std::optional{property->Type} : std::nullopt;
			if ((keyNode.Type == "value.noise_field" || keyNode.Type == "value.sample_noise") &&
				keyframe.Port == "position") {
				const auto port = NoiseNodePort(keyNode, keyframe.Port, PortDirection::Input);
				if (!port)
					keyedType = std::nullopt;
				else if (port->Alternatives.empty())
					keyedType = port->Type;
				else if (std::find(
							 port->Alternatives.begin(), port->Alternatives.end(), TypeOf(keyframe.Data)
						 ) != port->Alternatives.end())
					keyedType = TypeOf(keyframe.Data);
			}
			if (keyNode.Type == "pc.tunnel_in" && keyframe.Port == "value_in" && property &&
				property->Type == ValueType::Any && detail::ValidValuePayload(keyframe.Data, false))
				keyedType = TypeOf(keyframe.Data);
			if (document.FormatVersion >= 9 && keyframe.Interpolation == "source" &&
				detail::SourceArgumentAuthoredDefault(
					document.Nodes[node->second], keyframe.Port, keyframe.Data
				))
				keyedType = TypeOf(keyframe.Data);
			for (const DynamicInput &input : document.Nodes[node->second].DynamicInputs)
				if (!property && input.Id == keyframe.Port) {
					const auto *source = detail::AliasedSourceInput(document.Nodes[node->second], input.Id);
					const bool physicalAny =
						document.FormatVersion >= 9 && keyframe.Interpolation == "source" &&
						input.Type == ValueType::Any && source && source->Type == ValueType::Any;
					if ((physicalAny ||
						 detail::SourceLuaArgumentType(document.Nodes[node->second], input.Id) ||
						 (document.Nodes[node->second].Type == "pc.hlsl" &&
						  input.Id.starts_with("argument_value_"))) &&
						IsAuthoredValueType(TypeOf(keyframe.Data)))
						keyedType = TypeOf(keyframe.Data);
					else if (IsAuthoredValueType(input.Type))
						keyedType = input.Type;
					else if (detail::PuppetControlValue(
								 document.Nodes[node->second], input.Id, input.Type, keyframe.Data
							 ))
						keyedType = ValueType::Array;
				}
			if (!keyedType) {
				SetDiagnostic(
					diagnostic,
					Status::UnknownPort,
					"keyframe refers to an unknown property",
					keyframe.NodeId,
					keyframe.Port
				);
				return diagnostic.Code;
			}
			bool sourceArray = false;
			const auto *keyCatalogue = FindCatalogueEntry(document.Nodes[node->second].Type);
			const auto *keyInput = keyCatalogue ? FindCatalogueInput(*keyCatalogue, keyframe.Port) : nullptr;
			if (!keyInput && keyCatalogue) {
				size_t dynamicGroup = 0;
				keyInput = FindDynamicTemplate(*keyCatalogue, keyframe.Port, dynamicGroup);
			}
			const bool sourceEnum = keyInput && CatalogueSourceEnumValue(*keyInput, keyframe.Data);
			if (document.FormatVersion >= 8 && keyframe.Interpolation == "source") {
				const auto *catalogue = keyCatalogue;
				const auto *input = keyInput;
				if (input)
					if (const auto *array = std::get_if<ArrayValue>(&keyframe.Data))
						sourceArray = CatalogueAuthoredArray(*catalogue, *input, *array);
			}
			if (document.FormatVersion >= 9 && keyframe.Interpolation == "source" &&
				detail::SourceSeparatedVec2Input(document.Nodes[node->second], keyframe.Port) &&
				detail::SourceNumericVec2Tuple(keyframe.Data))
				sourceArray = true;
			const bool sourceEmptyKey =
				SourceEmptyGroupVectorKey(document, document.Nodes[node->second], keyInput, keyframe.Port);
			if (TypeOf(keyframe.Data) != *keyedType &&
				!(document.Nodes[node->second].Type == "pc.group_input" && keyframe.Port == "parent_value") &&
				!sourceArray && !sourceEnum && !sourceEmptyKey &&
				!(document.FormatVersion >= 8 && keyframe.Interpolation == "source" && keyInput &&
				  CatalogueSourceRawValue(*keyInput, keyframe.Data))) {
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
			if (keyframe.SourceDriver) {
				const auto *array = std::get_if<ArrayValue>(&keyframe.Data);
				const bool numeric =
					(sourceEnum && keyInput->SourceBehavior->FractionalInterpolation == true) ||
					std::holds_alternative<double>(keyframe.Data) ||
					std::holds_alternative<int64_t>(keyframe.Data) ||
					std::holds_alternative<Colour>(keyframe.Data) ||
					std::holds_alternative<Vector2>(keyframe.Data) ||
					std::holds_alternative<Vector3>(keyframe.Data) ||
					std::holds_alternative<Vector4>(keyframe.Data) ||
					std::holds_alternative<Quaternion>(keyframe.Data) ||
					std::holds_alternative<Area>(keyframe.Data) ||
					std::holds_alternative<Gradient>(keyframe.Data) ||
					(array && array->Nested.empty() &&
					 (array->ElementType == ValueType::Scalar || array->ElementType == ValueType::Integer));
				if (!numeric) {
					SetDiagnostic(
						diagnostic,
						Status::UnsupportedExecution,
						"source driver requires a numeric component domain",
						keyframe.NodeId,
						keyframe.Port
					);
					return diagnostic.Code;
				}
				if (keyframe.SineDriver || keyframe.Interpolation != "source" ||
					!detail::ValidSourceDriver(*keyframe.SourceDriver)) {
					SetDiagnostic(
						diagnostic,
						Status::InvalidValue,
						"source driver needs source easing, finite bounded "
						"controls and no sine driver",
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
						"sine keyframe driver parameters must be finite and "
						"smooth must be in [0, 1]",
						keyframe.NodeId,
						keyframe.Port
					);
					return diagnostic.Code;
				}
			}
			if (!keyframeIds
					 .emplace(
						 keyframe.NodeId,
						 keyframe.Port,
						 keyframe.NegativeFrame,
						 keyframe.Tick,
						 keyframe.Subframe
					 )
					 .second) {
				SetDiagnostic(
					diagnostic,
					Status::DuplicateId,
					"duplicate keyframe position",
					keyframe.NodeId,
					keyframe.Port
				);
				return diagnostic.Code;
			}
		}
		auto authoredTracks = detail::MakeEvaluationMap<
			std::pair<std::string_view, std::string_view>,
			detail::EvaluationVector<const Keyframe *>>(budget);
		for (const Keyframe &keyframe : document.Keyframes)
			authoredTracks
				.try_emplace(
					std::pair<std::string_view, std::string_view>{keyframe.NodeId, keyframe.Port},
					detail::EvaluationAllocator<const Keyframe *>(budget)
				)
				.first->second.push_back(&keyframe);
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
		auto trackIds = detail::MakeEvaluationSet<std::pair<std::string_view, std::string_view>>(budget);
		for (const AnimationTrack &track : document.Tracks) {
			const auto identity = std::pair<std::string_view, std::string_view>{track.NodeId, track.Port};
			if (!trackIds.insert(identity).second) {
				SetDiagnostic(
					diagnostic, Status::DuplicateId, "duplicate animation track", track.NodeId, track.Port
				);
				return diagnostic.Code;
			}
			const auto authored = authoredTracks.find(identity);
			const bool empty = authored == authoredTracks.end();
			const auto owner = nodeIndices.find(track.NodeId);
			const Node *node = owner == nodeIndices.end() ? nullptr : &document.Nodes[owner->second];
			if (node && IsNoisePortSelector(*node, track.Port)) {
				SetDiagnostic(
					diagnostic,
					Status::InvalidValue,
					"noise port selectors cannot be animated",
					track.NodeId,
					track.Port
				);
				return diagnostic.Code;
			}
			const bool sourceStatic =
				node &&
				std::find(node->SourceStaticInputs.begin(), node->SourceStaticInputs.end(), track.Port) !=
					node->SourceStaticInputs.end();
			const bool sourceAnimated =
				node &&
				std::find(node->SourceAnimatedInputs.begin(), node->SourceAnimatedInputs.end(), track.Port) !=
					node->SourceAnimatedInputs.end();
			const bool sourceEarlyReturn =
				sourceStatic ||
				(sourceAnimated && (empty || (authored->second.size() == 1 &&
											  authored->second.front()->Interpolation == "source")));
			const bool declaredEmpty =
				empty && node &&
				(std::find(
					 node->SourceAnimatedInputs.begin(), node->SourceAnimatedInputs.end(), track.Port
				 ) != node->SourceAnimatedInputs.end() ||
				 std::find(node->SourceStaticInputs.begin(), node->SourceStaticInputs.end(), track.Port) !=
					 node->SourceStaticInputs.end());
			if (empty && !declaredEmpty) {
				SetDiagnostic(
					diagnostic,
					Status::InvalidValue,
					"animation track has no keyframes",
					track.NodeId,
					track.Port
				);
				return diagnostic.Code;
			}
			if (track.QuaternionMode &&
				(*track.QuaternionMode < 0 || *track.QuaternionMode > 1 ||
				 (empty ? FindPortType(*node, track.Port, PortDirection::Input) != ValueType::Quaternion
						: std::any_of(
							  authored->second.begin(), authored->second.end(), [](const Keyframe *key) {
								  return !std::holds_alternative<Quaternion>(key->Data) ||
										 key->Interpolation != "source";
							  }
						  )))) {
				SetDiagnostic(
					diagnostic,
					Status::InvalidValue,
					"quaternion mode needs source quaternion keys and a raw or "
					"Euler choice",
					track.NodeId,
					track.Port
				);
				return diagnostic.Code;
			}
			if (track.NodeId.size() > Limits::MaximumTextBytes ||
				track.Port.size() > Limits::MaximumTextBytes || track.End.size() > Limits::MaximumTextBytes ||
				(track.End != "hold" && track.End != "loop" && track.End != "ping" && track.End != "wrap") ||
				(!sourceEarlyReturn && track.LoopRange < -1) ||
				(!empty && !sourceEarlyReturn &&
				 track.LoopRange >= static_cast<int64_t>(authored->second.size()))) {
				SetDiagnostic(
					diagnostic,
					Status::InvalidValue,
					"animation track has invalid end or loop range",
					track.NodeId,
					track.Port
				);
				return diagnostic.Code;
			}
			// Source static and zero/one-key animated getters return before end/range selection.
			if (!empty && !sourceEarlyReturn && track.End == "wrap" && !document.Timeline) {
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

		detail::EvaluationVector<size_t> indegree(
			document.Nodes.size(), 0, detail::EvaluationAllocator<size_t>(budget)
		);
		detail::EvaluationVector<detail::EvaluationVector<size_t>> downstream{
			detail::EvaluationAllocator<detail::EvaluationVector<size_t>>(budget)
		};
		downstream.reserve(document.Nodes.size());
		for (size_t index = 0; index < document.Nodes.size(); ++index)
			downstream.emplace_back(detail::EvaluationAllocator<size_t>(budget));
		auto uniqueLinks = detail::MakeEvaluationSet<
			std::tuple<std::string_view, std::string_view, std::string_view, std::string_view>>(budget);
		auto linkedInputs = detail::MakeEvaluationSet<std::pair<std::string_view, std::string_view>>(budget);
		auto junctionInputs = detail::MakeEvaluationHashMap<std::string_view, const Link *>(budget);
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
			const auto key =
				std::tuple<std::string_view, std::string_view, std::string_view, std::string_view>{
					link.FromNode, link.FromPort, link.ToNode, link.ToPort
				};
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
			const auto commonFromSelector = detail::CommonSelector(link.FromPort);
			const auto commonToSelector = detail::CommonSelector(link.ToPort);
			const auto *commonFrom =
				commonFromSelector ? detail::CommonOwner(document, link.FromNode) : nullptr;
			const auto *commonTo = commonToSelector ? detail::CommonOwner(document, link.ToNode) : nullptr;
			if ((commonFromSelector && !commonFrom) ||
				(commonToSelector && (!commonTo || *commonToSelector != SourceCommonSelector::Update))) {
				SetDiagnostic(
					diagnostic,
					Status::UnknownPort,
					"common route selector or owner is invalid",
					link.ToNode,
					link.ToPort
				);
				return diagnostic.Code;
			}
			if ((from == nodeIndices.end() && fromJunction == junctionIndices.end() && !commonFrom) ||
				(to == nodeIndices.end() && toJunction == junctionIndices.end() && !commonTo)) {
				SetDiagnostic(
					diagnostic,
					Status::InvalidOutput,
					"link refers to an unknown node or junction",
					link.FromNode,
					link.FromPort
				);
				return diagnostic.Code;
			}
			const std::string_view sourceGroup = commonFrom ? detail::CommonOwnerScope(document, *commonFrom)
												 : from != nodeIndices.end()
													 ? document.Nodes[from->second].GroupId
													 : document.Junctions[fromJunction->second].GroupId;
			const std::string_view targetGroup = commonTo ? detail::CommonOwnerScope(document, *commonTo)
												 : to != nodeIndices.end()
													 ? document.Nodes[to->second].GroupId
													 : document.Junctions[toJunction->second].GroupId;
			const auto transparentScope = [&](std::string_view scope) {
				for (size_t depth = 0; !scope.empty() && depth < document.Groups.size(); ++depth) {
					const auto group =
						std::find_if(document.Groups.begin(), document.Groups.end(), [&](const Group &item) {
							return item.Id == scope;
						});
					if (group == document.Groups.end() || group->OwnerNodeId.empty()) break;
					scope = group->ParentId;
				}
				return scope;
			};
			const auto sourceScope = transparentScope(sourceGroup),
					   targetScope = transparentScope(targetGroup);
			if (sourceScope != targetScope && document.FormatVersion >= 2) {
				const auto sourceOwner =
					std::find_if(document.Groups.begin(), document.Groups.end(), [&](const Group &group) {
						return group.Id == sourceScope;
					});
				const auto targetOwner =
					std::find_if(document.Groups.begin(), document.Groups.end(), [&](const Group &group) {
						return group.Id == targetScope;
					});
				const bool declaredBoundary =
					(sourceOwner != document.Groups.end() && !sourceOwner->Ports.empty()) ||
					(targetOwner != document.Groups.end() && !targetOwner->Ports.empty());
				if (declaredBoundary) {
					const bool leavesChild =
						sourceOwner != document.Groups.end() &&
						transparentScope(sourceOwner->ParentId) == targetScope &&
						fromJunction != junctionIndices.end() &&
						std::any_of(
							sourceOwner->Ports.begin(), sourceOwner->Ports.end(), [&](const GroupPort &port) {
								return port.JunctionId == link.FromNode &&
									   port.Direction == PortDirection::Output;
							}
						);
					const bool entersChild =
						targetOwner != document.Groups.end() &&
						transparentScope(targetOwner->ParentId) == sourceScope &&
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
			if (commonFrom)
				source = detail::CommonSelectorType(*commonFromSelector);
			else if (from != nodeIndices.end())
				source = FindPortType(
					document.Nodes[from->second], link.FromPort, PortDirection::Output, &document
				);
			else if (link.FromPort == "value")
				source = document.Junctions[fromJunction->second].Type;
			std::optional<ValueType> target;
			if (commonTo)
				target = ValueType::Any;
			else if (to != nodeIndices.end())
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
			const bool sourceFontInput =
				to != nodeIndices.end() && targetType == ValueType::Text &&
				detail::SourceFontInput(document.Nodes[to->second].Type, link.ToPort) &&
				(sourceType == ValueType::Font || sourceType == ValueType::Array);
			const bool arrayElement = from != nodeIndices.end() && to != nodeIndices.end() &&
									  sourceType == ValueType::Array && targetType == ValueType::Image &&
									  document.Nodes[to->second].Type == "value.array";
			const bool heightBlendArrayInput = to != nodeIndices.end() && sourceType == ValueType::Array &&
											   targetType == ValueType::Image &&
											   document.Nodes[to->second].Type == "image.height_blend";
			const bool heightBlendArrayOutput = from != nodeIndices.end() && sourceType == ValueType::Image &&
												targetType == ValueType::Array &&
												document.Nodes[from->second].Type == "image.height_blend";
			bool catalogueArrayInput = false;
			if (to != nodeIndices.end() && sourceType == ValueType::Array && targetType == ValueType::Image) {
				if (const CatalogueEntry *entry = FindCatalogueEntry(document.Nodes[to->second].Type)) {
					const CatalogueInput *input = FindCatalogueInput(*entry, link.ToPort);
					catalogueArrayInput =
						input && input->SourceIndex >= 0 && input->ArrayDepthKnown &&
						input->Type == ValueType::Image &&
						(FindCatalogueInput(*entry, "attribute_process") ||
						 (entry->Type == "pc.flip_render" && input->Id == "fluid_particle") ||
						 (entry->Type == "pc.rigid_object" && input->Id == "texture") ||
						 (entry->Type == "pc.crop_content" && input->Id == "surface_in") ||
						 ((entry->Type == "pc.cache" || entry->Type == "pc.cache_array") &&
						  input->Id == "surface_in") ||
						 (entry->Type == "pc.font_bitmap" && input->Id == "font_surfaces" &&
						  input->ArrayDepth == 1) ||
						 (entry->Type == "pc.sequence_anim" && input->Id == "surface_in" &&
						  input->ArrayDepth == 1));
				}
			}
			const bool catalogueStrandArrayInput =
				to != nodeIndices.end() && sourceType == ValueType::Array &&
				targetType == ValueType::Strand && link.ToPort == "input_0" &&
				(document.Nodes[to->second].Type == "pc.strand_gravity" ||
				 document.Nodes[to->second].Type == "pc.strand_update");
			bool catalogueAtlasArrayInput = false;
			if (to != nodeIndices.end() && sourceType == ValueType::Array && targetType == ValueType::Atlas) {
				if (const auto *entry = FindCatalogueEntry(document.Nodes[to->second].Type)) {
					const auto *input = FindCatalogueInput(*entry, link.ToPort);
					const bool atlasPort = ((entry->Type == "pc.atlas_get" || entry->Type == "pc.atlas_set" ||
											 entry->Type == "pc.atlas_struct") &&
											link.ToPort == "input_0") ||
										   (entry->Type == "pc.atlas_draw" && link.ToPort == "input_1") ||
										   (entry->Type == "pc.atlas_affector" &&
											(link.ToPort == "atlas_in" || link.ToPort == "target_atlas"));
					catalogueAtlasArrayInput =
						atlasPort && input && input->SourceIndex >= 0 && input->Type == ValueType::Atlas &&
						input->SourceKind == "Atlas" && FindCatalogueInput(*entry, "attribute_process");
				}
			}
			bool sourceMaterialInput = false;
			if (to != nodeIndices.end() && targetType == ValueType::Material3D &&
				(sourceType == ValueType::Image || sourceType == ValueType::Array)) {
				if (const auto *entry = FindCatalogueEntry(document.Nodes[to->second].Type)) {
					const auto *input = FindCatalogueInput(*entry, link.ToPort);
					sourceMaterialInput = input && input->SourceIndex >= 0 &&
										  input->Type == ValueType::Material3D &&
										  input->SourceKind == "D3Material";
				}
			}
			bool heightmapColourArrayInput = false;
			if (from != nodeIndices.end() && to != nodeIndices.end() && sourceType == ValueType::Array &&
				targetType == ValueType::Gradient &&
				document.Nodes[to->second].Type == "pc.heightmap_project_3_d" &&
				link.ToPort == "height_color" && link.FromPort == "colors" &&
				(document.Nodes[from->second].Type == "pc.gradient_sample" ||
				 document.Nodes[from->second].Type == "pc.gradient_extract")) {
				const auto *entry = FindCatalogueEntry(document.Nodes[to->second].Type);
				const auto *input = entry ? FindCatalogueInput(*entry, link.ToPort) : nullptr;
				const auto *sourceEntry = FindCatalogueEntry(document.Nodes[from->second].Type);
				const bool sourceColourPalette =
					sourceEntry &&
					(sourceEntry->SourceNode == "Node_Gradient_Sample" ||
					 sourceEntry->SourceNode == "Node_Gradient_Extract") &&
					std::any_of(
						sourceEntry->Outputs.begin(), sourceEntry->Outputs.end(), [](const auto &output) {
							return output.Id == "colors" && output.SourceIndex == 0 &&
								   output.Type == ValueType::Array;
						}
					);
				// Source palettes retain a Colour domain for the Gradient getter.
				heightmapColourArrayInput = sourceColourPalette && input &&
											input->Type == ValueType::Gradient &&
											input->SourceKind == "Gradient";
			}
			const auto dynamicBoundary = [&](std::string_view junctionId) {
				return std::any_of(document.Groups.begin(), document.Groups.end(), [&](const Group &group) {
					return std::any_of(group.Ports.begin(), group.Ports.end(), [&](const GroupPort &port) {
						return !port.ControlNodeId.empty() && port.JunctionId == junctionId;
					});
				});
			};
			// Any junctions retain consumer-resolved transport when a source group is ungrouped.
			const bool boundaryLink = (fromJunction != junctionIndices.end() &&
									   (document.Junctions[fromJunction->second].Type == ValueType::Any ||
										dynamicBoundary(link.FromNode))) ||
									  (toJunction != junctionIndices.end() &&
									   (document.Junctions[toJunction->second].Type == ValueType::Any ||
										dynamicBoundary(link.ToNode)));
			const bool catalogueLink =
				(from != nodeIndices.end() && FindCatalogueEntry(document.Nodes[from->second].Type)) ||
				(to != nodeIndices.end() && FindCatalogueEntry(document.Nodes[to->second].Type));
			const bool opaqueCallbackLink =
				(from != nodeIndices.end() && detail::IsGroupCallbackOpaque(document.Nodes[from->second])) ||
				(to != nodeIndices.end() && detail::IsGroupCallbackOpaque(document.Nodes[to->second]));
			const PortSchema *inputSchema =
				to != nodeIndices.end()
					? FindPort(document.Nodes[to->second].Type, link.ToPort, PortDirection::Input)
					: nullptr;
			const PortSchema *outputSchema =
				from != nodeIndices.end()
					? FindPort(document.Nodes[from->second].Type, link.FromPort, PortDirection::Output)
					: nullptr;
			std::optional<PortSchema> noiseInput, noiseOutput;
			if (to != nodeIndices.end()) {
				const auto &node = document.Nodes[to->second];
				if (node.Type == "value.noise_field" || node.Type == "value.sample_noise") {
					noiseInput = NoiseNodePort(node, link.ToPort, PortDirection::Input);
					inputSchema = noiseInput ? &*noiseInput : nullptr;
				}
			}
			if (from != nodeIndices.end()) {
				const auto &node = document.Nodes[from->second];
				if (node.Type == "value.noise_field" || node.Type == "value.sample_noise" ||
					(IsNoiseImageGenerator(node.Type) && link.FromPort == "field")) {
					noiseOutput = NoiseNodePort(node, link.FromPort, PortDirection::Output, &document);
					outputSchema = noiseOutput ? &*noiseOutput : nullptr;
				}
			}
			if (to != nodeIndices.end() && document.Nodes[to->second].Type == "value.noise_field" &&
				link.ToPort == "position" && noiseInput && sourceType != targetType) {
				SetDiagnostic(
					diagnostic,
					Status::TypeMismatch,
					"noise coordinates must match the selected dimension",
					link.ToNode,
					link.ToPort
				);
				return diagnostic.Code;
			}
			const bool unionLink = (inputSchema && !inputSchema->Alternatives.empty()) ||
								   (outputSchema && !outputSchema->Alternatives.empty());
			const auto accepts = [&](ValueType type) {
				if (!inputSchema || inputSchema->Alternatives.empty()) return type == targetType;
				return std::find(inputSchema->Alternatives.begin(), inputSchema->Alternatives.end(), type) !=
					   inputSchema->Alternatives.end();
			};
			const bool explicitUnion =
				unionLink &&
				(outputSchema && !outputSchema->Alternatives.empty()
					 ? std::all_of(
						   outputSchema->Alternatives.begin(), outputSchema->Alternatives.end(), accepts
					   )
					 : accepts(sourceType));
			if (unionLink && !explicitUnion) {
				SetDiagnostic(
					diagnostic,
					Status::TypeMismatch,
					"link union includes incompatible port types",
					link.ToNode,
					link.ToPort
				);
				return diagnostic.Code;
			}
			if (sourceType != targetType && !explicitUnion && !opaqueCallbackLink && !arrayElement &&
				!heightBlendArrayInput && !heightBlendArrayOutput && !catalogueArrayInput &&
				!catalogueAtlasArrayInput && !catalogueStrandArrayInput && !sourceMaterialInput &&
				!heightmapColourArrayInput && !sourceFontInput &&
				!((catalogueLink || boundaryLink || commonFrom || commonTo) &&
				  CatalogueJunctionCompatible(sourceType, targetType))) {
				SetDiagnostic(
					diagnostic,
					Status::TypeMismatch,
					"link connects incompatible port types",
					link.ToNode,
					link.ToPort
				);
				return diagnostic.Code;
			}
			const auto inputKey = std::pair<std::string_view, std::string_view>{link.ToNode, link.ToPort};
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
		const uint64_t planStorageBytes =
			(document.Nodes.size() + document.Outputs.size()) * sizeof(size_t) +
			document.Links.size() * (sizeof(Link) + sizeof(ResolvedInput) + sizeof(SourceCommonRoute));
		auto storageCharge = budget.Reserve(planStorageBytes);
		if (!storageCharge || !planCharge.Merge(std::move(*storageCharge))) {
			SetDiagnostic(
				diagnostic, Status::LimitExceeded, "compile result storage exceeds the live byte budget"
			);
			return diagnostic.Code;
		}
		std::vector<Link> effectiveLinks;
		std::vector<ResolvedInput> resolvedInputs;
		std::vector<SourceCommonRoute> commonRoutes;
		commonRoutes.reserve(document.Links.size());
		effectiveLinks.reserve(document.Links.size());
		resolvedInputs.reserve(document.Links.size());
		for (const Link &link : document.Links) {
			if (!nodeIndices.contains(link.ToNode) &&
				!(detail::CommonSelector(link.ToPort) == SourceCommonSelector::Update &&
				  detail::CommonOwner(document, link.ToNode)))
				continue;
			std::string_view sourceNode = link.FromNode;
			std::string_view sourcePort = link.FromPort;
			auto visited = detail::MakeEvaluationHashSet<std::string_view>(budget);
			bool usedDefault = false;
			while (junctionIndices.contains(sourceNode)) {
				if (!visited.insert(document.Junctions[junctionIndices.at(sourceNode)].Id).second) {
					SetDiagnostic(
						diagnostic, Status::Cycle, "junction routing contains a cycle", sourceNode, "value"
					);
					return diagnostic.Code;
				}
				const auto input = junctionInputs.find(sourceNode);
				if (input == junctionInputs.end()) {
					const Junction &junction = document.Junctions[junctionIndices.at(sourceNode)];
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
					auto defaultCharge = budget.Reserve(
						detail::RetainedPayloadBytes(*junction.Default) +
						std::max(link.ToNode.size(), std::string{}.capacity()) +
						std::max(link.ToPort.size(), std::string{}.capacity())
					);
					if (!defaultCharge || !planCharge.Merge(std::move(*defaultCharge))) {
						SetDiagnostic(
							diagnostic,
							Status::LimitExceeded,
							"compile default clone exceeds the live byte budget",
							link.ToNode,
							link.ToPort
						);
						return diagnostic.Code;
					}
					resolvedInputs.push_back({link.ToNode, link.ToPort, *junction.Default});
					usedDefault = true;
					break;
				}
				sourceNode = input->second->FromNode;
				sourcePort = input->second->FromPort;
			}
			if (!usedDefault) {
				uint64_t nameBytes = std::max(sourceNode.size(), std::string{}.capacity()) +
									 std::max(sourcePort.size(), std::string{}.capacity()) +
									 std::max(link.ToNode.size(), std::string{}.capacity()) +
									 std::max(link.ToPort.size(), std::string{}.capacity());
				if ((detail::CommonSelector(sourcePort) && detail::CommonOwner(document, sourceNode)) ||
					detail::CommonSelector(link.ToPort) == SourceCommonSelector::Update)
					if (!AddBytes(nameBytes, std::max(sourcePort.size(), std::string{}.capacity()))) {
						SetDiagnostic(diagnostic, Status::LimitExceeded, "common route name size overflows");
						return diagnostic.Code;
					}
				auto routeCharge = budget.Reserve(nameBytes);
				if (!routeCharge || !planCharge.Merge(std::move(*routeCharge))) {
					SetDiagnostic(
						diagnostic,
						Status::LimitExceeded,
						"compile route names exceed the live byte budget",
						link.ToNode,
						link.ToPort
					);
					return diagnostic.Code;
				}
				const auto selector = detail::CommonSelector(sourcePort);
				const auto *commonOwner = selector ? detail::CommonOwner(document, sourceNode) : nullptr;
				const bool updateDestination =
					detail::CommonSelector(link.ToPort) == SourceCommonSelector::Update;
				if (commonOwner || updateDestination) {
					commonRoutes.push_back(
						{commonOwner ? commonOwner->SourceOwnerId : std::string(sourceNode),
						 selector.value_or(SourceCommonSelector::HeldOutput),
						 link.ToNode,
						 link.ToPort,
						 updateDestination,
						 std::string(sourcePort)}
					);
				} else
					effectiveLinks.push_back(
						{std::string(sourceNode), std::string(sourcePort), link.ToNode, link.ToPort}
					);
			}
		}
		// An unconnected source instance reads the nearest base input, including its link.
		// Publish that route in the plan so ordinary DAG retention and ordering cover it too.
		const size_t authoredRoutes = effectiveLinks.size(), authoredDefaults = resolvedInputs.size();
		uint64_t instanceRouteWork = 0;
		const auto inputRouteOwner = [&](const Node &start, std::string_view port) -> const Node * {
			const Node *current = &start;
			for (size_t hop = 0; hop < document.Nodes.size(); ++hop) {
				const uint64_t hopWork = current->InstanceOverrides.size() + 1;
				if (hopWork > 64'000'000 - instanceRouteWork) return nullptr;
				instanceRouteWork += hopWork;
				if (linkedInputs.contains({current->Id, port}) ||
					std::find(current->InstanceOverrides.begin(), current->InstanceOverrides.end(), port) !=
						current->InstanceOverrides.end() ||
					detail::SourceInputInstanceBase(*current, port).empty())
					return current;
				const auto found = nodeIndices.find(detail::SourceInputInstanceBase(*current, port));
				if (found == nodeIndices.end()) return nullptr;
				current = &document.Nodes[found->second];
			}
			return nullptr;
		};
		size_t inheritedRoutes = 0, inheritedDefaults = 0;
		uint64_t inheritedBytes = 0;
		const auto visitInherited = [&](bool publish) -> bool {
			for (const Node &node : document.Nodes) {
				if (node.InstanceBase.empty() && node.SourceParentInputBase.empty()) continue;
				for (size_t routeIndex = 0; routeIndex < authoredRoutes + authoredDefaults; ++routeIndex) {
					const bool isDefault = routeIndex >= authoredRoutes;
					const size_t sourceIndex = isDefault ? routeIndex - authoredRoutes : routeIndex;
					const std::string_view port = isDefault
													  ? std::string_view(resolvedInputs[sourceIndex].Port)
													  : std::string_view(effectiveLinks[sourceIndex].ToPort);
					const std::string_view sourceOwner =
						isDefault ? std::string_view(resolvedInputs[sourceIndex].NodeId)
								  : std::string_view(effectiveLinks[sourceIndex].ToNode);
					const Node *owner = inputRouteOwner(node, port);
					if (!owner) {
						SetDiagnostic(
							diagnostic,
							Status::LimitExceeded,
							"instance input route resolution exceeds bounded work",
							node.Id,
							port
						);
						return false;
					}
					if (owner->Id != sourceOwner || owner->Id == node.Id) continue;
					if (!publish) {
						if (isDefault) {
							++inheritedDefaults;
							inheritedBytes += detail::RetainedPayloadBytes(resolvedInputs[sourceIndex].Data) +
											  std::max(node.Id.size(), std::string{}.capacity()) +
											  std::max(port.size(), std::string{}.capacity());
						} else {
							++inheritedRoutes;
							const Link &source = effectiveLinks[sourceIndex];
							inheritedBytes += std::max(source.FromNode.size(), std::string{}.capacity()) +
											  std::max(source.FromPort.size(), std::string{}.capacity()) +
											  std::max(node.Id.size(), std::string{}.capacity()) +
											  std::max(port.size(), std::string{}.capacity());
						}
						if (inheritedRoutes + inheritedDefaults >
							Limits::MaximumLinks - document.Links.size()) {
							SetDiagnostic(
								diagnostic,
								Status::LimitExceeded,
								"instance input routes exceed native link bound",
								node.Id,
								port
							);
							return false;
						}
					} else if (isDefault) {
						const auto &source = resolvedInputs[sourceIndex];
						resolvedInputs.push_back({node.Id, source.Port, source.Data});
					} else {
						const auto &source = effectiveLinks[sourceIndex];
						effectiveLinks.push_back({source.FromNode, source.FromPort, node.Id, source.ToPort});
					}
				}
			}
			return true;
		};
		if (!visitInherited(false)) return diagnostic.Code;
		if (inheritedRoutes || inheritedDefaults) {
			// reserve() temporarily retains the old allocation while constructing its replacement.
			inheritedBytes += (authoredRoutes + inheritedRoutes) * sizeof(Link) +
							  (authoredDefaults + inheritedDefaults) * sizeof(ResolvedInput);
			auto inheritedCharge = budget.Reserve(inheritedBytes);
			if (!inheritedCharge || !planCharge.Merge(std::move(*inheritedCharge))) {
				SetDiagnostic(
					diagnostic,
					Status::LimitExceeded,
					"instance route clones exceed the live compile byte budget"
				);
				return diagnostic.Code;
			}
			effectiveLinks.reserve(authoredRoutes + inheritedRoutes);
			resolvedInputs.reserve(authoredDefaults + inheritedDefaults);
			if (!visitInherited(true)) return diagnostic.Code;
			// Final vectors own these names; nearest-owner resolution above used authored links only.
			for (size_t i = authoredRoutes; i < effectiveLinks.size(); ++i)
				linkedInputs.insert({effectiveLinks[i].ToNode, effectiveLinks[i].ToPort});
			for (size_t i = authoredDefaults; i < resolvedInputs.size(); ++i)
				linkedInputs.insert({resolvedInputs[i].NodeId, resolvedInputs[i].Port});
		}
		for (const Junction &junction : document.Junctions) {
			auto visited = detail::MakeEvaluationHashSet<std::string_view>(budget);
			std::string_view current = junction.Id;
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
		uint64_t hlslDeclarationWork = 0;
		for (const auto &node : document.Nodes) {
			if (node.Type != "pc.hlsl") continue;
			for (const auto &input : node.DynamicInputs) {
				if (!input.Id.starts_with("argument_value_")) continue;
				const std::string selector =
					"argument_type_" + input.Id.substr(std::string_view("argument_value_").size());
				const Node *ownerPointer = inputRouteOwner(node, selector);
				if (!ownerPointer) {
					SetDiagnostic(
						diagnostic,
						Status::LimitExceeded,
						"HLSL selector owner resolution exceeds bounded work",
						node.Id,
						selector
					);
					return diagnostic.Code;
				}
				const Node &owner = *ownerPointer;
				const uint64_t work = effectiveLinks.size() + resolvedInputs.size() +
									  document.Keyframes.size() + document.Nodes.size() +
									  owner.SourceInputExpressions.size() + owner.SourceStaticInputs.size();
				if (work > 64'000'000 - hlslDeclarationWork) {
					SetDiagnostic(
						diagnostic,
						Status::LimitExceeded,
						"HLSL declaration validation exceeds work bound",
						node.Id,
						selector
					);
					return diagnostic.Code;
				}
				hlslDeclarationWork += work;
				const auto literal =
					std::find_if(resolvedInputs.begin(), resolvedInputs.end(), [&](const auto &value) {
						return value.NodeId == node.Id && value.Port == selector;
					});
				const Value *resolved = literal == resolvedInputs.end() ? nullptr : &literal->Data;
				const bool linked =
					std::any_of(effectiveLinks.begin(), effectiveLinks.end(), [&](const auto &link) {
						return link.ToNode == node.Id && link.ToPort == selector;
					});
				const bool staticSelector =
					std::find(owner.SourceStaticInputs.begin(), owner.SourceStaticInputs.end(), selector) !=
					owner.SourceStaticInputs.end();
				const bool animated =
					!resolved && !staticSelector &&
					std::any_of(document.Keyframes.begin(), document.Keyframes.end(), [&](const auto &key) {
						return key.NodeId == owner.Id && key.Port == selector;
					});
				const bool expression =
					!resolved &&
					std::any_of(
						owner.SourceInputExpressions.begin(),
						owner.SourceInputExpressions.end(),
						[&](const auto &program) { return program.Enabled && program.Port == selector; }
					);
				if (linked || animated || expression) {
					SetDiagnostic(
						diagnostic,
						Status::UnsupportedExecution,
						"dynamic HLSL argument type requires a matching native cooked schema",
						node.Id,
						selector
					);
					return diagnostic.Code;
				}
				const auto type = detail::SourceArgumentType(owner, input.Id, resolved);
				if (!type || *type != input.Type) {
					SetDiagnostic(
						diagnostic,
						Status::TypeMismatch,
						"HLSL argument declaration differs from its resolved type selector",
						node.Id,
						input.Id
					);
					return diagnostic.Code;
				}
				const Node *valueOwner = inputRouteOwner(node, input.Id);
				if (!valueOwner) {
					SetDiagnostic(
						diagnostic,
						Status::LimitExceeded,
						"HLSL value owner resolution exceeds bounded work",
						node.Id,
						input.Id
					);
					return diagnostic.Code;
				}
				if (input.Default && valueOwner == &node &&
					!detail::SourceHlslArgumentValue(owner, input.Id, *input.Default, resolved)) {
					SetDiagnostic(
						diagnostic,
						Status::TypeMismatch,
						"HLSL argument value does not match its declared source shape",
						node.Id,
						input.Id
					);
					return diagnostic.Code;
				}
				for (const auto &key : document.Keyframes) {
					if (key.NodeId != valueOwner->Id || key.Port != input.Id) continue;
					if (!detail::SourceHlslArgumentValue(owner, input.Id, key.Data, resolved)) {
						SetDiagnostic(
							diagnostic,
							Status::TypeMismatch,
							"HLSL argument key does not match its declared source shape",
							node.Id,
							input.Id
						);
						return diagnostic.Code;
					}
				}
			}
		}
		for (const auto &node : document.Nodes) {
			if (node.Type != "pc.directory_search") continue;
			const auto *owner = EffectiveInputOwner(document, node, "type");
			if (!owner) owner = &node;
			const bool staticMode =
				std::find(owner->SourceStaticInputs.begin(), owner->SourceStaticInputs.end(), "type") !=
				owner->SourceStaticInputs.end();
			const bool changing =
				std::any_of(
					resolvedInputs.begin(),
					resolvedInputs.end(),
					[&](const ResolvedInput &input) {
						return input.NodeId == owner->Id && input.Port == "type";
					}
				) ||
				std::any_of(
					effectiveLinks.begin(),
					effectiveLinks.end(),
					[&](const Link &link) { return link.ToNode == owner->Id && link.ToPort == "type"; }
				) ||
				std::find(owner->SourceAnimatedInputs.begin(), owner->SourceAnimatedInputs.end(), "type") !=
					owner->SourceAnimatedInputs.end() ||
				(!staticMode &&
				 std::any_of(
					 document.Keyframes.begin(),
					 document.Keyframes.end(),
					 [&](const Keyframe &key) { return key.NodeId == owner->Id && key.Port == "type"; }
				 )) ||
				std::any_of(
					owner->SourceInputExpressions.begin(),
					owner->SourceInputExpressions.end(),
					[](const SourceInputExpression &expression) { return expression.Port == "type"; }
				);
			if (changing) {
				SetDiagnostic(
					diagnostic,
					Status::UnsupportedExecution,
					"Directory Search output typing requires a static authored type selector",
					node.Id,
					"type"
				);
				return diagnostic.Code;
			}
		}
		std::vector<InlineOwnerDependency> inlineDependencies;
		std::vector<InlineControlDependency> inlineControlDependencies;
		const bool hasInlineScopes = std::any_of(
										 document.Groups.begin(),
										 document.Groups.end(),
										 [](const Group &group) { return !group.OwnerNodeId.empty(); }
									 ) ||
									 !plan.PcxNamedDependencies.empty();
		auto inlineCharge = budget.Reserve(
			hasInlineScopes ? document.Nodes.size() * (sizeof(InlineOwnerDependency) + sizeof(size_t)) : 0
		);
		if (!inlineCharge || !planCharge.Merge(std::move(*inlineCharge))) {
			SetDiagnostic(
				diagnostic, Status::LimitExceeded, "inline owner dependencies exceed evaluation budget"
			);
			return diagnostic.Code;
		}
		inlineDependencies.reserve(hasInlineScopes ? document.Nodes.size() : 0);
		auto controlsCharge = budget.Reserve(
			hasInlineScopes ? document.Nodes.size() * 4 * (sizeof(InlineControlDependency) + sizeof(size_t))
							: 0
		);
		if (!controlsCharge || !planCharge.Merge(std::move(*controlsCharge))) {
			SetDiagnostic(
				diagnostic, Status::LimitExceeded, "collection control routes exceed evaluation budget"
			);
			return diagnostic.Code;
		}
		inlineControlDependencies.reserve(hasInlineScopes ? document.Nodes.size() * 4 : 0);
		const auto addControlRoute = [&](size_t consumer, size_t producer) {
			const InlineControlDependency route{consumer, producer};
			if (std::find(inlineControlDependencies.begin(), inlineControlDependencies.end(), route) !=
				inlineControlDependencies.end())
				return;
			inlineControlDependencies.push_back(route);
			indegree[consumer]++;
			downstream[producer].push_back(consumer);
		};
		for (size_t index = 0; index < document.Nodes.size(); ++index) {
			std::string_view scope = document.Nodes[index].GroupId;
			for (size_t depth = 0; !scope.empty() && depth < document.Groups.size(); ++depth) {
				const auto group =
					std::find_if(document.Groups.begin(), document.Groups.end(), [&](const Group &item) {
						return item.Id == scope;
					});
				if (group == document.Groups.end()) break;
				if (!group->OwnerNodeId.empty()) {
					const size_t owner = nodeIndices.at(group->OwnerNodeId);
					const bool builder = document.Nodes[owner].Type == "pc.pixel_builder";
					inlineDependencies.push_back({index, owner, builder});
					if (!builder) {
						indegree[index]++;
						downstream[owner].push_back(index);
					} else {
						for (const Link &link : effectiveLinks)
							if (link.ToNode == group->OwnerNodeId &&
								(link.ToPort == "dimension" || link.ToPort == "dimension_unit"))
								addControlRoute(index, nodeIndices.at(link.FromNode));
						if (document.Nodes[index].Type == "pc.pb_output") {
							addControlRoute(owner, index);
							for (const Link &link : effectiveLinks)
								if (link.ToNode == document.Nodes[index].Id && link.ToPort == "surface")
									addControlRoute(owner, nodeIndices.at(link.FromNode));
						}
					}
					break;
				}
				scope = group->ParentId;
			}
		}
		const auto verletStep = [&](const InlineOwnerDependency &route) {
			const auto &type = document.Nodes[route.Consumer].Type;
			return document.Nodes[route.Owner].Type == "pc.verlet_sim_inline" &&
				   (type == "pc.verlet_sim_step" || type == "pc.verlet_sim_render");
		};
		size_t colliderRoutes = 0;
		for (size_t index = 0; index < inlineDependencies.size(); ++index) {
			const auto &step = inlineDependencies[index];
			if (!verletStep(step)) continue;
			const size_t count =
				std::count_if(inlineDependencies.begin(), inlineDependencies.end(), [&](const auto &route) {
					return route.Owner == step.Owner &&
						   document.Nodes[route.Consumer].Type == "pc.verlet_sim_collide";
				});
			const bool first = std::none_of(
				inlineDependencies.begin(), inlineDependencies.begin() + index, [&](const auto &route) {
					return route.Owner == step.Owner && verletStep(route);
				}
			);
			const size_t additional = count + (first && count ? count - 1 : 0);
			if (additional > Limits::MaximumLinks - colliderRoutes) {
				SetDiagnostic(
					diagnostic,
					Status::LimitExceeded,
					"Verlet collider dependency graph exceeds native link bound",
					document.Nodes[step.Consumer].Id,
					"mesh"
				);
				return diagnostic.Code;
			}
			colliderRoutes += additional;
		}
		if (colliderRoutes) {
			const size_t ownerEdges =
				std::count_if(inlineDependencies.begin(), inlineDependencies.end(), [](const auto &route) {
					return !route.ControlsOnly;
				});
			const size_t existingEdges =
				effectiveLinks.size() + ownerEdges + inlineControlDependencies.size();
			if (existingEdges > Limits::MaximumLinks ||
				colliderRoutes > Limits::MaximumLinks - existingEdges) {
				SetDiagnostic(
					diagnostic,
					Status::LimitExceeded,
					"Verlet collider dependency graph exceeds native link bound"
				);
				return diagnostic.Code;
			}
			auto additionsCharge = budget.Reserve(document.Nodes.size() * sizeof(size_t));
			if (!additionsCharge) {
				SetDiagnostic(
					diagnostic,
					Status::LimitExceeded,
					"collider dependency workspace exceeds evaluation budget"
				);
				return diagnostic.Code;
			}
			std::vector<size_t> additions(document.Nodes.size());
			const auto visitColliderRoutes = [&](auto append) {
				for (size_t index = 0; index < inlineDependencies.size(); ++index) {
					const auto &step = inlineDependencies[index];
					if (!verletStep(step)) continue;
					const bool first = std::none_of(
						inlineDependencies.begin(),
						inlineDependencies.begin() + index,
						[&](const auto &route) { return route.Owner == step.Owner && verletStep(route); }
					);
					std::optional<size_t> previous;
					for (const auto &route : inlineDependencies) {
						if (route.Owner != step.Owner ||
							document.Nodes[route.Consumer].Type != "pc.verlet_sim_collide")
							continue;
						append(step.Consumer, route.Consumer);
						if (first && previous) append(route.Consumer, *previous);
						previous = route.Consumer;
					}
				}
			};
			visitColliderRoutes([&](size_t, size_t producer) { ++additions[producer]; });
			const size_t required = inlineControlDependencies.size() + colliderRoutes;
			if (required > inlineControlDependencies.capacity()) {
				auto charge = budget.Reserve(required * sizeof(InlineControlDependency));
				if (!charge || !planCharge.Merge(std::move(*charge))) {
					SetDiagnostic(
						diagnostic, Status::LimitExceeded, "collider control routes exceed evaluation budget"
					);
					return diagnostic.Code;
				}
				inlineControlDependencies.reserve(required);
			}
			for (size_t producer = 0; producer < additions.size(); ++producer) {
				const size_t required = downstream[producer].size() + additions[producer];
				if (!additions[producer] || required <= downstream[producer].capacity()) continue;
				auto charge = budget.Reserve(required * sizeof(size_t));
				if (!charge || !planCharge.Merge(std::move(*charge))) {
					SetDiagnostic(
						diagnostic, Status::LimitExceeded, "collider dependency rows exceed evaluation budget"
					);
					return diagnostic.Code;
				}
				downstream[producer].reserve(required);
			}
			visitColliderRoutes(addControlRoute);
		}
		std::vector<GroupSurfaceDependency> surfaceDependencies;
		auto surfaceCharge = budget.Reserve(document.Nodes.size() * sizeof(GroupSurfaceDependency));
		if (!surfaceCharge || !planCharge.Merge(std::move(*surfaceCharge))) {
			SetDiagnostic(
				diagnostic,
				Status::LimitExceeded,
				"group format dependency storage exceeds the live byte budget"
			);
			return diagnostic.Code;
		}
		surfaceDependencies.reserve(document.Nodes.size());
		for (size_t index = 0; index < document.Nodes.size(); ++index) {
			const Node *attributes = &document.Nodes[index];
			for (size_t hop = 0; !attributes->InstanceBase.empty() && hop < document.Nodes.size(); ++hop)
				attributes = &document.Nodes[nodeIndices.at(attributes->InstanceBase)];
			bool inherited = false;
			if (const auto *entry = FindCatalogueEntry(attributes->Type)) {
				if (const auto *depth = FindCatalogueInput(*entry, "attribute_color_depth")) {
					const auto *authored = FindValue(*attributes, depth->Id);
					const auto fallback = CatalogueDefault(*depth);
					const Value *choice = authored ? &authored->Data : (fallback ? &*fallback : nullptr);
					inherited =
						choice && (std::get_if<EnumValue>(choice)
									   ? std::get<EnumValue>(*choice).Value == 1
									   : std::get_if<int64_t>(choice) && std::get<int64_t>(*choice) == 1);
					if (std::any_of(effectiveLinks.begin(), effectiveLinks.end(), [&](const Link &link) {
							return link.ToNode == attributes->Id && link.ToPort == depth->Id;
						})) {
						SetDiagnostic(
							diagnostic,
							Status::UnsupportedExecution,
							"linked native depth requires conditional group "
							"dependency scheduling",
							attributes->Id,
							depth->Id
						);
						return diagnostic.Code;
					}
				}
			}
			if (!inherited) continue;
			const auto route = detail::FindGroupInputDepth(document, attributes->GroupId);
			if (route.Source == detail::GroupInputDepth::Kind::Invalid) {
				SetDiagnostic(
					diagnostic, Status::InvalidGroup, "group input format route is invalid", attributes->Id
				);
				return diagnostic.Code;
			}
			if (route.Source != detail::GroupInputDepth::Kind::NodeOutput) continue;
			const auto source = nodeIndices.find(route.NodeId);
			if (source == nodeIndices.end()) {
				SetDiagnostic(
					diagnostic, Status::UnknownNode, "group format producer is missing", attributes->Id
				);
				return diagnostic.Code;
			}
			auto names = budget.Reserve(std::max(route.Port.size(), std::string{}.capacity()));
			if (!names || !planCharge.Merge(std::move(*names))) {
				SetDiagnostic(
					diagnostic,
					Status::LimitExceeded,
					"group format route names exceed the live byte budget",
					attributes->Id
				);
				return diagnostic.Code;
			}
			surfaceDependencies.push_back({index, source->second, std::string(route.Port)});
			const bool ordinary =
				std::any_of(effectiveLinks.begin(), effectiveLinks.end(), [&](const Link &link) {
					return link.FromNode == route.NodeId && link.ToNode == document.Nodes[index].Id;
				});
			if (!ordinary) {
				indegree[index]++;
				downstream[source->second].push_back(index);
			}
		}
		for (const Link &link : effectiveLinks) {
			const size_t from = nodeIndices.at(link.FromNode);
			const size_t to = nodeIndices.at(link.ToNode);
			indegree[to]++;
			downstream[from].push_back(to);
		}
		if (!document.ProjectGlobalNodeId.empty()) {
			const auto global = nodeIndices.find(document.ProjectGlobalNodeId);
			if (document.FormatVersion < 9 ||
				document.ProjectGlobalNodeId.size() > Limits::MaximumTextBytes ||
				global == nodeIndices.end() || document.Nodes[global->second].Type != "pc.global_scope") {
				SetDiagnostic(
					diagnostic,
					Status::InvalidValue,
					"project global root must name a bounded native global scope"
				);
				return diagnostic.Code;
			}
		}
		std::vector<PcxNamedDependency> pcxDependencies;
		uint64_t pcxCompileWork = 0;
		const auto addPcxRoute = [&](size_t consumer,
									 size_t producer,
									 std::string_view name,
									 std::string_view port,
									 bool input) -> bool {
			PcxNamedDependency route{consumer, producer, std::string(name), std::string(port), input};
			if (std::find(pcxDependencies.begin(), pcxDependencies.end(), route) != pcxDependencies.end())
				return true;
			if (pcxDependencies.size() >= Limits::MaximumLinks) {
				SetDiagnostic(diagnostic, Status::LimitExceeded, "PCX routes exceed the graph bound");
				return false;
			}
			auto charge = budget.Reserve(
				2 * sizeof(PcxNamedDependency) + 4 * sizeof(size_t) + name.size() + port.size() + 64
			);
			if (!charge || !planCharge.Merge(std::move(*charge))) {
				SetDiagnostic(diagnostic, Status::LimitExceeded, "PCX routes exceed their allocation bound");
				return false;
			}
			pcxDependencies.push_back(std::move(route));
			++indegree[consumer];
			downstream[producer].push_back(consumer);
			return true;
		};

		uint64_t tunnelSelectorWork = 0;
		const auto tunnelSelector = [&](const Node &node, std::string_view port, const Value *&value) {
			const Node *owner = inputRouteOwner(node, port);
			if (!owner) {
				SetDiagnostic(
					diagnostic,
					Status::LimitExceeded,
					"tunnel registry input owner exceeds bounded work",
					node.Id,
					std::string(port)
				);
				return false;
			}
			const uint64_t units = 1 + effectiveLinks.size() + resolvedInputs.size() +
								   document.Keyframes.size() + owner->SourceInputExpressions.size() +
								   owner->SourceAnimatedInputs.size() + owner->Values.size();
			if (units > 64'000'000 - tunnelSelectorWork) {
				SetDiagnostic(
					diagnostic,
					Status::LimitExceeded,
					"tunnel registry getter traversal exceeds bounded work",
					node.Id,
					std::string(port)
				);
				return false;
			}
			tunnelSelectorWork += units;
			const bool changing =
				std::any_of(
					effectiveLinks.begin(),
					effectiveLinks.end(),
					[&](const Link &link) { return link.ToNode == owner->Id && link.ToPort == port; }
				) ||
				std::any_of(
					owner->SourceInputExpressions.begin(),
					owner->SourceInputExpressions.end(),
					[&](const auto &expression) { return expression.Port == port && expression.Enabled; }
				) ||
				std::find(owner->SourceAnimatedInputs.begin(), owner->SourceAnimatedInputs.end(), port) !=
					owner->SourceAnimatedInputs.end();
			if (changing) {
				value = nullptr;
				return true;
			}
			for (const auto &resolved : resolvedInputs)
				if (resolved.NodeId == owner->Id && resolved.Port == port) {
					value = &resolved.Data;
					return true;
				}
			if (const auto *stored = FindValue(*owner, port)) {
				value = &stored->Data;
				return true;
			}
			for (const auto &key : document.Keyframes)
				if (key.NodeId == owner->Id && key.Port == port) {
					value = &key.Data;
					return true;
				}
			value = nullptr;
			return true;
		};
		bool dynamicTunnelRegistry = false;
		for (const auto &node : document.Nodes) {
			if (node.Type != "pc.tunnel_in" && node.Type != "pc.tunnel_out") continue;
			for (const auto port : {std::string_view{"name"}, std::string_view{"scope"}}) {
				if (port == "scope" && node.Type == "pc.tunnel_out") continue;
				const Node *owner = inputRouteOwner(node, port);
				if (!owner) {
					SetDiagnostic(
						diagnostic,
						Status::InvalidValue,
						"tunnel selector owner is unavailable",
						node.Id,
						std::string(port)
					);
					return diagnostic.Code;
				}
				const uint64_t selectorUnits = effectiveLinks.size() + owner->SourceInputExpressions.size() +
											   owner->SourceAnimatedInputs.size() +
											   owner->SourceProperties.size() + document.Keyframes.size();
				if (selectorUnits > 64'000'000 - tunnelSelectorWork) {
					SetDiagnostic(
						diagnostic,
						Status::LimitExceeded,
						"tunnel selector checks exceed bounded work",
						node.Id,
						std::string(port)
					);
					return diagnostic.Code;
				}
				tunnelSelectorWork += selectorUnits;
				dynamicTunnelRegistry |=
					std::any_of(
						effectiveLinks.begin(),
						effectiveLinks.end(),
						[&](const Link &link) { return link.ToNode == owner->Id && link.ToPort == port; }
					) ||
					std::any_of(
						owner->SourceInputExpressions.begin(),
						owner->SourceInputExpressions.end(),
						[&](const auto &expression) { return expression.Port == port && expression.Enabled; }
					) ||
					std::find(owner->SourceAnimatedInputs.begin(), owner->SourceAnimatedInputs.end(), port) !=
						owner->SourceAnimatedInputs.end() ||
					std::any_of(
						document.Keyframes.begin(),
						document.Keyframes.end(),
						[&](const auto &key) { return key.NodeId == owner->Id && key.Port == port; }
					) ||
					std::any_of(
						owner->SourceProperties.begin(),
						owner->SourceProperties.end(),
						[&](const auto &property) {
							const auto *expression = std::get_if<std::string>(&property.Data);
							return port == "name" && property.Port == "nameExpression" && expression &&
								   !expression->empty();
						}
					);
			}
		}
		if (!dynamicTunnelRegistry &&
			!detail::CompileSourceTunnelRoutes(document, tunnelSelector, addPcxRoute, diagnostic))
			return diagnostic.Code;
		for (size_t consumer = 0; consumer < document.Nodes.size(); ++consumer) {
			const auto &authored = document.Nodes[consumer];
			const auto *catalogue = FindCatalogueEntry(authored.Type);
			const size_t inputCount =
				(catalogue ? catalogue->Inputs.size() : FindSchema(authored.Type)->Ports.size()) +
				authored.DynamicInputs.size();
			if ((!authored.SourceInputExpressions.empty() && document.FormatVersion < 9) ||
				authored.SourceInputExpressions.size() >
					std::min(Limits::MaximumSourceInputExpressionsPerNode, inputCount)) {
				SetDiagnostic(
					diagnostic,
					Status::InvalidValue,
					"PCX input programs require bounded v9 input slots",
					authored.Id
				);
				return diagnostic.Code;
			}
			std::vector<std::string_view> expressionPorts;
			std::vector<std::tuple<std::string_view, bool, bool>> programs;
			for (const auto &expression : authored.SourceInputExpressions) {
				if (expression.Port.size() > Limits::MaximumTextBytes ||
					expression.Code.size() > Limits::MaximumTextBytes ||
					(!FindPortType(authored, expression.Port, PortDirection::Input) &&
					 (!catalogue || !FindCatalogueInput(*catalogue, expression.Port))) ||
					std::find(expressionPorts.begin(), expressionPorts.end(), expression.Port) !=
						expressionPorts.end()) {
					SetDiagnostic(
						diagnostic,
						Status::InvalidValue,
						"PCX input program has an invalid or duplicated port",
						authored.Id,
						expression.Port
					);
					return diagnostic.Code;
				}
				expressionPorts.push_back(expression.Port);
				if (expression.Enabled) {
					const auto *owner = EffectiveInputOwner(document, authored, expression.Port);
					if (!owner || owner->Id == authored.Id)
						programs.emplace_back(
							expression.Code,
							true,
							(authored.Type == "pc.tunnel_in" || authored.Type == "pc.tunnel_out") &&
								(expression.Port == "name" || expression.Port == "scope")
						);
				}
			}
			const auto appendInheritedProgram = [&](std::string_view port) {
				const auto *owner = EffectiveInputOwner(document, authored, port);
				if (!owner || owner->Id == authored.Id) return;
				for (const auto &expression : owner->SourceInputExpressions)
					if (expression.Port == port && expression.Enabled)
						programs.emplace_back(
							expression.Code,
							true,
							(authored.Type == "pc.tunnel_in" || authored.Type == "pc.tunnel_out") &&
								(expression.Port == "name" || expression.Port == "scope")
						);
			};
			if (catalogue)
				for (const auto &input : catalogue->Inputs)
					appendInheritedProgram(input.Id);
			for (const auto &input : authored.DynamicInputs)
				appendInheritedProgram(input.Id);
			if (authored.Type == "pc.equation" || authored.Type == "pc.pcx_equation") {
				if (const auto *value = FindValue(authored, "equation"))
					if (const auto *text = std::get_if<std::string>(&value->Data))
						programs.emplace_back(*text, false, false);
			}
			if (authored.Type == "pc.equation" || authored.Type == "pc.pcx_equation")
				for (const auto &key : document.Keyframes)
					if (key.NodeId == authored.Id && key.Port == "equation")
						if (const auto *code = std::get_if<std::string>(&key.Data))
							programs.emplace_back(*code, false, false);
			if (authored.Type == "pc.globalvar" && !document.ProjectGlobalNodeId.empty()) {
				const size_t producer = nodeIndices.at(document.ProjectGlobalNodeId);
				for (const auto &input : document.Nodes[producer].DynamicInputs)
					if (!addPcxRoute(consumer, producer, input.Id, input.Id, true)) return diagnostic.Code;
			}
			for (const auto &[code, program, preRender] : programs) {
				auto parserCharge = budget.Reserve(
					code.size() * 8 + 4096 * sizeof(PcxInstruction) + 4 * Limits::MaximumArrayBytes
				);
				if (!parserCharge) {
					SetDiagnostic(
						diagnostic,
						Status::LimitExceeded,
						"PCX compiler workspace exceeds its live byte bound",
						authored.Id
					);
					return diagnostic.Code;
				}
				PcxExpressionValue tree;
				Diagnostic parseDiagnostic;
				const auto status = program ? CompilePcxProgram(code, tree, parseDiagnostic)
											: CompilePcxExpression(code, tree, parseDiagnostic);
				if (status != Status::Ok) {
					diagnostic = std::move(parseDiagnostic);
					diagnostic.NodeId = authored.Id;
					return diagnostic.Code;
				}
				if (preRender) continue;
				std::vector<size_t> consumers{consumer};
				if (!program && authored.Type == "pc.pcx_equation") {
					for (size_t position = 0; position < consumers.size(); ++position)
						for (const auto &link : effectiveLinks) {
							if (++pcxCompileWork > 64'000'000) {
								SetDiagnostic(
									diagnostic,
									Status::LimitExceeded,
									"PCX dependency compilation exceeds work bound"
								);
								return diagnostic.Code;
							}
							if (link.FromNode != document.Nodes[consumers[position]].Id) continue;
							const auto next = nodeIndices.at(link.ToNode);
							if (std::find(consumers.begin(), consumers.end(), next) == consumers.end())
								consumers.push_back(next);
						}
				}
				std::vector<std::string_view> locals;
				if (program) locals = {"name", "node_name", "value", "node_values"};
				if (catalogue)
					for (const auto &dynamic : authored.DynamicInputs) {
						size_t group = 0;
						const auto *field = FindDynamicTemplate(*catalogue, dynamic.Id, group);
						if (!field || field->Id != "argument_name") continue;
						const auto *authoredName = FindValue(authored, dynamic.Id);
						const Value *value = authoredName	   ? &authoredName->Data
											 : dynamic.Default ? &*dynamic.Default
															   : nullptr;
						if (const auto *name = value ? std::get_if<std::string>(value) : nullptr)
							locals.push_back(*name);
					}
				for (const auto &instruction : tree.Data->Instructions)
					if ((instruction.Operation == "=" || instruction.Operation == "default") &&
						instruction.Arguments.size() == 2) {
						const auto &left = tree.Data->Instructions[instruction.Arguments[0]];
						if (left.Operation == "name") locals.push_back(std::get<std::string>(left.Literal));
					}
				for (const auto &instruction : tree.Data->Instructions) {
					if (instruction.Operation != "name") continue;
					const auto &name = std::get<std::string>(instruction.Literal);
					if (detail::CommonNamed(document, name)) continue;
					if (std::find(locals.begin(), locals.end(), name) != locals.end() ||
						name.starts_with("self."))
						continue;
					const auto first = name.find('.'),
							   second = first == std::string::npos ? first : name.find('.', first + 1);
					std::optional<size_t> producer;
					std::string port;
					bool inputPort = true;
					if (first == std::string::npos && !document.ProjectGlobalNodeId.empty()) {
						producer = nodeIndices.at(document.ProjectGlobalNodeId);
						port = name;
					} else if (second != std::string::npos) {
						const auto ownerName = std::string_view(name).substr(0, first);
						for (size_t candidate = 0; candidate < document.Nodes.size(); ++candidate) {
							if (++pcxCompileWork > 64'000'000) {
								SetDiagnostic(
									diagnostic,
									Status::LimitExceeded,
									"PCX dependency compilation exceeds work bound"
								);
								return diagnostic.Code;
							}
							if (!ownerName.empty() &&
								document.Nodes[candidate].SourceInternalName == ownerName)
								producer = candidate;
						}
						std::string direction = name.substr(first + 1, second - first - 1);
						port = name.substr(second + 1);
						if (const auto extra = port.find('.'); extra != std::string::npos) port.resize(extra);
						for (char &c : direction)
							if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
						for (char &c : port)
							if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
						inputPort = direction == "input" || direction == "inputs";
						if (!inputPort && direction != "output" && direction != "outputs") continue;
					}
					if (!producer || !FindPortType(
										 document.Nodes[*producer],
										 port,
										 inputPort ? PortDirection::Input : PortDirection::Output
									 ))
						continue;
					for (const size_t destination : consumers)
						if (!addPcxRoute(destination, *producer, name, port, inputPort))
							return diagnostic.Code;
				}
			}
		}
		for (const Node &node : document.Nodes) {
			if (node.Type == "image.transform_3d" && !linkedInputs.contains({node.Id, "surface"})) {
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
				!linkedInputs.contains({node.Id, "image"})) {
				SetDiagnostic(
					diagnostic, Status::InvalidValue, "node requires an image input", node.Id, "image"
				);
				return diagnostic.Code;
			}
			if (node.Type == "image.displace" && !linkedInputs.contains({node.Id, "displace_map"})) {
				SetDiagnostic(
					diagnostic, Status::InvalidValue, "displace requires a map input", node.Id, "displace_map"
				);
				return diagnostic.Code;
			}
			if (node.Type == "image.height_blend") {
				for (const char *port : {"background", "foreground"}) {
					if (!linkedInputs.contains({node.Id, port})) {
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
			if (node.Type == "image.blend" && !linkedInputs.contains({node.Id, "background"})) {
				SetDiagnostic(
					diagnostic,
					Status::InvalidValue,
					"blend requires a background image",
					node.Id,
					"background"
				);
				return diagnostic.Code;
			}
			if (node.Type == "value.array_get" && !linkedInputs.contains({node.Id, "array"})) {
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
		outputNodes.reserve(document.Outputs.size());
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
			const auto commonSelector = detail::CommonSelector(output.Port);
			const auto *commonOwner = commonSelector ? detail::CommonOwner(document, output.NodeId) : nullptr;
			const auto port =
				commonOwner ? std::optional{detail::CommonSelectorType(*commonSelector)}
				: node == nodeIndices.end()
					? std::optional<ValueType>{}
					: FindPortType(
						  document.Nodes[node->second], output.Port, PortDirection::Output, &document
					  );
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
			outputNodes.push_back(
				commonOwner && node == nodeIndices.end() ? document.Nodes.size() : node->second
			);
		}
		if (document.Outputs.empty() && !allowNoDeclaredOutputs) {
			SetDiagnostic(diagnostic, Status::InvalidOutput, "document has no declared output");
			return diagnostic.Code;
		}

		std::priority_queue<size_t, detail::EvaluationVector<size_t>, std::greater<>> ready{
			std::greater<>{}, detail::EvaluationVector<size_t>{detail::EvaluationAllocator<size_t>(budget)}
		};
		for (size_t index = 0; index < indegree.size(); index++) {
			if (indegree[index] == 0) ready.push(index);
		}
		Plan compiled;
		compiled.SourceCommonRuntimeOnly = allowNoDeclaredOutputs;
		compiled.NodeOrder.reserve(document.Nodes.size());
		compiled.OutputNodes = std::move(outputNodes);
		compiled.EffectiveLinks = std::move(effectiveLinks);
		compiled.ResolvedInputs = std::move(resolvedInputs);
		compiled.SourceCommonRoutes = std::move(commonRoutes);
		compiled.GroupSurfaceDependencies = std::move(surfaceDependencies);
		compiled.InlineOwnerDependencies = std::move(inlineDependencies);
		compiled.InlineControlDependencies = std::move(inlineControlDependencies);
		compiled.PcxNamedDependencies = std::move(pcxDependencies);
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
		if (const auto status =
				detail::AppendRigidSchedule(document, compiled, budget, planCharge, diagnostic);
			status != Status::Ok)
			return status;
		plan = std::move(compiled);
		diagnostic = {};
		return Status::Ok;
	} catch (const std::bad_alloc &) {
		SetDiagnostic(
			diagnostic, Status::LimitExceeded, "compile allocation exceeds the available storage budget"
		);
		return diagnostic.Code;
	} catch (const std::length_error &) {
		SetDiagnostic(diagnostic, Status::LimitExceeded, "compile storage capacity exceeds native bounds");
		return diagnostic.Code;
	}

	Status CompileSourceCommonRuntime(
		const Document &document, Plan &plan, Diagnostic &diagnostic, uint64_t maximumBytes
	) {
		if (!maximumBytes || maximumBytes > Limits::MaximumEvaluationBytes) {
			SetDiagnostic(diagnostic, Status::LimitExceeded, "common runtime compile cap exceeds bounds");
			return diagnostic.Code;
		}
		detail::EvaluationBudget budget(maximumBytes);
		const auto bytes = DocumentRetainedPayloadBytes(document);
		auto authored = bytes ? budget.Reserve(*bytes) : std::nullopt;
		if (!authored) {
			SetDiagnostic(diagnostic, Status::LimitExceeded, "common authored document exceeds live bytes");
			return diagnostic.Code;
		}
		detail::AllocationReservation charge;
		return CompileWithBudget(document, plan, diagnostic, budget, charge, true);
	}

	Status Compile(const Document &document, Plan &plan, Diagnostic &diagnostic) {
		return Compile(document, plan, diagnostic, Limits::MaximumEvaluationBytes);
	}
	Status
	Compile(const Document &document, Plan &plan, Diagnostic &diagnostic, uint64_t maximumWorkspaceBytes) {
		if (maximumWorkspaceBytes > Limits::MaximumEvaluationBytes) {
			SetDiagnostic(diagnostic, Status::LimitExceeded, "compile workspace exceeds native bounds");
			return diagnostic.Code;
		}
		detail::EvaluationBudget budget(maximumWorkspaceBytes);
		detail::AllocationReservation planCharge;
		return CompileWithBudget(document, plan, diagnostic, budget, planCharge);
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
	// Named outputs of a source catalogue executor.
	struct CatalogueOutputs {
		std::vector<Diagnostic> Diagnostics;
		std::vector<std::pair<std::string_view, SourceSocketDomain>> Domains;
		std::vector<std::pair<std::string, SourceSocketDomain>> FrozenDomains;
		std::vector<std::pair<std::string, Image>> Images;
		std::vector<std::pair<std::string, ImageArray>> ImageArrays;
		ValueOutputs Values;
		std::optional<detail::PixelBuilderLayer> PixelBuilderUpdate;
	};
	using NodeResult = std::variant<
		Image,
		ImageArray,
		ValueOutputs,
		ShapeOutputs,
		ConversionOutputs,
		MirrorOutputs,
		CatalogueOutputs>;
	struct EvaluationResult {
		explicit EvaluationResult(uint64_t maximumBytes = Limits::MaximumEvaluationBytes)
			: Budget(maximumBytes) {}
		detail::EvaluationBudget Budget;
		detail::AllocationReservation Charge;
		NodeResult Data;
	};
	struct EvaluationSnapshot::Storage {
		explicit Storage(uint64_t maximumBytes) : Budget(maximumBytes) {}
		detail::EvaluationBudget Budget;
		detail::AllocationReservation Charge;
		detail::AllocationReservation ReplacementShadow;
		detail::AllocationReservation ReplayCharge;
		std::vector<EvaluationInputValue> Values;
		std::vector<EvaluationInputImage> Images;
		std::vector<SnapshotImageArray> ImageArrays;
		std::optional<SurfaceFormat> SurfacePolicy;
		int64_t InterpolationPolicy = 1;
	};
	struct StatefulSnapshotAccess {
		using Storage = EvaluationSnapshot::Storage;
		static void Install(EvaluationSnapshot &snapshot, std::unique_ptr<Storage> storage) {
			snapshot.Data = std::move(storage);
		}
	};
	EvaluationSnapshot::EvaluationSnapshot() = default;
	EvaluationSnapshot::~EvaluationSnapshot() = default;
	EvaluationSnapshot::EvaluationSnapshot(EvaluationSnapshot &&) noexcept = default;
	EvaluationSnapshot &EvaluationSnapshot::operator=(EvaluationSnapshot &&) noexcept = default;
	std::span<const EvaluationInputValue> EvaluationSnapshot::Values() const noexcept {
		return Data ? std::span<const EvaluationInputValue>(Data->Values)
					: std::span<const EvaluationInputValue>{};
	}
	std::span<const EvaluationInputImage> EvaluationSnapshot::Images() const noexcept {
		return Data ? std::span<const EvaluationInputImage>(Data->Images)
					: std::span<const EvaluationInputImage>{};
	}
	std::span<const SnapshotImageArray> EvaluationSnapshot::ImageArrays() const noexcept {
		return Data ? std::span<const SnapshotImageArray>(Data->ImageArrays)
					: std::span<const SnapshotImageArray>{};
	}
	std::optional<SurfaceFormat> EvaluationSnapshot::InheritedSurfaceFormat() const noexcept {
		return Data ? Data->SurfacePolicy : std::nullopt;
	}
	int64_t EvaluationSnapshot::InheritedInterpolation() const noexcept {
		return Data ? Data->InterpolationPolicy : 1;
	}
	uint64_t EvaluationSnapshot::RetainedBytes() const noexcept {
		return Data ? Data->Charge.Bytes() : 0;
	}
	static std::optional<SourceSocketKind> DeclaredSourceKind(ValueType type) {
		switch (type) {
		case ValueType::Integer:
			return SourceSocketKind::Integer;
		case ValueType::Scalar:
			return SourceSocketKind::Float;
		case ValueType::Boolean:
			return SourceSocketKind::Boolean;
		case ValueType::Colour:
			return SourceSocketKind::Colour;
		case ValueType::Image:
			return SourceSocketKind::Surface;
		case ValueType::Curve:
			return SourceSocketKind::Curve;
		case ValueType::Object:
			return SourceSocketKind::Object;
		case ValueType::NodeRef:
			return SourceSocketKind::Node;
		case ValueType::Path2D:
		case ValueType::Path3D:
			return SourceSocketKind::Path;
		case ValueType::Particle:
			return SourceSocketKind::Particle;
		case ValueType::Rigid:
			return SourceSocketKind::Rigid;
		case ValueType::SmokeDomain:
			return SourceSocketKind::SmokeDomain;
		case ValueType::Struct:
			return SourceSocketKind::Struct;
		case ValueType::Strand:
			return SourceSocketKind::Strand;
		case ValueType::Mesh2D:
			return SourceSocketKind::Mesh2D;
		case ValueType::Mesh:
			return SourceSocketKind::Mesh3D;
		case ValueType::Light3D:
			return SourceSocketKind::Light3D;
		case ValueType::Scene3D:
			return SourceSocketKind::Scene3D;
		case ValueType::Material3D:
			return SourceSocketKind::Material3D;
		case ValueType::PcxNode:
			return SourceSocketKind::PcxNode;
		case ValueType::AudioBit:
			return SourceSocketKind::Audio;
		case ValueType::FluidDomain:
			return SourceSocketKind::FluidDomain;
		case ValueType::Sdf:
			return SourceSocketKind::Sdf;
		case ValueType::Gradient:
			return SourceSocketKind::Gradient;
		case ValueType::Font:
			return SourceSocketKind::Font;
		// Text may represent a source file-path declaration. Any may be a dynamic
		// resource. Neither is inferred from a native carrier or runtime payload
		// shape.
		default:
			return std::nullopt;
		}
	}
	// Typed values produced by a value node or a catalogue executor.
	static std::optional<SourceSocketDomain>
	FindOutputDomain(const Node &node, const NodeResult &result, std::string_view port) {
		if (const auto *outputs = std::get_if<CatalogueOutputs>(&result)) {
			for (const auto &[id, domain] : outputs->FrozenDomains)
				if (id == port) return domain;
			for (const auto &[id, domain] : outputs->Domains)
				if (id == port) return domain;
			for (const auto &value : outputs->Values)
				if (value.Port == port) {
					if (const auto *field = std::get_if<NoiseFieldValue>(&value.Data))
						return SourceSocketDomain{NoiseFieldType(*field), {}, {}};
					if (const auto *array = std::get_if<ArrayValue>(&value.Data);
						array && IsNoiseFieldType(array->ElementType))
						return SourceSocketDomain{array->ElementType, {}, {}};
				}
		}
		if (const auto *entry = FindCatalogueEntry(node.Type))
			for (const auto &output : entry->Outputs)
				if (output.Id == port) {
					ValueType declared = output.Type;
					if (node.Type == "pc.gradient_extract" || node.Type == "pc.gradient_sample")
						declared = ValueType::Colour;
					return SourceSocketDomain{declared, std::nullopt, DeclaredSourceKind(declared)};
				}
		return std::nullopt;
	}

	static const Diagnostic *FindOutputDiagnostic(const NodeResult &result, std::string_view port) {
		if (const auto *outputs = std::get_if<CatalogueOutputs>(&result))
			for (const auto &diagnostic : outputs->Diagnostics)
				if (diagnostic.Port == port) return &diagnostic;
		return nullptr;
	}
	static const ValueOutputs *FindValueOutputs(const NodeResult &result) {
		if (const auto *values = std::get_if<ValueOutputs>(&result)) return values;
		if (const auto *catalogue = std::get_if<CatalogueOutputs>(&result)) return &catalogue->Values;
		return nullptr;
	}
	static ValueOutputs *FindValueOutputs(NodeResult &result) {
		if (auto *values = std::get_if<ValueOutputs>(&result)) return values;
		if (auto *catalogue = std::get_if<CatalogueOutputs>(&result)) return &catalogue->Values;
		return nullptr;
	}
	static const ImageArray *FindImageArrayOutput(const NodeResult &result, std::string_view port) {
		if (const auto *array = std::get_if<ImageArray>(&result)) return array;
		if (const auto *catalogue = std::get_if<CatalogueOutputs>(&result))
			for (const auto &[id, array] : catalogue->ImageArrays)
				if (id == port) return &array;
		return nullptr;
	}
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
		if (const auto *catalogue = std::get_if<CatalogueOutputs>(&result)) {
			for (const auto &[id, image] : catalogue->Images)
				if (id == port) return &image;
			for (const auto &value : catalogue->Values) {
				if (value.Port != port) continue;
				const auto *atlas = std::get_if<AtlasValue>(&value.Data);
				if (atlas && atlas->Data && atlas->Data->Kind == AtlasKind::SurfaceAtlas &&
					detail::ValidRuntimeValue(value.Data))
					return &atlas->Data->Surface.Data;
			}
		}
		return nullptr;
	}

	static uint64_t ItemBytes(const ImageArrayItem &item) {
		const auto *children = std::get_if<std::vector<ImageArrayItem>>(&item.Data);
		if (!children) return 0;
		uint64_t bytes = children->size() * sizeof(ImageArrayItem);
		for (const ImageArrayItem &child : *children)
			bytes += ItemBytes(child);
		return bytes;
	}

	static uint64_t ResultBytes(const ImageArray &images) {
		uint64_t bytes = images.Images.size() * sizeof(Image) + images.Items.size() * sizeof(ImageArrayItem);
		for (const ImageArrayItem &item : images.Items)
			bytes += ItemBytes(item);
		for (const Image &image : images.Images)
			bytes += image.Pixels.size();
		return bytes;
	}
	static bool AddArrayBytes(uint64_t &total, size_t count, size_t elementBytes);
	static uint64_t RetainedImageArrayBytes(const ImageArray &images) {
		uint64_t bytes = 0;
		size_t items = 0;
		const auto retainItems =
			[&](auto &&self, const std::vector<ImageArrayItem> &rows, size_t depth) -> bool {
			if (depth > Limits::MaximumArrayDepth || rows.size() > Limits::MaximumArrayElements - items ||
				!AddArrayBytes(bytes, rows.capacity(), sizeof(ImageArrayItem)))
				return false;
			items += rows.size();
			for (const auto &item : rows)
				if (const auto *nested = std::get_if<std::vector<ImageArrayItem>>(&item.Data))
					if (!self(self, *nested, depth + 1)) return false;
			return true;
		};
		if (!AddArrayBytes(bytes, images.Images.capacity(), sizeof(Image)) ||
			!retainItems(retainItems, images.Items, 0))
			return std::numeric_limits<uint64_t>::max();
		for (const auto &image : images.Images)
			if (!AddBytes(bytes, image.Pixels.capacity())) return std::numeric_limits<uint64_t>::max();
		return bytes;
	}
	static uint64_t RetainedStatefulValueBytes(const std::variant<Image, ImageArray, EvaluatedValue> &value) {
		return std::visit(
			[](const auto &output) -> uint64_t {
				using T = std::decay_t<decltype(output)>;
				if constexpr (std::is_same_v<T, Image>)
					return output.Pixels.capacity();
				else if constexpr (std::is_same_v<T, ImageArray>)
					return RetainedImageArrayBytes(output);
				else {
					uint64_t bytes = detail::RetainedPayloadBytes(output.Data);
					return AddBytes(bytes, output.Port.capacity()) ? bytes
																   : std::numeric_limits<uint64_t>::max();
				}
			},
			value
		);
	}
	uint64_t RetainedStatefulOutputBytes(const StatefulOutputEvaluationResult &result) {
		uint64_t bytes = 0;
		if (result.Outputs.size() > Limits::MaximumOutputs ||
			!AddArrayBytes(bytes, result.Outputs.capacity(), sizeof(StatefulNamedOutput)))
			return std::numeric_limits<uint64_t>::max();
		for (const auto &output : result.Outputs)
			if (!AddBytes(bytes, output.Id.capacity()) ||
				!AddBytes(bytes, RetainedStatefulValueBytes(output.Output)))
				return std::numeric_limits<uint64_t>::max();
		return bytes;
	}

	static uint64_t ResultBytes(const CatalogueOutputs &catalogue) {
		uint64_t bytes = catalogue.Diagnostics.size() * sizeof(Diagnostic);
		for (const auto &diagnostic : catalogue.Diagnostics)
			bytes += diagnostic.NodeId.size() + diagnostic.Port.size() + diagnostic.Message.size();
		for (const auto &[id, image] : catalogue.Images)
			bytes += image.Pixels.size();
		for (const auto &[id, array] : catalogue.ImageArrays)
			bytes += ResultBytes(array);
		for (const AuthoredValue &value : catalogue.Values)
			bytes += detail::ValuePayloadBytes(value.Data);
		return bytes;
	}

	static uint64_t ResultBytes(const ValueOutputs &values) {
		uint64_t bytes = 0;
		for (const AuthoredValue &value : values)
			bytes += detail::ValuePayloadBytes(value.Data);
		return bytes;
	}
	static uint64_t ResultBytes(const ShapeOutputs &shape) {
		return shape.Colored.Pixels.size() + shape.Mask.Pixels.size() + shape.Height.Pixels.size() +
			   shape.UV.Pixels.size();
	}
	static uint64_t ResultBytes(const ConversionOutputs &conversion) {
		uint64_t bytes = 0;
		for (const Image &image : conversion.Channels)
			bytes += image.Pixels.size();
		return bytes;
	}
	static uint64_t ResultBytes(const MirrorOutputs &mirror) {
		return mirror.Colored.Pixels.size() + mirror.Mask.Pixels.size();
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
		if (const auto *catalogue = std::get_if<CatalogueOutputs>(&result)) return ResultBytes(*catalogue);
		uint64_t bytes = 0;
		for (const AuthoredValue &value : *FindValueOutputs(result))
			bytes += detail::ValuePayloadBytes(value.Data);
		return bytes;
	}

	// Native image collectors use indexed leaves; the replay Value carrier owns source-shaped pixels.
	static bool MeasureFrozenImageArray(const ArrayValue &array, size_t &images, size_t &items) {
		const auto element = [&](const ElementValue &value) {
			if (!std::holds_alternative<SurfaceValue>(value)) return false;
			++images;
			++items;
			return true;
		};
		const auto visit = [&](auto &&self, const SourceArrayItem &item) -> bool {
			if (const auto *children = std::get_if<std::vector<SourceArrayItem>>(&item.Data)) {
				++items;
				for (const auto &child : *children)
					if (!self(self, child)) return false;
				return true;
			}
			if (std::holds_alternative<Image>(item.Data)) {
				++images;
				++items;
				return true;
			}
			return element(std::get<ElementValue>(item.Data));
		};
		for (const auto &item : array.Items)
			if (!visit(visit, item)) return false;
		for (const auto &row : array.Nested) {
			++items;
			for (const auto &value : row)
				if (!element(value)) return false;
		}
		for (const auto &value : array.Elements)
			if (!element(value)) return false;
		return images <= Limits::MaximumArrayElements && items <= Limits::MaximumArrayElements;
	}
	static ImageArray RestoreFrozenImageArray(ArrayValue &array, size_t images) {
		ImageArray output;
		output.Images.reserve(images);
		const auto image = [&](Image &value) {
			ImageArrayItem item{output.Images.size()};
			output.Images.push_back(std::move(value));
			return item;
		};
		const auto element = [&](ElementValue &value) { return image(std::get<SurfaceValue>(value).Data); };
		const auto visit = [&](auto &&self, SourceArrayItem &item) -> ImageArrayItem {
			if (auto *children = std::get_if<std::vector<SourceArrayItem>>(&item.Data)) {
				std::vector<ImageArrayItem> row;
				row.reserve(children->size());
				for (auto &child : *children)
					row.push_back(self(self, child));
				return ImageArrayItem{std::move(row)};
			}
			if (auto *value = std::get_if<Image>(&item.Data)) return image(*value);
			return element(std::get<ElementValue>(item.Data));
		};
		output.Items.reserve(array.Items.size() + array.Nested.size() + array.Elements.size());
		for (auto &item : array.Items)
			output.Items.push_back(visit(visit, item));
		for (auto &values : array.Nested) {
			std::vector<ImageArrayItem> row;
			row.reserve(values.size());
			for (auto &value : values)
				row.push_back(element(value));
			output.Items.push_back({std::move(row)});
		}
		for (auto &value : array.Elements)
			output.Items.push_back(element(value));
		return output;
	}

	template <class T> static bool FrozenSurfaceBounds(const T &value, uint32_t maximumDimension) {
		if constexpr (std::is_same_v<T, Value> || std::is_same_v<T, ElementValue>)
			return std::visit(
				[&](const auto &leaf) { return FrozenSurfaceBounds(leaf, maximumDimension); }, value
			);
		else if constexpr (std::is_same_v<T, SourceArrayItem>)
			return std::visit(
				[&](const auto &leaf) { return FrozenSurfaceBounds(leaf, maximumDimension); }, value.Data
			);
		else if constexpr (std::is_same_v<T, ArrayValue>)
			return FrozenSurfaceBounds(value.Elements, maximumDimension) &&
				   FrozenSurfaceBounds(value.Nested, maximumDimension) &&
				   FrozenSurfaceBounds(value.Items, maximumDimension);
		else if constexpr (std::is_same_v<T, std::vector<ElementValue>> ||
						   std::is_same_v<T, std::vector<std::vector<ElementValue>>> ||
						   std::is_same_v<T, std::vector<SourceArrayItem>>)
			return std::all_of(value.begin(), value.end(), [&](const auto &leaf) {
				return FrozenSurfaceBounds(leaf, maximumDimension);
			});
		else if constexpr (std::is_same_v<T, SurfaceValue>)
			return FrozenSurfaceBounds(value.Data, maximumDimension);
		else if constexpr (std::is_same_v<T, Image>)
			return ValidSurfaceLayout(value, maximumDimension, Limits::MaximumOutputBytes);
		else if constexpr (std::is_same_v<T, AtlasValue>)
			return !value.Data || value.Data->Kind != AtlasKind::SurfaceAtlas ||
				   FrozenSurfaceBounds(value.Data->Surface, maximumDimension);
		else
			return true;
	}

	static std::optional<uint64_t> CacheGroupValueCloneBytes(const Value &value) {
		if (const auto *surface = std::get_if<SurfaceValue>(&value)) {
			if (!ValidSurfaceLayout(surface->Data, Limits::MaximumDimension, Limits::MaximumOutputBytes) ||
				!FiniteSurfaceSamples(surface->Data))
				return std::nullopt;
			return sizeof(Value) + surface->Data.Pixels.size();
		}
		return ValueClonePayloadBytes(value);
	}
	// Frozen journals carry owned raw values. Restore named getters without input reads or kernels.
	static bool RestoreFrozenCacheGroupOutputs(
		const Node &node,
		const CacheGroupReplayNode &snapshot,
		uint32_t maximumDimension,
		CatalogueOutputs &output,
		detail::EvaluationBudget &budget,
		detail::AllocationReservation &charge,
		Diagnostic &diagnostic
	) {
		ENGINE_PROFILE("imagegraph.cache_group.restore");
		uint64_t bytes = 0;
		size_t values = 0, images = 0, arrays = 0, domains = 0, refusals = 0;
		const auto overflow = [&]() {
			SetDiagnostic(
				diagnostic, Status::LimitExceeded, "frozen output size exceeds bounds", node.Id, "cache_group"
			);
			return false;
		};
		for (const auto &port : snapshot.Outputs) {
			if (port.Data) {
				if (!FrozenSurfaceBounds(*port.Data, maximumDimension)) {
					SetDiagnostic(
						diagnostic,
						Status::LimitExceeded,
						"frozen surface exceeds request bounds",
						node.Id,
						port.Port
					);
					return false;
				}
				const auto payload = CacheGroupValueCloneBytes(*port.Data);
				if (!payload || !AddBytes(bytes, *payload)) return overflow();
				if (std::holds_alternative<SurfaceValue>(*port.Data)) {
					++images;
					if (!AddBytes(bytes, sizeof(std::pair<std::string, Image>))) return overflow();
				} else if (port.ImageArrayPayload && std::holds_alternative<ArrayValue>(*port.Data)) {
					++arrays;
					size_t arrayImages = 0, arrayItems = 0;
					if (!MeasureFrozenImageArray(std::get<ArrayValue>(*port.Data), arrayImages, arrayItems)) {
						SetDiagnostic(
							diagnostic,
							Status::TypeMismatch,
							"frozen native image array contains a value leaf",
							node.Id,
							port.Port
						);
						return false;
					}
					if (!AddBytes(bytes, sizeof(std::pair<std::string, ImageArray>)) ||
						!AddArrayBytes(bytes, arrayImages, sizeof(Image)) ||
						!AddArrayBytes(bytes, arrayItems, sizeof(ImageArrayItem)))
						return overflow();
				} else {
					++values;
					if (!AddBytes(bytes, sizeof(AuthoredValue))) return overflow();
				}
				if (!AddBytes(bytes, std::max(port.Port.size(), std::string{}.capacity()))) return overflow();
			}
			if (port.Domain) {
				++domains;
				if (!AddBytes(bytes, sizeof(std::pair<std::string, SourceSocketDomain>)) ||
					!AddBytes(bytes, std::max(port.Port.size(), std::string{}.capacity())))
					return overflow();
			}
			if (port.Refusal) {
				++refusals;
				if (!AddBytes(bytes, sizeof(Diagnostic))) return overflow();
				for (const auto *text : {&port.Refusal->NodeId, &port.Refusal->Port, &port.Refusal->Message})
					if (!AddBytes(bytes, std::max(text->size(), std::string{}.capacity()))) return overflow();
			}
		}
		auto reservation = budget.Reserve(bytes);
		if (!reservation) {
			SetDiagnostic(
				diagnostic,
				Status::LimitExceeded,
				"frozen output clone exceeds live byte budget",
				node.Id,
				"cache_group"
			);
			return false;
		}
		charge = std::move(*reservation);
		output.Values.reserve(values);
		output.Images.reserve(images);
		output.ImageArrays.reserve(arrays);
		output.FrozenDomains.reserve(domains);
		output.Diagnostics.reserve(refusals);
		for (const auto &port : snapshot.Outputs) {
			if (port.Refusal) output.Diagnostics.push_back(*port.Refusal);
			if (port.Domain) output.FrozenDomains.emplace_back(port.Port, *port.Domain);
			if (!port.Data) continue;
			if (const auto *surface = std::get_if<SurfaceValue>(&*port.Data))
				output.Images.emplace_back(port.Port, surface->Data);
			else if (port.ImageArrayPayload && std::holds_alternative<ArrayValue>(*port.Data)) {
				ArrayValue owned = std::get<ArrayValue>(*port.Data);
				size_t arrayImages = 0, arrayItems = 0;
				if (!MeasureFrozenImageArray(owned, arrayImages, arrayItems)) std::terminate();
				output.ImageArrays.emplace_back(port.Port, RestoreFrozenImageArray(owned, arrayImages));
			} else {
				output.Values.push_back({port.Port, *port.Data});
				detail::StripSourcePathShiftIdentities(output.Values.back().Data);
			}
		}
		return true;
	}

	struct CacheGroupCaptureView {
		bool ImageArrayPayload = false;
		std::string_view Port;
		const Value *Data = nullptr;
		const Image *Surface = nullptr;
		const ImageArray *Images = nullptr;
		const Diagnostic *Refusal = nullptr;
		std::optional<SourceSocketDomain> Domain;
		bool Published = false;
	};
	static std::optional<uint64_t> CapturedImageArrayBytes(const ImageArray &array) {
		uint64_t bytes = 0;
		size_t count = 0;
		const auto image = [&](size_t index) {
			return index < array.Images.size() && bytes <= Limits::MaximumArrayBytes &&
				   array.Images[index].Pixels.size() <= Limits::MaximumArrayBytes - bytes &&
				   ValidSurfaceLayout(
					   array.Images[index], Limits::MaximumDimension, Limits::MaximumArrayBytes
				   ) &&
				   FiniteSurfaceSamples(array.Images[index]) &&
				   AddBytes(bytes, array.Images[index].Pixels.size());
		};
		const auto item = [&](auto &&self, const ImageArrayItem &value, size_t depth) -> bool {
			if (depth > Limits::MaximumArrayDepth || ++count > Limits::MaximumArrayElements ||
				!AddBytes(bytes, sizeof(SourceArrayItem)) || bytes > Limits::MaximumArrayBytes)
				return false;
			if (const auto *index = std::get_if<size_t>(&value.Data)) return image(*index);
			for (const auto &child : std::get<std::vector<ImageArrayItem>>(value.Data))
				if (!self(self, child, depth + 1)) return false;
			return true;
		};
		if (array.Items.empty()) {
			if (array.Images.size() > Limits::MaximumArrayElements) return std::nullopt;
			for (size_t index = 0; index < array.Images.size(); ++index)
				if (!AddBytes(bytes, sizeof(SourceArrayItem)) || !image(index)) return std::nullopt;
		} else
			for (const auto &value : array.Items)
				if (!item(item, value, 1)) return std::nullopt;
		return bytes <= Limits::MaximumArrayBytes ? std::optional<uint64_t>{bytes} : std::nullopt;
	}
	static ArrayValue CaptureImageArray(const ImageArray &array) {
		ArrayValue output{ValueType::Any, {}};
		const auto item = [&](auto &&self, const ImageArrayItem &value) -> SourceArrayItem {
			if (const auto *index = std::get_if<size_t>(&value.Data))
				return SourceArrayItem{array.Images[*index]};
			const auto &children = std::get<std::vector<ImageArrayItem>>(value.Data);
			std::vector<SourceArrayItem> row;
			row.reserve(children.size());
			for (const auto &child : children)
				row.push_back(self(self, child));
			return SourceArrayItem{std::move(row)};
		};
		output.Items.reserve(array.Items.empty() ? array.Images.size() : array.Items.size());
		if (array.Items.empty())
			for (const auto &image : array.Images)
				output.Items.push_back({image});
		else
			for (const auto &value : array.Items)
				output.Items.push_back(item(item, value));
		return output;
	}
	static uint64_t
	RetainedCacheGroupOutputBytes(std::span<const CacheGroupReplayOutput> outputs, size_t capacity) {
		uint64_t bytes = capacity * sizeof(CacheGroupReplayOutput);
		for (const auto &port : outputs) {
			if (!AddBytes(bytes, port.Port.capacity())) return UINT64_MAX;
			if (port.Data && !AddBytes(bytes, detail::RetainedPayloadBytes(*port.Data))) return UINT64_MAX;
			if (port.Refusal)
				for (const auto *text : {&port.Refusal->NodeId, &port.Refusal->Port, &port.Refusal->Message})
					if (!AddBytes(bytes, text->capacity())) return UINT64_MAX;
		}
		return bytes;
	}
	// Replace only published getters. Uncomputed constructor ports and owner/activity remain intact.
	static bool CaptureCacheGroupOutputs(
		const Node &node,
		const NodeResult &result,
		CacheGroupReplayNode &target,
		detail::EvaluationBudget &budget,
		detail::AllocationReservation &stateCharge,
		uint64_t &comparisonWork,
		Diagnostic &diagnostic
	) {
		ENGINE_PROFILE("imagegraph.cache_group.capture");
		constexpr size_t MAXIMUM_PORTS = Limits::MaximumDynamicOutputsPerNode + Limits::MaximumGroupPorts;
		constexpr uint64_t WORK_LIMIT = 64ull * 1024 * 1024;
		const auto fail = [&](Status code, const char *message, std::string_view port = {}) {
			SetDiagnostic(diagnostic, code, message, node.Id, std::string(port));
			return false;
		};
		using IndexEntry = std::pair<const std::string_view, size_t>;
		std::map<std::string_view, size_t, std::less<>, detail::EvaluationAllocator<IndexEntry>> indices{
			std::less<>{}, detail::EvaluationAllocator<IndexEntry>(budget)
		};
		detail::EvaluationVector<CacheGroupCaptureView> views{
			detail::EvaluationAllocator<CacheGroupCaptureView>(budget)
		};
		const auto find = [&](std::string_view port) -> CacheGroupCaptureView * {
			if (port.empty() || port.size() > Limits::MaximumTextBytes) {
				fail(Status::InvalidValue, "published cache-group output name is invalid");
				return nullptr;
			}
			size_t levels = 1;
			for (size_t count = indices.size(); count > 1; count = (count + 1) / 2)
				++levels;
			const uint64_t work = 4 * levels * (port.size() + 1);
			if (work > WORK_LIMIT - comparisonWork) {
				fail(Status::LimitExceeded, "cache-group capture exceeds whole-batch name work");
				return nullptr;
			}
			comparisonWork += work;
			const auto existing = indices.find(port);
			if (existing != indices.end()) return &views[existing->second];
			if (views.size() == MAXIMUM_PORTS) {
				fail(Status::LimitExceeded, "cache-group capture exceeds named output count");
				return nullptr;
			}
			indices.emplace(port, views.size());
			CacheGroupCaptureView view;
			view.Port = port;
			views.push_back(view);
			return &views.back();
		};
		for (const auto &port : target.Outputs) {
			auto *view = find(port.Port);
			if (!view) return false;
			view->Data = port.Data ? &*port.Data : nullptr;
			view->ImageArrayPayload = port.ImageArrayPayload;
			view->Domain = port.Domain;
			view->Refusal = port.Refusal ? &*port.Refusal : nullptr;
		}
		const auto publish = [&](std::string_view port) -> CacheGroupCaptureView * {
			auto *view = find(port);
			if (view && !view->Published) {
				view->Data = nullptr;
				view->ImageArrayPayload = false;
				view->Refusal = nullptr;
				view->Domain = FindOutputDomain(node, result, port);
				view->Published = true;
			}
			return view;
		};
		if (const auto *catalogue = std::get_if<CatalogueOutputs>(&result)) {
			for (const auto &[id, surface] : catalogue->Images) {
				auto *view = publish(id);
				if (!view) return false;
				view->Surface = &surface;
			}
			for (const auto &[id, images] : catalogue->ImageArrays) {
				auto *view = publish(id);
				if (!view) return false;
				view->Images = &images;
				view->ImageArrayPayload = true;
			}
			for (const auto &value : catalogue->Values) {
				auto *view = publish(value.Port);
				if (!view) return false;
				view->Data = &value.Data;
			}
			for (const auto &refusal : catalogue->Diagnostics) {
				auto *view = publish(refusal.Port);
				if (!view) return false;
				if (!view->Refusal) view->Refusal = &refusal;
			}
			for (const auto &[id, domain] : catalogue->Domains) {
				auto *view = find(id);
				if (!view) return false;
				view->Domain = domain;
			}
		} else if (const auto *values = std::get_if<ValueOutputs>(&result)) {
			for (const auto &value : *values) {
				auto *view = publish(value.Port);
				if (!view) return false;
				view->Data = &value.Data;
			}
		} else if (const auto *schema = FindSchema(node.Type)) {
			for (const auto &port : schema->Ports) {
				if (port.Direction != PortDirection::Output) continue;
				const auto *surface =
					port.Type == ValueType::Image ? FindImageOutput(result, port.Id) : nullptr;
				const auto *images =
					port.Type == ValueType::Array ? FindImageArrayOutput(result, port.Id) : nullptr;
				if (!surface && !images) continue;
				auto *view = publish(port.Id);
				if (!view) return false;
				view->Surface = surface;
				view->Images = images;
				view->ImageArrayPayload = images != nullptr;
			}
		}
		uint64_t bytes = views.size() * sizeof(CacheGroupReplayOutput);
		for (auto &view : views) {
			if (!AddBytes(bytes, std::max(view.Port.size(), std::string{}.capacity())))
				return fail(Status::LimitExceeded, "cache-group clone size overflows", view.Port);
			std::optional<uint64_t> payload{0};
			if (view.Data)
				payload = CacheGroupValueCloneBytes(*view.Data);
			else if (view.Surface) {
				if (view.Surface->Pixels.size() > Limits::MaximumOutputBytes)
					return fail(
						Status::LimitExceeded, "cache-group surface exceeds native output bounds", view.Port
					);
				if ((!ValidSurfaceLayout(
						 *view.Surface, Limits::MaximumDimension, Limits::MaximumOutputBytes
					 ) ||
					 !FiniteSurfaceSamples(*view.Surface)))
					payload.reset();
				else
					payload = view.Surface->Pixels.size();
			} else if (view.Images) {
				payload = CapturedImageArrayBytes(*view.Images);
				if (!payload)
					return fail(
						Status::LimitExceeded,
						"cache-group image array exceeds typed shape or payload bounds",
						view.Port
					);
			}
			if (!payload) {
				if (!view.Refusal)
					return fail(Status::InvalidValue, "published cache-group payload is invalid", view.Port);
				view.Data = nullptr;
				view.Surface = nullptr;
				view.Images = nullptr;
				view.ImageArrayPayload = false;
				payload = 0;
			}
			if (!AddBytes(bytes, *payload))
				return fail(Status::LimitExceeded, "cache-group clone size overflows", view.Port);
			if (view.Refusal)
				for (const auto *text : {&view.Refusal->NodeId, &view.Refusal->Port, &view.Refusal->Message})
					if (!AddBytes(bytes, std::max(text->size(), std::string{}.capacity())))
						return fail(Status::LimitExceeded, "cache-group refusal size overflows", view.Port);
		}
		auto charge = budget.Reserve(bytes);
		if (!charge)
			return fail(Status::LimitExceeded, "cache-group snapshot clone exceeds live byte budget");
		std::vector<CacheGroupReplayOutput> outputs;
		outputs.reserve(views.size());
		for (const auto &view : views) {
			CacheGroupReplayOutput output;
			output.Port = view.Port;
			output.ImageArrayPayload = view.ImageArrayPayload;
			output.Domain = view.Domain;
			if (view.Refusal) output.Refusal = *view.Refusal;
			if (view.Data)
				output.Data = *view.Data;
			else if (view.Surface)
				output.Data = SurfaceValue{*view.Surface};
			else if (view.Images)
				output.Data = CaptureImageArray(*view.Images);
			outputs.push_back(std::move(output));
		}
		const uint64_t oldBytes = RetainedCacheGroupOutputBytes(target.Outputs, target.Outputs.capacity());
		target.Outputs = std::move(outputs);
		if (!stateCharge.Merge(std::move(*charge))) std::terminate();
		auto released = stateCharge.Split(oldBytes);
		if (!released) std::terminate();
		return true;
	}

	struct WavPreviewCapture {
		std::string_view NodeId;
		WavPreviewControls Controls;
		bool Captured = false;
	};

	struct Vector2PresentationCapture {
		std::string_view NodeId;
		Vector2Presentation Controls;
		const Image *Sprite = nullptr;
	};
	struct NodeInputCapture {
		std::string_view NodeId;
		std::vector<EvaluationInputValue> *Values = nullptr;
		std::vector<EvaluationInputImage> *Images = nullptr;
		std::optional<SurfaceFormat> *SurfacePolicy = nullptr;
		detail::AllocationReservation *Charge = nullptr;
		std::vector<SnapshotImageArray> *ImageArrays = nullptr;
		int64_t *InterpolationPolicy = nullptr;
		bool ExternalBoundary = true;
		std::string_view SourcePort = {};
		std::optional<std::span<const AuthoredValue>> ObservedSourceInputs = std::nullopt;
		std::string_view ObservedSourceInputOwner = {};
	};
	struct InlineOwnerInputs {
		detail::AllocationReservation Charge;
		std::vector<EvaluationInputValue> Values;
		std::vector<EvaluationInputImage> Images;
		std::vector<SnapshotImageArray> ImageArrays;
		std::vector<std::pair<std::string_view, const Value *>> ValueViews;
		std::vector<std::pair<std::string_view, const Image *>> ImageViews;
		std::vector<std::pair<std::string_view, const ImageArray *>> ImageArrayViews;
		std::optional<SurfaceFormat> SurfacePolicy;
		std::vector<std::string_view> LinkedValues;
		bool Captured = false;
	};
	struct PcxEvaluationNames final : PcxNameResolver {
		size_t Consumer;
		const EvaluationRequest *CommonRequest = nullptr;
		const Plan &Compiled;
		const Document &Authored;
		const std::vector<NodeResult> &Results;
		std::span<const InlineOwnerInputs> Inputs;
		std::span<const PcxNamedDependency> DynamicRoutes;
		std::optional<PcxNamedDependency> *PendingRoute = nullptr;
		std::span<const uint8_t> Produced;
		detail::EvaluationBudget *RouteBudget = nullptr;
		detail::AllocationReservation *RouteCharge = nullptr;
		PcxEvaluationNames(
			size_t consumer,
			const Plan &plan,
			const Document &document,
			const std::vector<NodeResult> &results,
			std::span<const InlineOwnerInputs> inputs,
			std::span<const PcxNamedDependency> dynamicRoutes = {},
			std::optional<PcxNamedDependency> *pendingRoute = nullptr,
			detail::EvaluationBudget *routeBudget = nullptr,
			detail::AllocationReservation *routeCharge = nullptr
		)
			: Consumer(consumer), Compiled(plan), Authored(document), Results(results), Inputs(inputs),
			  DynamicRoutes(dynamicRoutes), PendingRoute(pendingRoute), RouteBudget(routeBudget),
			  RouteCharge(routeCharge) {}
		bool Await(
			size_t producer, std::string_view name, std::string_view port, bool input, Diagnostic &diagnostic
		) const {
			diagnostic = {
				Status::UnsupportedExecution,
				Authored.Nodes[producer].Id,
				std::string(port),
				"PCX named dependency is awaiting its producer"
			};
			if (!PendingRoute || !RouteBudget || !RouteCharge) return false;
			auto charge = RouteBudget->Reserve(
				2 * (name.size() + port.size()) + 2 * sizeof(PcxNamedDependency) + 4 * sizeof(size_t) + 64
			);
			if (!charge || !RouteCharge->Merge(std::move(*charge))) {
				diagnostic.Code = Status::LimitExceeded;
				diagnostic.Message = "dynamic PCX dependencies exceed the live byte budget";
				return false;
			}
			*PendingRoute =
				PcxNamedDependency{Consumer, producer, std::string(name), std::string(port), input};
			return false;
		}
		static bool Copy(const Value &source, Value &owned, Diagnostic &diagnostic) {
			const auto bytes = ValueClonePayloadBytes(source);
			if (!bytes || *bytes > Limits::MaximumArrayBytes) {
				diagnostic = {Status::LimitExceeded, {}, {}, "PCX observation exceeds its value bytes"};
				return false;
			}
			owned = source;
			return true;
		}
		static bool Surface(const Image &source, Value &owned, Diagnostic &diagnostic) {
			if (source.Pixels.size() > Limits::MaximumArrayBytes) {
				diagnostic = {
					Status::LimitExceeded, {}, {}, "PCX surface observation exceeds its value bytes"
				};
				return false;
			}
			owned = SurfaceValue{source};
			return true;
		}
		static bool Images(const ImageArray &images, Value &owned, Diagnostic &diagnostic) {
			uint64_t bytes = 0;
			size_t count = 0;
			const auto convert =
				[&](auto &&self, const ImageArrayItem &item, SourceArrayItem &output, size_t depth) -> bool {
				if (depth > 64 || ++count > Limits::MaximumArrayElements) return false;
				bytes += sizeof(SourceArrayItem);
				if (bytes > Limits::MaximumArrayBytes) return false;
				if (const auto *index = std::get_if<size_t>(&item.Data)) {
					if (*index >= images.Images.size()) return false;
					const auto &image = images.Images[*index];
					if (image.Pixels.size() > Limits::MaximumArrayBytes - bytes) return false;
					bytes += image.Pixels.size();
					output.Data = image;
					return true;
				}
				std::vector<SourceArrayItem> children;
				for (const auto &child : std::get<std::vector<ImageArrayItem>>(item.Data)) {
					SourceArrayItem next{ElementValue{double{0}}};
					if (!self(self, child, next, depth + 1)) return false;
					children.push_back(std::move(next));
				}
				output.Data = std::move(children);
				return true;
			};
			ArrayValue array{ValueType::Any, {}};
			if (images.Items.empty()) {
				for (size_t i = 0; i < images.Images.size(); ++i) {
					SourceArrayItem next{ElementValue{double{0}}};
					if (!convert(convert, ImageArrayItem{i}, next, 1)) {
						diagnostic = {
							Status::LimitExceeded, {}, {}, "PCX image observation exceeds shape or bytes"
						};
						return false;
					}
					array.Items.push_back(std::move(next));
				}
			} else
				for (const auto &item : images.Items) {
					SourceArrayItem next{ElementValue{double{0}}};
					if (!convert(convert, item, next, 1)) {
						diagnostic = {
							Status::LimitExceeded, {}, {}, "PCX image observation exceeds shape or bytes"
						};
						return false;
					}
					array.Items.push_back(std::move(next));
				}
			owned = std::move(array);
			return true;
		}
		bool Resolve(std::string_view name, Value &value, Diagnostic &diagnostic) const override {
			if (name.starts_with("Project.") || name.starts_with("Program.") || name.starts_with("Device."))
				return false;
			if (const auto common = detail::CommonNamed(Authored, name)) {
				if (!CommonRequest || !RouteBudget || !RouteCharge) {
					diagnostic = {
						Status::UnsupportedExecution,
						common->Owner->SourceOwnerId,
						{},
						"common PCX getter needs borrowed runtime state"
					};
					return false;
				}
				detail::AllocationReservation charge;
				const auto status = detail::ReadSourceCommonGetter(
					Authored,
					Compiled,
					common->Owner->SourceOwnerId,
					common->Selector,
					*CommonRequest,
					*RouteBudget,
					value,
					charge,
					diagnostic
				);
				if (status == Status::Ok && !RouteCharge->Merge(std::move(charge))) std::terminate();
				return status == Status::Ok;
			}
			const auto compiled = std::find_if(
				Compiled.PcxNamedDependencies.begin(),
				Compiled.PcxNamedDependencies.end(),
				[&](const auto &route) { return route.Consumer == Consumer && route.Name == name; }
			);
			const auto dynamic =
				std::find_if(DynamicRoutes.begin(), DynamicRoutes.end(), [&](const auto &route) {
					return route.Consumer == Consumer && route.Name == name;
				});
			const PcxNamedDependency *found = compiled != Compiled.PcxNamedDependencies.end() ? &*compiled
											  : dynamic != DynamicRoutes.end()				  ? &*dynamic
																							  : nullptr;
			if (!found) {
				diagnostic = {};
				const auto first = name.find('.'),
						   second = first == std::string_view::npos ? first : name.find('.', first + 1);
				if (second != std::string_view::npos) {
					const auto owner = name.substr(0, first);
					for (auto node = Authored.Nodes.rbegin(); node != Authored.Nodes.rend(); ++node)
						if (!owner.empty() && node->SourceInternalName == owner) {
							std::string direction(name.substr(first + 1, second - first - 1)),
								port(name.substr(second + 1));
							if (auto extra = port.find('.'); extra != std::string::npos) port.resize(extra);
							for (char &c : direction)
								if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
							for (char &c : port)
								if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
							const bool input = direction == "input" || direction == "inputs";
							if (!input && direction != "output" && direction != "outputs") break;
							const auto *entry = FindCatalogueEntry(node->Type);
							if (FindPortType(
									*node, port, input ? PortDirection::Input : PortDirection::Output
								) ||
								(input && entry && FindCatalogueInput(*entry, port))) {
								return Await(
									size_t(node.base() - Authored.Nodes.begin() - 1),
									name,
									port,
									input,
									diagnostic
								);
							}
							diagnostic.Message = "Junction " + port + " not found.";
							break;
						}
				}
				if (first == std::string_view::npos && !Authored.ProjectGlobalNodeId.empty())
					for (size_t index = 0; index < Authored.Nodes.size(); ++index) {
						const auto &global = Authored.Nodes[index];
						if (global.Id != Authored.ProjectGlobalNodeId) continue;
						if (std::any_of(
								global.DynamicInputs.begin(),
								global.DynamicInputs.end(),
								[&](const auto &input) { return input.Id == name; }
							)) {
							return Await(index, name, name, true, diagnostic);
						}
					}
				value = double{0};
				return true;
			}
			if (!Produced.empty() &&
				!(found->Input ? Inputs[found->Producer].Captured : bool(Produced[found->Producer])))
				return Await(found->Producer, found->Name, found->Port, found->Input, diagnostic);
			if (found->Input) {
				const auto &inputs = Inputs[found->Producer];
				if (!inputs.Captured) {
					diagnostic = {Status::InvalidValue, {}, {}, "PCX observed inputs were not captured"};
					return false;
				}
				for (const auto &input : inputs.Values)
					if (input.Port == found->Port) {
						return Copy(input.Data, value, diagnostic);
					}
				for (const auto &image : inputs.Images)
					if (image.Port == found->Port) {
						return Surface(image.Data, value, diagnostic);
					}
				for (const auto &images : inputs.ImageArrays)
					if (images.Port == found->Port) return Images(images.Data, value, diagnostic);
			} else {
				const auto &result = Results[found->Producer];
				if (const auto *refusal = FindOutputDiagnostic(result, found->Port)) {
					diagnostic = *refusal;
					return false;
				}
				if (const auto *values = FindValueOutputs(result))
					for (const auto &output : *values)
						if (output.Port == found->Port) {
							return Copy(output.Data, value, diagnostic);
						}
				if (const auto *image = FindImageOutput(result, found->Port)) {
					return Surface(*image, value, diagnostic);
				}
				if (const auto *images = FindImageArrayOutput(result, found->Port))
					return Images(*images, value, diagnostic);
			}
			value = double{0};
			diagnostic = {};
			return true;
		}
	};
	struct NodeValuesCapture {
		std::string_view NodeId;
		std::vector<AuthoredValue> *Values = nullptr;
		detail::AllocationReservation *Charge = nullptr;
	};

	static bool AddBytes(uint64_t &total, uint64_t bytes) {
		if (bytes > std::numeric_limits<uint64_t>::max() - total) return false;
		total += bytes;
		return true;
	}
	static bool AddArrayBytes(uint64_t &total, size_t count, size_t elementBytes) {
		if (elementBytes != 0 && count > std::numeric_limits<uint64_t>::max() / elementBytes) return false;
		return AddBytes(total, static_cast<uint64_t>(count) * elementBytes);
	}

	static bool CaptureNodeInputs(
		detail::NodeContext &context,
		NodeInputCapture &capture,
		std::span<NodeResult> results,
		std::span<detail::AllocationReservation> resultCharges
	) {
		*capture.SurfacePolicy = context.InheritedSurfaceFormat;
		if (capture.InterpolationPolicy) *capture.InterpolationPolicy = context.InheritedInterpolation;
		if (!context.ImageArrays.empty() && !capture.ImageArrays)
			return context.Fail(
				Status::UnsupportedExecution, "snapshot owner has no image-array storage", capture.NodeId
			);
		const auto inputArray = [&](std::string_view port) -> const ImageArray * {
			const auto found =
				std::find_if(context.ImageArrays.begin(), context.ImageArrays.end(), [&](const auto &entry) {
					return entry.first == port;
				});
			return found == context.ImageArrays.end() ? nullptr : found->second;
		};
		for (const auto &[port, array] : context.ImageArrays) {
			if (!array || array->Images.size() > Limits::MaximumArrayElements ||
				RetainedImageArrayBytes(*array) == std::numeric_limits<uint64_t>::max())
				return context.Fail(
					Status::LimitExceeded, "node image-array snapshot exceeds native bounds", port
				);
			const auto validItems = [&](auto &&self, const std::vector<ImageArrayItem> &items) -> bool {
				for (const auto &item : items) {
					if (const auto *image = std::get_if<size_t>(&item.Data)) {
						if (*image >= array->Images.size()) return false;
					} else if (!self(self, std::get<std::vector<ImageArrayItem>>(item.Data)))
						return false;
				}
				return true;
			};
			if (!validItems(validItems, array->Items))
				return context.Fail(
					Status::InvalidValue, "node image-array snapshot has an invalid image index", port
				);
			for (const auto &image : array->Images)
				if (!ValidSurfaceLayout(
						image, context.Request.MaximumImageDimension, Limits::MaximumEvaluationBytes
					) ||
					!FiniteSurfaceSamples(image))
					return context.Fail(
						Status::InvalidValue, "node image-array snapshot has invalid surface samples", port
					);
		}
		// Use the same first-port-wins rule as snapshot construction when proving
		// pointer uniqueness.
		const auto ports = [&](const auto &visit) {
			for (size_t i = 0; i < context.Entry.Inputs.size(); ++i) {
				const auto id = context.Entry.Inputs[i].Id;
				bool duplicate = false;
				for (size_t j = 0; j < i; ++j)
					if (context.Entry.Inputs[j].Id == id) duplicate = true;
				if (!duplicate && !visit(id)) return false;
			}
			for (size_t i = 0; i < context.Authored.DynamicInputs.size(); ++i) {
				const auto &id = context.Authored.DynamicInputs[i].Id;
				bool duplicate = false;
				for (const auto &input : context.Entry.Inputs)
					if (input.Id == id) duplicate = true;
				for (size_t j = 0; j < i; ++j)
					if (context.Authored.DynamicInputs[j].Id == id) duplicate = true;
				if (!duplicate && !visit(std::string_view(id))) return false;
			}
			return true;
		};
		const auto ownedAudio = [&](const Value *value) -> std::optional<detail::SnapshotAudioMove> {
			if (!value || !std::holds_alternative<AudioBit>(*value)) return std::nullopt;
			size_t aliases = 0;
			ports([&](std::string_view port) {
				if (context.Find(port) == value) ++aliases;
				return true;
			});
			if (aliases != 1) return std::nullopt;
			for (size_t i = 0; i < results.size(); ++i) {
				auto *output = FindValueOutputs(results[i]);
				if (!output) continue;
				for (auto &entry : *output)
					if (&entry.Data == value) {
						auto &audio = std::get<AudioBit>(entry.Data);
						const uint64_t bytes = detail::RetainedPayloadBytes(audio);
						if (bytes == 0) return std::nullopt;
						return detail::SnapshotAudioMove{&audio, nullptr, &resultCharges[i], bytes};
					}
			}
			return std::nullopt;
		};
		uint64_t bytes = 0;
		size_t moveCount = 0;
		if (!ports([&](std::string_view port) {
				if (const Value *value = context.Find(port)) {
					if (!AddBytes(bytes, std::max(port.size(), std::string{}.capacity()))) return false;
					if (ownedAudio(value))
						++moveCount;
					else if (!AddBytes(bytes, detail::RetainedPayloadBytes(*value)))
						return false;
				}
				if (const Image *image = context.Input(port)) {
					if (!AddBytes(bytes, std::max(port.size(), std::string{}.capacity())) ||
						!AddBytes(bytes, image->Pixels.capacity()))
						return false;
				}
				if (const auto *array = inputArray(port)) {
					if (!AddBytes(bytes, std::max(port.size(), std::string{}.capacity())) ||
						!AddBytes(bytes, RetainedImageArrayBytes(*array)))
						return false;
				}
				return true;
			}))
			return context.Fail(Status::LimitExceeded, "node input snapshot size overflow", capture.NodeId);
		const size_t valueSlots = context.Entry.Inputs.size() + context.Authored.DynamicInputs.size();
		const size_t imageSlots = context.Images.size();
		const size_t arraySlots = context.ImageArrays.size();
		if (!AddArrayBytes(bytes, valueSlots, sizeof(EvaluationInputValue)) ||
			!AddArrayBytes(bytes, imageSlots, sizeof(EvaluationInputImage)) ||
			!AddArrayBytes(bytes, arraySlots, sizeof(SnapshotImageArray)))
			return context.Fail(Status::LimitExceeded, "node input snapshot size overflow", capture.NodeId);
		auto charge = context.ReserveWorkspace(bytes, capture.NodeId);
		if (!charge || !capture.Charge->Merge(std::move(*charge)))
			return context.Fail(
				Status::LimitExceeded, "node input snapshot exceeds the evaluation budget", capture.NodeId
			);
		uint64_t moveBytes = 0;
		if (!AddArrayBytes(moveBytes, moveCount, sizeof(detail::SnapshotAudioMove)))
			return context.Fail(
				Status::LimitExceeded, "node input snapshot move size overflow", capture.NodeId
			);
		auto moveCharge = context.ReserveWorkspace(moveBytes, capture.NodeId);
		if (!moveCharge) return false;
		std::vector<detail::SnapshotAudioMove> moves;
		moves.reserve(moveCount);
		if (!moveCharge->Resize(moves.capacity() * sizeof(detail::SnapshotAudioMove)))
			return context.Fail(
				Status::LimitExceeded, "node input snapshot move workspace exceeds cap", capture.NodeId
			);
		capture.Values->reserve(valueSlots);
		capture.Images->reserve(imageSlots);
		if (capture.ImageArrays) capture.ImageArrays->reserve(arraySlots);
		// No source payload changes until all strings, vector capacities and copied
		// inputs are admitted.
		if (!ports([&](std::string_view port) {
				if (const Value *value = context.Find(port)) {
					auto move = ownedAudio(value);
					if (move) {
						capture.Values->push_back(
							{std::string(port), AudioBit{}, context.IsLinked(port), context.InputDomain(port)}
						);
						move->Target = &std::get<AudioBit>(capture.Values->back().Data);
						moves.push_back(*move);
					} else
						capture.Values->push_back(
							{std::string(port), *value, context.IsLinked(port), context.InputDomain(port)}
						);
				}
				if (const Image *image = context.Input(port))
					capture.Images->push_back({std::string(port), *image, context.InputDomain(port)});
				if (const auto *array = inputArray(port))
					capture.ImageArrays->push_back({std::string(port), *array, context.InputDomain(port)});
				return true;
			}))
			return false;
		if (capture.ExternalBoundary)
			for (auto &value : *capture.Values)
				detail::StripSourcePathShiftIdentities(value.Data);
		uint64_t actual = 0;
		if (!AddArrayBytes(actual, capture.Values->capacity(), sizeof(EvaluationInputValue)) ||
			!AddArrayBytes(actual, capture.Images->capacity(), sizeof(EvaluationInputImage)) ||
			(capture.ImageArrays &&
			 !AddArrayBytes(actual, capture.ImageArrays->capacity(), sizeof(SnapshotImageArray))))
			return context.Fail(
				Status::LimitExceeded, "node input snapshot actual size overflow", capture.NodeId
			);
		for (const auto &value : *capture.Values)
			if (!AddBytes(actual, value.Port.capacity()) ||
				!AddBytes(actual, detail::RetainedPayloadBytes(value.Data)))
				return context.Fail(
					Status::LimitExceeded, "node input snapshot actual size overflow", value.Port
				);
		for (const auto &image : *capture.Images)
			if (!AddBytes(actual, image.Port.capacity()) || !AddBytes(actual, image.Data.Pixels.capacity()))
				return context.Fail(
					Status::LimitExceeded, "node input snapshot actual size overflow", image.Port
				);
		if (capture.ImageArrays)
			for (const auto &array : *capture.ImageArrays)
				if (!AddBytes(actual, array.Port.capacity()) ||
					!AddBytes(actual, RetainedImageArrayBytes(array.Data)))
					return context.Fail(
						Status::LimitExceeded, "node image-array snapshot actual size overflow", array.Port
					);
		if (actual > bytes) {
			auto excess = context.ReserveWorkspace(actual - bytes, capture.NodeId);
			if (!excess || !capture.Charge->Merge(std::move(*excess)))
				return context.Fail(
					Status::LimitExceeded, "node input snapshot actual capacities exceed cap", capture.NodeId
				);
		}
		if (!detail::PrepareSnapshotAudioMoves(moves, *capture.Charge))
			return context.Fail(
				Status::LimitExceeded,
				"node input snapshot audio ownership transfer is not admitted",
				capture.NodeId
			);
		detail::CommitSnapshotAudioMoves(moves, *capture.Charge);
		return true;
	}

	static const Value *SourceEmptyGroupVectorView(
		const Document *document,
		const Node &node,
		std::string_view port,
		const Value *value,
		std::optional<bool> getterAnimated = std::nullopt
	);

	static std::optional<Quaternion> SourceQuaternionGetterProjection(
		const Document &document,
		std::string_view nodeId,
		std::string_view port,
		const Value &value,
		bool linked = false
	) {
		const auto target = std::find_if(document.Nodes.begin(), document.Nodes.end(), [&](const auto &node) {
			return node.Id == nodeId;
		});
		if (target == document.Nodes.end()) return std::nullopt;
		const auto *entry = FindCatalogueEntry(target->Type);
		const auto *input = entry ? FindCatalogueInput(*entry, port) : nullptr;
		if (!input || input->SourceIndex < 0 || input->Type != ValueType::Quaternion) return std::nullopt;
		// Quaternion.getValue processes the raw tuple with the receiving prop, after instance lookup.
		const Node *getter = &*target;

		const auto track =
			std::find_if(document.Tracks.begin(), document.Tracks.end(), [&](const auto &track) {
				return track.NodeId == getter->Id && track.Port == port && track.QuaternionMode;
			});
		const auto *tuple = std::get_if<Quaternion>(&value);
		Quaternion linkedTuple;
		if (!tuple && linked)
			if (const auto *vector = std::get_if<Vector4>(&value)) {
				linkedTuple = {vector->X, vector->Y, vector->Z, vector->W};
				tuple = &linkedTuple;
			}
		Quaternion converted;
		if (tuple && track != document.Tracks.end() &&
			ConvertSourceQuaternion(*tuple, *track->QuaternionMode, converted))
			return converted;
		return std::nullopt;
	}

	static bool SourceQuaternionKeysPresent(
		const Document &document,
		const GroupReplayState *replay,
		std::string_view nodeId,
		std::string_view port
	) {
		const auto *binding = replay ? replay->Binding(nodeId, port) : nullptr;
		const auto ownerId = binding ? std::string_view(binding->OwnerId) : nodeId;
		const auto *overlay = replay ? replay->SharedSubtype(ownerId, port) : nullptr;
		if (overlay) return !overlay->Keys.empty();
		return std::any_of(document.Keyframes.begin(), document.Keyframes.end(), [&](const auto &key) {
			return key.NodeId == ownerId && key.Port == port;
		});
	}

	static const Node *
	EffectiveInputOwner(const Document &document, const Node &start, std::string_view port) {
		const Node *current = &start;
		for (size_t hop = 0; hop < document.Nodes.size(); ++hop) {
			if (std::find(current->InstanceOverrides.begin(), current->InstanceOverrides.end(), port) !=
				current->InstanceOverrides.end())
				return current;
			if (detail::SourceInputInstanceBase(*current, port).empty()) return current;
			const auto base =
				std::find_if(document.Nodes.begin(), document.Nodes.end(), [&](const auto &node) {
					return node.Id == detail::SourceInputInstanceBase(*current, port) &&
						   node.Type == current->Type;
				});
			if (base == document.Nodes.end()) return nullptr;
			current = &*base;
		}
		return nullptr;
	}

#include "SourceCommonInputMaps.inc"

	static const Node *SourceExpressionOwner(
		const Document &document,
		const Plan &plan,
		const Node &local,
		std::string_view port,
		const GroupReplayState *replay
	) {
		const auto *binding = replay ? replay->Binding(local.Id, port) : nullptr;
		const Node *owner = EffectiveInputOwner(document, local, port);
		if (binding && replay->InstancesBound() &&
			!detail::InheritedMovedSourceGetter(local, port, binding)) {
			const auto bound =
				std::find_if(document.Nodes.begin(), document.Nodes.end(), [&](const auto &node) {
					return node.Id == binding->OwnerId;
				});
			owner = bound == document.Nodes.end() ? nullptr : &*bound;
		}
		if (binding && detail::MovedSourceAnimator(*binding)) {
			const bool linked =
				detail::CommonInputRoute(document, plan, local.Id, port) ||
				std::any_of(
					plan.EffectiveLinks.begin(),
					plan.EffectiveLinks.end(),
					[&](const auto &link) { return link.ToNode == local.Id && link.ToPort == port; }
				) ||
				std::any_of(plan.ResolvedInputs.begin(), plan.ResolvedInputs.end(), [&](const auto &input) {
					return input.NodeId == local.Id && input.Port == port;
				});
			owner = linked ? &local : EffectiveInputOwner(document, local, port);
		}
		return owner;
	}

	// Evaluation borrows durable names and keeps one exact-capacity index buffer.
	class EvaluationNodeIndices {
	  public:
		explicit EvaluationNodeIndices(const Document &document) {
			Entries.reserve(document.Nodes.size());
			for (size_t index = 0; index < document.Nodes.size(); ++index)
				Entries.emplace_back(document.Nodes[index].Id, index);
			std::sort(Entries.begin(), Entries.end());
		}
		std::optional<size_t> find(std::string_view id) const {
			const auto found = std::lower_bound(
				Entries.begin(), Entries.end(), id, [](const auto &entry, std::string_view name) {
					return entry.first < name;
				}
			);
			return found == Entries.end() || found->first != id ? std::nullopt
																: std::optional<size_t>{found->second};
		}
		size_t at(std::string_view id) const {
			const auto found = std::lower_bound(
				Entries.begin(), Entries.end(), id, [](const auto &entry, std::string_view name) {
					return entry.first < name;
				}
			);
			assert(found != Entries.end() && found->first == id);
			return found->second;
		}

	  private:
		std::vector<std::pair<std::string_view, size_t>> Entries;
	};

	// Source checkTunnels runs before this frame updates producer outputs.
	static Status ResolveSourceTunnelRegistry(
		const Document &document,
		const EvaluationRequest &request,
		Plan &plan,
		detail::EvaluationBudget &budget,
		detail::AllocationReservation &planCharge,
		Diagnostic &diagnostic,
		bool priorAlreadyAdmitted = false,
		std::vector<SourceTunnelRegistryObservation> *registry = nullptr,
		const Node *getterNode = nullptr,
		CacheGroupReplayOutput *getterOutput = nullptr,
		std::span<const NodeResult> getterResults = {},
		std::span<const uint8_t> getterProduced = {},
		DataReplayState *getterData = nullptr,
		std::string_view getterPort = "value_in",
		uint64_t *sharedWork = nullptr
	) {
		if (!detail::ValidateSourceTunnelRegistryObservations(
				document, request.SourceTunnelRegistryObservations, diagnostic
			))
			return diagnostic.Code;
		if (!std::any_of(document.Nodes.begin(), document.Nodes.end(), [](const Node &node) {
				return node.Type == "pc.tunnel_in" || node.Type == "pc.tunnel_out";
			}))
			return Status::Ok;
		const uint64_t indexBytes = document.Nodes.size() * sizeof(std::pair<std::string_view, size_t>);
		auto indexCharge = budget.Reserve(indexBytes);
		if (!indexCharge) {
			diagnostic = {Status::LimitExceeded, {}, {}, "tunnel registry index exceeds live bytes"};
			return diagnostic.Code;
		}
		const EvaluationNodeIndices indices(document);
		uint64_t localWork = 0;
		uint64_t &work = sharedWork ? *sharedWork : localWork;
		const auto spend = [&](uint64_t count) {
			if (count > 64'000'000 - work) {
				diagnostic = {Status::LimitExceeded, {}, {}, "tunnel registry getter work exceeds bounds"};
				return false;
			}
			work += count;
			return true;
		};
		uint64_t observationBytes =
			request.SourceTunnelRegistryObservations.size() * sizeof(SourceTunnelRegistryObservation);
		for (const auto &row : request.SourceTunnelRegistryObservations)
			observationBytes += row.NodeId.capacity() + 2 * row.Name.capacity();
		auto observations = budget.Reserve(observationBytes);
		if (!observations) {
			diagnostic = {Status::LimitExceeded, {}, {}, "tunnel registry observations exceed live bytes"};
			return diagnostic.Code;
		}
		const auto *prior = request.SourceTunnelPreviousOutputs;
		if (!prior && request.GroupRender) prior = &request.GroupRender->Outputs;
		if (!prior && request.DataReplay) prior = &request.DataReplay->CacheGroups;
		detail::AllocationReservation priorCharge;
		if (prior) {
			if (ValidateCacheGroupReplay(*prior, budget.Available(), diagnostic) != Status::Ok)
				return diagnostic.Code;
			const bool alreadyAdmitted = priorAlreadyAdmitted ||
										 (request.GroupRender && prior == &request.GroupRender->Outputs) ||
										 (request.DataReplay && prior == &request.DataReplay->CacheGroups);
			auto admitted = budget.Reserve(alreadyAdmitted ? 0 : RetainedCacheGroupReplayBytes(*prior));
			if (!admitted) {
				diagnostic = {
					Status::LimitExceeded, {}, {}, "tunnel registry prior outputs exceed live bytes"
				};
				return diagnostic.Code;
			}
			priorCharge = std::move(*admitted);
		}
		if (request.DataReplay) {
			auto validation = budget.Reserve(DataReplayValidationWorkspaceBytes(*request.DataReplay));
			if (!validation) {
				diagnostic = {
					Status::LimitExceeded, {}, {}, "tunnel callback journal validation exceeds live bytes"
				};
				return diagnostic.Code;
			}
			if (ValidateDataReplay(*request.DataReplay, Limits::MaximumEvaluationBytes, diagnostic) !=
				Status::Ok)
				return diagnostic.Code;
		}
		CacheGroupReplayState constructors;
		detail::AllocationReservation constructorCharge;
		bool initialized = false;
		const auto constructor =
			[&](const Node &node, std::string_view port, Diagnostic &failure) -> const Value * {
			if (!initialized) {
				if (detail::InitializeGroupRenderOutputs(
						document, {}, constructors, budget.Available(), failure
					) != Status::Ok)
					return nullptr;
				auto admitted = budget.Reserve(RetainedCacheGroupReplayBytes(constructors));
				if (!admitted) {
					failure = {
						Status::LimitExceeded,
						node.Id,
						std::string(port),
						"tunnel constructor outputs exceed live bytes"
					};
					return nullptr;
				}
				constructorCharge = std::move(*admitted);
				initialized = true;
			}
			for (const auto &row : constructors.Nodes) {
				if (!spend(1 + row.Outputs.size() + std::min(row.NodeId.size(), node.Id.size())))
					return nullptr;
				if (row.NodeId != node.Id) continue;
				for (const auto &output : row.Outputs) {
					if (!spend(1 + std::min(output.Port.size(), port.size()))) return nullptr;
					if (output.Port != port) continue;
					if (output.Refusal) {
						failure = *output.Refusal;
						return nullptr;
					}
					if (output.Data) return &*output.Data;
				}
			}
			failure = {
				Status::UnsupportedExecution,
				node.Id,
				std::string(port),
				"tunnel selector constructor needs a source observation"
			};
			return nullptr;
		};
		detail::EvaluationVector<uint8_t> needed(
			document.Nodes.size(), 0, detail::EvaluationAllocator<uint8_t>(budget)
		);
		detail::EvaluationVector<uint8_t> getters(
			document.Nodes.size(), 0, detail::EvaluationAllocator<uint8_t>(budget)
		);
		for (size_t index = 0; index < document.Nodes.size(); ++index) {
			const auto &node = document.Nodes[index];
			if (node.Type != "pc.tunnel_in" && node.Type != "pc.tunnel_out") continue;
			getters[index] = needed[index] = 1;
			for (const auto port : {std::string_view{"name"}, std::string_view{"scope"}}) {
				if (port == "scope" && node.Type == "pc.tunnel_out") continue;
				const Node *owner = SourceExpressionOwner(document, plan, node, port, request.GroupReplay);
				if (!owner) {
					diagnostic = {
						Status::InvalidValue,
						node.Id,
						std::string(port),
						"tunnel selector owner is unavailable"
					};
					return diagnostic.Code;
				}
				needed[indices.at(owner->Id)] = 1;
			}
		}
		detail::TimelineOverrides timeline;
		if (detail::ResolveTimelineOverrides(
				document, needed, request, budget, timeline, diagnostic, {}, true, getters
			) != Status::Ok)
			return diagnostic.Code;
		struct PreRenderNames final : PcxNameResolver {
			std::function<bool(std::string_view, Value &, Diagnostic &)> Lookup;
			bool Resolve(std::string_view name, Value &value, Diagnostic &failure) const override {
				return Lookup(name, value, failure);
			}
		};
		using InputKey = std::pair<size_t, std::string>;
		using InputRow = std::pair<const InputKey, Value>;
		std::map<InputKey, Value, std::less<InputKey>, detail::EvaluationAllocator<InputRow>> memo{
			std::less<InputKey>{}, detail::EvaluationAllocator<InputRow>(budget)
		};
		std::set<InputKey, std::less<InputKey>, detail::EvaluationAllocator<InputKey>> active{
			std::less<InputKey>{}, detail::EvaluationAllocator<InputKey>(budget)
		};
		detail::AllocationReservation memoPayload;
		std::function<const Value *(const Node &, std::string_view)> readInput;
		const auto observeOutput =
			[&](const Node &node, std::string_view port, const Link &link) -> const Value * {
			if (getterNode && !getterProduced.empty()) {
				const auto source = indices.at(link.FromNode);
				if (getterProduced[source]) {

					// Tunnel dispatch keeps produced rows live until every receiver has read them.
					const auto &result = getterResults[source];
					if (const auto *failure = FindOutputDiagnostic(result, link.FromPort)) {
						diagnostic = *failure;
						return nullptr;
					}
					if (const auto *values = FindValueOutputs(result))
						for (const auto &value : *values)
							if (value.Port == link.FromPort) return &value.Data;
					if (const auto *image = FindImageOutput(result, link.FromPort)) {
						auto charge = budget.Reserve(
							sizeof(Value) + image->Pixels.size() +
							4 * (std::string_view{"$getter.surface."}.size() + link.FromPort.size())
						);
						if (!charge || !memoPayload.Merge(std::move(*charge))) {
							diagnostic = {
								Status::LimitExceeded,
								node.Id,
								std::string(port),
								"getter surface snapshot exceeds live bytes"
							};
							return nullptr;
						}
						const InputKey key{source, "$getter.surface." + link.FromPort};
						return &memo.insert_or_assign(key, SurfaceValue{*image}).first->second;
					}
					if (const auto *images = FindImageArrayOutput(result, link.FromPort)) {
						detail::source_array::TreeCost cost;
						if (images->Items.empty()) {
							if (images->Images.size() > Limits::MaximumArrayElements) {
								diagnostic = {
									Status::LimitExceeded,
									node.Id,
									std::string(port),
									"getter image-array shape exceeds bounds"
								};
								return nullptr;
							}
							cost.Nodes = images->Images.size();
							cost.Bytes = cost.Nodes * sizeof(SourceArrayItem);
							if (cost.Bytes > Limits::MaximumArrayBytes) {
								diagnostic = {
									Status::LimitExceeded,
									node.Id,
									std::string(port),
									"getter image-array item storage exceeds bounds"
								};
								return nullptr;
							}
							for (const auto &image : images->Images) {
								if (image.Pixels.size() > Limits::MaximumArrayBytes - cost.Bytes) {
									diagnostic = {
										Status::LimitExceeded,
										node.Id,
										std::string(port),
										"getter image-array payload exceeds bounds"
									};
									return nullptr;
								}
								cost.Bytes += image.Pixels.size();
							}
						} else if (!detail::source_array::ImageCost(*images, images->Items, cost, 1)) {
							diagnostic = {
								Status::LimitExceeded,
								node.Id,
								std::string(port),
								"getter image-array shape exceeds bounds"
							};
							return nullptr;
						}
						if (!spend(cost.Nodes)) return nullptr;
						auto conversion = budget.Reserve(
							2 * cost.Bytes + sizeof(Value) + sizeof(ArrayValue) +
							4 * (std::string_view{"$getter.images."}.size() + link.FromPort.size())
						);
						if (!conversion) {
							diagnostic = {
								Status::LimitExceeded,
								node.Id,
								std::string(port),
								"getter array conversion exceeds live bytes"
							};
							return nullptr;
						}
						Value array;
						if (!PcxEvaluationNames::Images(*images, array, diagnostic)) return nullptr;
						const InputKey key{source, "$getter.images." + link.FromPort};
						const auto bytes = ValueClonePayloadBytes(array);
						if (!bytes || !conversion->Resize(*bytes + 2 * key.second.capacity()) ||
							!memoPayload.Merge(std::move(*conversion))) {
							diagnostic = {
								Status::LimitExceeded,
								node.Id,
								std::string(port),
								"getter image-array snapshot exceeds live bytes"
							};
							return nullptr;
						}
						return &memo.insert_or_assign(key, std::move(array)).first->second;
					}
					diagnostic = {
						Status::InvalidOutput,
						link.FromNode,
						link.FromPort,
						"processed tunnel getter producer did not publish its port"
					};
					return nullptr;
				}
			}
			return detail::FindSourceTunnelPriorValue(document, link, prior, constructor, diagnostic, work);
		};
		const auto rawInput = [&](const Node &node, std::string_view port) -> const Value * {
			if (const auto *route = detail::CommonInputRoute(document, plan, node.Id, port)) {
				Value held;
				detail::AllocationReservation heldCharge;
				if (detail::ReadSourceCommonGetter(
						document,
						plan,
						route->OwnerId,
						route->Selector,
						request,
						budget,
						held,
						heldCharge,
						diagnostic
					) != Status::Ok)
					return nullptr;
				auto identity =
					budget.Reserve(std::max(port.size(), std::string{}.capacity()) + sizeof(InputKey));
				if (!identity || !memoPayload.Merge(std::move(*identity)) ||
					!memoPayload.Merge(std::move(heldCharge))) {
					SetDiagnostic(
						diagnostic,
						Status::LimitExceeded,
						"common pre-render getter exceeds live bytes",
						node.Id,
						std::string(port)
					);
					return nullptr;
				}
				return &memo.insert_or_assign(
								InputKey{indices.at(node.Id), std::string(port)}, std::move(held)
				)
							.first->second;
			}
			const Node *owner = SourceExpressionOwner(document, plan, node, port, request.GroupReplay);
			if (!owner) {
				diagnostic = {
					Status::InvalidValue, node.Id, std::string(port), "pre-render input owner is unavailable"
				};
				return nullptr;
			}
			if (!spend(plan.EffectiveLinks.size() + plan.ResolvedInputs.size())) return nullptr;
			for (const auto &link : plan.EffectiveLinks) {
				if ((link.ToNode != owner->Id && link.ToNode != node.Id) || link.ToPort != port) continue;
				return observeOutput(node, port, link);
			}
			for (const auto &input : plan.ResolvedInputs)
				if ((input.NodeId == node.Id || input.NodeId == owner->Id) && input.Port == port)
					return &input.Data;
			needed[indices.at(owner->Id)] = 1;
			getters[indices.at(node.Id)] = 1;
			if (detail::ExtendTimelineOverrides(
					document, needed, request, budget, timeline, diagnostic, true, getters
				) != Status::Ok)
				return nullptr;
			const auto &sampled = timeline.Find(indices.at(owner->Id), *owner);
			if (const auto *stored = FindValue(sampled, port)) return &stored->Data;
			return nullptr;
		};
		const auto copyMemo = [&](const InputKey &key, const Value &value) -> const Value * {
			const auto bytes = ValueClonePayloadBytes(value);
			auto admission = bytes ? budget.Reserve(*bytes + key.second.capacity()) : std::nullopt;
			if (!admission || !memoPayload.Merge(std::move(*admission))) {
				diagnostic = {
					Status::LimitExceeded,
					document.Nodes[key.first].Id,
					std::string(key.second),
					"pre-render input snapshots exceed live bytes"
				};
				return nullptr;
			}
			return &memo.emplace(key, value).first->second;
		};
		PreRenderNames names;
		names.Lookup = [&](std::string_view name, Value &value, Diagnostic &failure) {
			if (!spend(1 + name.size() + document.Nodes.size())) {
				failure = diagnostic;
				return false;
			}
			if (name.starts_with("Project.") || name.starts_with("Program.") || name.starts_with("Device."))
				return false;
			const auto first = name.find('.'),
					   second = first == std::string_view::npos ? first : name.find('.', first + 1);
			const Node *producer = nullptr;
			std::string port;
			bool input = false;
			if (second != std::string_view::npos) {
				const auto owner = name.substr(0, first);
				std::string direction(name.substr(first + 1, second - first - 1));
				port = name.substr(second + 1);
				if (const auto extra = port.find('.'); extra != std::string::npos) port.resize(extra);
				for (char &c : direction)
					if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
				for (char &c : port)
					if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
				input = direction == "input" || direction == "inputs";
				if (!input && direction != "output" && direction != "outputs") {
					value = double{0};
					return true;
				}
				for (auto node = document.Nodes.rbegin(); node != document.Nodes.rend(); ++node) {
					if (!spend(1 + std::min(owner.size(), node->SourceInternalName.size()))) {
						failure = diagnostic;
						return false;
					}
					if (!owner.empty() && node->SourceInternalName == owner) {
						producer = &*node;
						break;
					}
				}
			} else if (first == std::string_view::npos && !document.ProjectGlobalNodeId.empty()) {
				const auto global = indices.at(document.ProjectGlobalNodeId);
				const auto &node = document.Nodes[global];
				for (const auto &slot : node.DynamicInputs)
					if (slot.Id == name) {
						producer = &node;
						port = slot.Id;
						input = true;
						break;
					}
			}
			if (!producer) {
				value = double{0};
				return true;
			}
			const Value *observed = nullptr;
			if (input)
				observed = readInput(*producer, port);
			else {
				Link link{producer->Id, port, {}, {}};
				observed = observeOutput(*producer, port, link);
			}
			if (!observed) {
				failure = diagnostic;
				if (diagnostic.Code == Status::Ok) value = double{0};
				return diagnostic.Code == Status::Ok;
			}
			if (!PcxEvaluationNames::Copy(*observed, value, failure)) return false;
			return true;
		};
		readInput = [&](const Node &node, std::string_view port) -> const Value * {
			if (!spend(1 + port.size())) return nullptr;
			auto identityCharge = budget.Reserve(2 * port.size() + sizeof(InputKey));
			if (!identityCharge) {
				diagnostic = {
					Status::LimitExceeded,
					node.Id,
					std::string(port),
					"pre-render getter identities exceed live bytes"
				};
				return nullptr;
			}
			const InputKey key{indices.at(node.Id), std::string(port)};
			if (const auto found = memo.find(key); found != memo.end()) return &found->second;
			if (active.size() >= 64 || active.contains(key)) {
				diagnostic = {
					Status::Cycle, node.Id, std::string(port), "pre-render input expressions form a cycle"
				};
				return nullptr;
			}
			active.insert(key);
			const auto *entry = FindCatalogueEntry(node.Type);
			if (!entry) {
				const auto *raw = rawInput(node, port);
				active.erase(key);
				return raw ? copyMemo(key, *raw) : nullptr;
			}
			auto views = budget.Reserve(
				(entry->Inputs.size() + node.DynamicInputs.size()) *
				(sizeof(Value) + sizeof(std::pair<std::string_view, const Value *>))
			);
			if (!views) {
				diagnostic = {
					Status::LimitExceeded,
					node.Id,
					std::string(port),
					"pre-render getter views exceed live bytes"
				};
				return nullptr;
			}
			detail::NodeContext context(node, *entry, request, budget);
			context.EvaluationDocument = &document;
			context.Timeline = document.Timeline ? &*document.Timeline : nullptr;
			context.PcxNames = &names;
			if (document.Project) {
				context.Project.SurfaceWidth = document.Project->SurfaceWidth;
				context.Project.SurfaceHeight = document.Project->SurfaceHeight;
			}
			context.InputProvenanceResolved = true;
			const auto append = [&](std::string_view slot,
									const CatalogueInput *declared,
									const std::optional<Value> *fallback) {
				const auto *raw = rawInput(node, slot);
				if (!raw && diagnostic.Code != Status::Ok) return false;
				if (raw)
					context.ValueViews.emplace_back(slot, raw);
				else if (fallback && *fallback)
					context.Values.emplace_back(slot, **fallback);
				else if (declared) {
					if (auto value = CatalogueDefault(*declared))
						context.Values.emplace_back(slot, std::move(*value));
					else if ((node.Type == "pc.tunnel_in" || node.Type == "pc.tunnel_out") && slot == "name")
						context.Values.emplace_back(slot, std::string{});
					else if (node.Type == "pc.tunnel_in" && slot == "value_in")
						context.Values.emplace_back(slot, int64_t{-4});
					context.CatalogueDefaultInputs.push_back(slot);
				}
				if (const auto *route = detail::CommonInputRoute(document, plan, node.Id, slot)) {
					context.LinkedValues.push_back(slot);
					context.InputDomains.emplace_back(slot, detail::CommonSelectorDomain(route->Selector));
				}
				for (const auto &link : plan.EffectiveLinks)
					if (link.ToNode == node.Id && link.ToPort == slot) {
						context.LinkedValues.push_back(slot);
						break;
					}
				return true;
			};
			const Node *owner = SourceExpressionOwner(document, plan, node, port, request.GroupReplay);
			if (!owner) return nullptr;
			const SourceInputExpression *expression = nullptr;
			for (const auto &program : owner->SourceInputExpressions)
				if (program.Port == port && program.Enabled) {
					expression = &program;
					break;
				}
			bool wholeInputMap = false;
			if (expression && !expression->Code.empty()) {
				auto parseCharge = budget.Reserve(
					expression->Code.size() * 8 + 4096 * sizeof(PcxInstruction) +
					4 * Limits::MaximumArrayBytes
				);
				if (!parseCharge) {
					diagnostic = {
						Status::LimitExceeded,
						node.Id,
						std::string(port),
						"pre-render expression parsing exceeds live bytes"
					};
					return nullptr;
				}
				PcxExpressionValue tree;
				if (CompilePcxProgram(expression->Code, tree, diagnostic) != Status::Ok) return nullptr;
				for (const auto &instruction : tree.Data->Instructions) {
					const auto *name = std::get_if<std::string>(&instruction.Literal);
					wholeInputMap |= instruction.Operation == "name" && name &&
									 (*name == "self" || name->starts_with("self.") ||
									  *name == "node_values" || name->starts_with("node_values."));
				}
			}
			for (const auto &slot : entry->Inputs)
				if ((wholeInputMap || slot.Id == port) && !append(slot.Id, &slot, nullptr)) return nullptr;
			for (const auto &slot : node.DynamicInputs)
				if ((wholeInputMap || slot.Id == port) && !append(slot.Id, nullptr, &slot.Default))
					return nullptr;
			if (expression) {
				const std::array programs{detail::PcxInputProgram{owner, expression}};
				if (!detail::ApplyPcxInputExpressions(context, programs)) {
					diagnostic = {context.FailureCode, node.Id, context.FailurePort, context.FailureMessage};
					return nullptr;
				}
			}
			detail::SourceGetterProjection projection(context);
			if (!projection.Prepare()) {
				diagnostic = {context.FailureCode, node.Id, context.FailurePort, context.FailureMessage};
				return nullptr;
			}
			const auto *value = context.Find(port);
			const Value *owned = value ? copyMemo(key, *value) : nullptr;
			active.erase(key);
			return owned;
		};

		if (getterNode) {
			if (!getterOutput || getterResults.size() != document.Nodes.size() ||
				getterProduced.size() != document.Nodes.size()) {
				diagnostic = {
					Status::InvalidValue,
					getterNode->Id,
					"value_in",
					"getter snapshot masks do not match document"
				};
				return diagnostic.Code;
			}
			if (getterPort == "name") {
				const Node *owner =
					SourceExpressionOwner(document, plan, *getterNode, getterPort, request.GroupReplay);
				if (owner)
					for (const auto &property : owner->SourceProperties) {
						const auto *code = std::get_if<std::string>(&property.Data);
						if (property.Port != "nameExpression" || !code || code->empty()) continue;
						for (const auto &observation : request.SourceTunnelRegistryObservations) {
							if (!spend(1 + std::min(observation.NodeId.size(), getterNode->Id.size())))
								return diagnostic.Code;
							if (observation.NodeId != getterNode->Id) continue;
							auto storage = budget.Reserve(sizeof(Value) + observation.Name.size());
							if (!storage || !planCharge.Merge(std::move(*storage))) {
								diagnostic = {
									Status::LimitExceeded,
									getterNode->Id,
									"name",
									"foreign receiver key exceeds live bytes"
								};
								return diagnostic.Code;
							}
							getterOutput->Port = "name";
							getterOutput->Data = observation.Name;
							return Status::Ok;
						}
						diagnostic = {
							Status::UnsupportedExecution,
							getterNode->Id,
							"name",
							"foreign receiver nameExpression needs a current source registry observation"
						};
						return diagnostic.Code;
					}
			}
			const auto *value = readInput(*getterNode, getterPort);
			if (!value) {
				if (diagnostic.Code == Status::Ok)
					diagnostic = {
						Status::InvalidValue, getterNode->Id, "value_in", "source getter value is absent"
					};
				return diagnostic.Code;
			}
			const auto bytes = ValueClonePayloadBytes(*value);
			auto output = bytes ? budget.Reserve(*bytes) : std::nullopt;
			if (!output || !planCharge.Merge(std::move(*output))) {
				diagnostic = {
					Status::LimitExceeded,
					getterNode->Id,
					"value_in",
					"source getter publication exceeds live bytes"
				};
				return diagnostic.Code;
			}
			getterOutput->Port = getterPort;
			getterOutput->Data = *value;
			const auto *entry = FindCatalogueEntry(getterNode->Type);
			if (!entry) {
				diagnostic = {
					Status::UnknownNode,
					getterNode->Id,
					"value_in",
					"source tunnel getter declaration is missing"
				};
				return diagnostic.Code;
			}
			detail::NodeContext context(*getterNode, *entry, request, budget);
			context.CurrentData = getterData;
			context.ByteBudget = budget.Available();
			if (getterNode->Type != "pc.tunnel_in") return Status::Ok;
			SourceSocketDomain domain;
			if (!detail::SourceTunnelSenderDomain(context, false, domain)) {
				diagnostic = {
					context.FailureCode, getterNode->Id, context.FailurePort, context.FailureMessage
				};
				return diagnostic.Code;
			}
			getterOutput->Domain = domain;
			const Node *owner =
				SourceExpressionOwner(document, plan, *getterNode, "value_in", request.GroupReplay);
			const bool expressed = owner && std::any_of(
												owner->SourceInputExpressions.begin(),
												owner->SourceInputExpressions.end(),
												[](const auto &expression) {
													return expression.Port == "value_in" &&
														   expression.Enabled && !expression.Code.empty();
												}
											);
			const auto marker = [&](const CacheGroupReplayState &state, const Link &link, bool &found) {
				for (const auto &row : state.Nodes) {
					if (!spend(1 + std::min(row.NodeId.size(), link.FromNode.size()) + row.NodeType.size()))
						return false;
					if (row.NodeId != link.FromNode ||
						row.NodeType != document.Nodes[indices.at(link.FromNode)].Type)
						continue;
					for (const auto &port : row.Outputs) {
						if (!spend(1 + std::min(port.Port.size(), link.FromPort.size()))) return false;
						if (port.Port != link.FromPort) continue;
						found = true;
						getterOutput->ImageArrayPayload = port.ImageArrayPayload;
						return true;
					}
				}
				return true;
			};
			if (!expressed)
				for (const auto &link : plan.EffectiveLinks) {
					if (!spend(1 + link.ToPort.size() + link.ToNode.size())) return diagnostic.Code;
					if (link.ToPort != "value_in" ||
						(link.ToNode != getterNode->Id && (!owner || link.ToNode != owner->Id)))
						continue;
					const auto source = indices.at(link.FromNode);
					if (getterProduced[source])
						getterOutput->ImageArrayPayload =
							FindImageArrayOutput(getterResults[source], link.FromPort) != nullptr;
					else {
						bool found = false;
						if (prior && !marker(*prior, link, found)) return diagnostic.Code;
						if (!found && initialized && !marker(constructors, link, found))
							return diagnostic.Code;
					}
					break;
				}
			return Status::Ok;
		}
		// observations borrow caller strings; defaults and sampled rows outlive route construction.
		detail::EvaluationVector<Value> observedValues(
			2 * document.Nodes.size(), Value{int64_t{-4}}, detail::EvaluationAllocator<Value>(budget)
		);
		for (const auto &row : request.SourceTunnelRegistryObservations) {
			const auto index = indices.at(row.NodeId);
			observedValues[2 * index] = row.Name;
			if (row.Scope) observedValues[2 * index + 1] = *row.Scope;
		}
		const auto selector = [&](const Node &node, std::string_view port, const Value *&value) {
			if (node.Type == "pc.tunnel_out" && port == "name") {
				value = nullptr;
				if (!request.DataReplay) return true;
				for (const auto &entry : request.DataReplay->Entries) {
					if (!spend(1 + std::min(node.Id.size(), entry.NodeId.size()))) return false;
					if (entry.NodeId != node.Id || entry.ProcessorRow != 0) continue;
					if (entry.Values.size() != 1 || entry.Values.front().Frame != entry.Tick ||
						CompareFrameTime(
							{entry.Tick, entry.Subframe, entry.NegativeFrame},
							{request.Tick, request.Subframe, request.NegativeFrame}
						) > 0 ||
						!std::holds_alternative<StructValue>(entry.Values.front().Data)) {
						diagnostic = {
							Status::InvalidValue,
							node.Id,
							"name",
							"retained receiver key needs one typed callback record"
						};
						return false;
					}
					const auto &record = std::get<StructValue>(entry.Values.front().Data);
					if (!record.Data || record.Data->Fields.size() != 3) {
						diagnostic = {
							Status::InvalidValue,
							node.Id,
							"name",
							"retained receiver callback fields are invalid"
						};
						return false;
					}
					for (const auto &[field, data] : record.Data->Fields) {
						if (field != "key") continue;
						if (const auto *cold = std::get_if<int64_t>(&data); cold && *cold == -4) return true;
						if (!std::holds_alternative<std::string>(data)) {
							diagnostic = {
								Status::InvalidValue,
								node.Id,
								"name",
								"retained receiver key is neither text nor cold noone"
							};
							return false;
						}
						value = &data;
						return true;
					}
					diagnostic = {Status::InvalidValue, node.Id, "name", "retained receiver key is absent"};
					return false;
				}
				return true;
			}

			if (!spend(document.Nodes.size() + request.SourceTunnelRegistryObservations.size())) return false;
			for (const auto &row : request.SourceTunnelRegistryObservations) {
				if (row.NodeId != node.Id) continue;
				value = &observedValues[2 * indices.at(node.Id) + (port == "scope")];
				return true;
			}
			const Node *owner = SourceExpressionOwner(document, plan, node, port, request.GroupReplay);
			if (!owner) return false;
			if (!spend(
					plan.EffectiveLinks.size() + plan.ResolvedInputs.size() +
					owner->SourceInputExpressions.size() + owner->SourceProperties.size()
				))
				return false;

			for (const auto &property : owner->SourceProperties) {
				if (port != "name" || property.Port != "nameExpression") continue;
				const auto *expression = std::get_if<std::string>(&property.Data);
				if (expression && !expression->empty()) {
					diagnostic = {
						Status::UnsupportedExecution,
						node.Id,
						std::string(port),
						"foreign tunnel nameExpression needs a source registry observation"
					};
					return false;
				}
			}
			value = readInput(node, port);
			return value != nullptr || diagnostic.Code == Status::Ok;
		};
		if (registry) {
			for (const auto &sender : document.Nodes) {
				if (sender.Type != "pc.tunnel_in") continue;
				const Value *name = nullptr, *scope = nullptr;
				if (!selector(sender, "name", name) || !selector(sender, "scope", scope))
					return diagnostic.Code;
				const auto *text = name ? std::get_if<std::string>(name) : nullptr;
				const auto choice = scope ? detail::SourceChoiceNumber(*scope) : std::optional<double>{1};
				if ((name && !text) || !choice) {
					diagnostic = {
						Status::InvalidValue,
						sender.Id,
						"name",
						"tunnel registry selectors require text and source choice"
					};
					return diagnostic.Code;
				}
				auto storage = budget.Reserve(
					2 * sizeof(SourceTunnelRegistryObservation) + sender.Id.size() + (text ? text->size() : 0)
				);
				if (!storage || !planCharge.Merge(std::move(*storage))) {
					diagnostic = {
						Status::LimitExceeded,
						sender.Id,
						"name",
						"tunnel registry snapshots exceed live bytes"
					};
					return diagnostic.Code;
				}
				registry->push_back({sender.Id, text ? *text : std::string{}, choice});
			}
		}
		std::erase_if(plan.PcxNamedDependencies, [](const PcxNamedDependency &route) {
			return route.Name == detail::SourceTunnelRouteName;
		});
		const auto addRoute =
			[&](size_t consumer, size_t producer, std::string_view name, std::string_view port, bool input) {
				if (plan.PcxNamedDependencies.size() >= Limits::MaximumLinks) {
					diagnostic = {
						Status::LimitExceeded,
						document.Nodes[consumer].Id,
						"name",
						"tunnel routes exceed graph bounds"
					};
					return false;
				}
				auto admitted = budget.Reserve(
					2 * sizeof(PcxNamedDependency) + 4 * sizeof(size_t) + name.size() + port.size() + 64
				);
				if (!admitted || !planCharge.Merge(std::move(*admitted))) {
					diagnostic = {
						Status::LimitExceeded,
						document.Nodes[consumer].Id,
						"name",
						"tunnel routes exceed live bytes"
					};
					return false;
				}
				plan.PcxNamedDependencies.push_back(
					{consumer, producer, std::string(name), std::string(port), input}
				);
				return true;
			};
		if (!detail::CompileSourceTunnelRoutes(document, selector, addRoute, diagnostic))
			return diagnostic.Code;
		detail::PendingGraph graph(budget);
		detail::EvaluationVector<const CacheGroupReplayNode *> frozen(
			document.Nodes.size(), nullptr, detail::EvaluationAllocator<const CacheGroupReplayNode *>(budget)
		);
		detail::EvaluationVector<size_t> roots(
			document.Nodes.size(), 0, detail::EvaluationAllocator<size_t>(budget)
		);
		for (size_t index = 0; index < roots.size(); ++index)
			roots[index] = index;
		if (graph.Rebuild(document, plan, {}, frozen, roots, {}, {}, false, work, diagnostic) != Status::Ok)
			return diagnostic.Code;
		detail::EvaluationVector<uint8_t> emitted(
			document.Nodes.size(), 0, detail::EvaluationAllocator<uint8_t>(budget)
		);
		plan.NodeOrder.clear();
		while (plan.NodeOrder.size() < document.Nodes.size()) {
			bool progress = false;
			for (size_t index = 0; index < document.Nodes.size(); ++index) {
				if (emitted[index]) continue;
				if (!spend(1 + graph.Upstream[index].size())) return diagnostic.Code;
				if (!std::all_of(
						graph.Upstream[index].begin(), graph.Upstream[index].end(), [&](size_t source) {
							return emitted[source];
						}
					))
					continue;
				emitted[index] = 1;
				plan.NodeOrder.push_back(index);
				progress = true;
			}
			if (!progress) {
				diagnostic = {Status::Cycle, {}, {}, "selected pre-render tunnel dependencies form a cycle"};
				return diagnostic.Code;
			}
		}
		return Status::Ok;
	}

	std::optional<std::string_view> SourceInputExpressionOwner(
		const Document &document,
		const Plan &plan,
		std::string_view nodeId,
		std::string_view port,
		const GroupReplayState *replay
	) {
		const auto local = std::find_if(document.Nodes.begin(), document.Nodes.end(), [&](const auto &node) {
			return node.Id == nodeId;
		});
		if (local == document.Nodes.end()) return std::nullopt;
		const auto *owner = SourceExpressionOwner(document, plan, *local, port, replay);
		return owner ? std::optional<std::string_view>{owner->Id} : std::nullopt;
	}

	static std::optional<bool> SourceTriggerInputValue(
		const Document &document,
		const Node &node,
		const CatalogueInput &input,
		const EvaluationRequest &request,
		bool &unsupportedClock
	) {
		const auto *entry = request.GroupReplay ? request.GroupReplay->Find(node.Id) : nullptr;
		const auto *type = FindValue(node, "input_type");
		const bool authoredTrigger = type && std::holds_alternative<EnumValue>(type->Data) &&
									 std::get<EnumValue>(type->Data).Value == 19;
		const bool trigger = input.SourceKind == "Trigger" ||
							 (node.Type == "pc.group_input" && input.Id == "parent_value" &&
							  (entry ? entry->Domain.Kind == SourceSocketKind::Trigger : authoredTrigger));
		if (!trigger) return std::nullopt;
		const Node *owner = EffectiveInputOwner(document, node, input.Id);
		if (!owner) return false;
		if (std::find(owner->SourceStaticInputs.begin(), owner->SourceStaticInputs.end(), input.Id) !=
			owner->SourceStaticInputs.end())
			return false;
		const auto *overlay =
			request.GroupReplay ? request.GroupReplay->SharedSubtype(owner->Id, input.Id) : nullptr;
		const auto *keys = overlay ? &overlay->Keys : entry ? &entry->ParentKeys : nullptr;
		if ((overlay && overlay->Fixed) || (entry && entry->ParentReset)) return false;
		// The source Trigger key map marks key positions, irrespective of stored boolean values.
		// Fractional map coercion is not used by this native integer-frame button path.
		if (request.Subframe != 0 || request.NegativeFrame) {
			const bool keyed =
				(keys && !keys->empty()) ||
				std::any_of(document.Keyframes.begin(), document.Keyframes.end(), [&](const auto &key) {
					return key.NodeId == owner->Id && key.Port == input.Id;
				});
			unsupportedClock = keyed;
			return false;
		}
		const auto match = [&](const Keyframe &key) {
			return key.NodeId == owner->Id && key.Port == input.Id && !key.NegativeFrame &&
				   key.Subframe == 0 && key.Tick == request.Tick;
		};
		if (keys && !keys->empty()) return std::any_of(keys->begin(), keys->end(), match);
		return std::any_of(document.Keyframes.begin(), document.Keyframes.end(), match);
	}

	static const Value *SharedGroupInputView(
		const Document &document,
		const GroupReplayState *replay,
		const Node &target,
		std::string_view port,
		std::optional<Quaternion> *projection = nullptr
	) {
		const std::string_view nodeId = target.Id;
		const auto *binding = replay && replay->InstancesBound() ? replay->Binding(nodeId, port) : nullptr;
		if (detail::InheritedMovedSourceGetter(target, port, binding)) binding = nullptr;
		const std::string_view ownerPort = detail::SourceGetterPort(target, port, binding);
		const Node *owner = nullptr;
		if (binding) {
			const auto found =
				std::find_if(document.Nodes.begin(), document.Nodes.end(), [&](const auto &node) {
					return node.Id == binding->OwnerId;
				});
			if (found != document.Nodes.end()) owner = &*found;
		} else {
			owner = EffectiveInputOwner(document, target, port);
		}
		if (!owner) return nullptr;
		const std::string_view ownerId = owner->Id;
		const bool sourceStatic =
			std::find(owner->SourceStaticInputs.begin(), owner->SourceStaticInputs.end(), ownerPort) !=
			owner->SourceStaticInputs.end();
		const auto *overlay =
			replay && replay->InstancesBound() ? replay->SharedSubtype(ownerId, ownerPort) : nullptr;
		const auto *entry = replay ? replay->Find(ownerId) : nullptr;
		const std::optional<Value> *fixed = overlay ? &overlay->Fixed : nullptr;
		const std::vector<Keyframe> *keys = overlay ? &overlay->Keys : nullptr;
		if (!overlay && entry) {
			if (port == "parent_value") {
				fixed = &entry->ParentReset;
				keys = &entry->ParentKeys;
			} else if (port == "subtype") {
				fixed = &entry->SubtypeStatic;
				keys = &entry->SubtypeKeys;
			}
		}
		const auto getterView = [&](const Value *value, bool rawKey = false) {
			if (projection && rawKey)
				*projection = SourceQuaternionGetterProjection(document, nodeId, port, *value);

			return SourceEmptyGroupVectorView(
				&document,
				*owner,
				port,
				value,
				binding ? std::optional<bool>(binding->Getter == GroupSubtypeAnimator::Animated)
						: std::nullopt
			);
		};
		if (fixed && *fixed) return getterView(&**fixed);
		const Keyframe *first = nullptr;
		size_t count = 0;
		const auto consider = [&](const Keyframe &key) {
			if (key.NodeId != ownerId || key.Port != ownerPort) return;
			if (!first) first = &key;
			++count;
		};
		if (keys && !keys->empty())
			for (const auto &key : *keys)
				consider(key);
		else
			for (const auto &key : document.Keyframes)
				consider(key);
		bool raw = binding ? binding->Getter == GroupSubtypeAnimator::Static : sourceStatic;
		if (binding && binding->Getter == GroupSubtypeAnimator::Animated &&
			binding->Writer == GroupSubtypeAnimator::Static && count > 1)
			raw = true;
		if (!binding && replay && replay->InstancesBound()) {
			for (const auto &target : replay->Bindings())
				if (target.OwnerId == nodeId && detail::BindingAnimatorPort(target) == ownerPort &&
					target.Writer == GroupSubtypeAnimator::Static) {
					raw = true;
					break;
				}
		}
		if (!raw) return nullptr;
		if (first) return getterView(&first->Data, true);
		for (const auto &value : owner->Values)
			if (value.Port == ownerPort) return getterView(&value.Data);
		return nullptr;
	}

	// Source static Range/Vec2 getters expand scalar zero. Range also
	// verifies a represented empty source key to two zero slots.
	static const Value *SourceEmptyGroupVectorView(
		const Document *document,
		const Node &node,
		std::string_view port,
		const Value *value,
		std::optional<bool> getterAnimated
	) {
		if (!document || !value || (node.Type != "pc.group_input" && node.Type != "pc.group_output"))
			return value;
		const auto *entry = FindCatalogueEntry(node.Type);
		const auto *input = entry ? FindCatalogueInput(*entry, port) : nullptr;
		if (!input || input->Type != ValueType::Vector2 ||
			(input->SourceKind != "Range" && input->SourceKind != "Vec2"))
			return value;
		const bool animated = getterAnimated.value_or(
			std::find(node.SourceAnimatedInputs.begin(), node.SourceAnimatedInputs.end(), port) !=
			node.SourceAnimatedInputs.end()
		);
		const bool hasKeys =
			std::any_of(document->Keyframes.begin(), document->Keyframes.end(), [&](const Keyframe &key) {
				return key.NodeId == node.Id && key.Port == port;
			});
		const bool sourceEmptyKey = SourceEmptyGroupVectorKey(*document, node, input, port);
		if (animated && hasKeys && !sourceEmptyKey) return value;
		const auto *scalar = std::get_if<double>(value);
		const bool staticZero = !animated && scalar && *scalar == 0.0;
		const auto *array = std::get_if<ArrayValue>(value);
		const bool emptyRange =
			(animated || sourceEmptyKey) && input->SourceKind == "Range" && array &&
			array->ElementType == ValueType::Scalar && array->Elements.empty() && array->Nested.empty() &&
			array->Items.empty() &&
			std::any_of(document->Tracks.begin(), document->Tracks.end(), [&](const AnimationTrack &track) {
				return track.NodeId == node.Id && track.Port == port;
			});
		if (!staticZero && !emptyRange) return value;
		static constexpr Value zeroVector{Vector2{0, 0}};
		return &zeroVector;
	}

	static bool CaptureAuthoredValues(
		const Node &base,
		const Node &local,
		detail::EvaluationBudget &budget,
		NodeValuesCapture &capture,
		Diagnostic &diagnostic,
		const GroupReplayEntry *replay = nullptr,
		const GroupReplayState *replayState = nullptr,
		const Document *document = nullptr,
		const detail::TimelineOverrides *timelineOverrides = nullptr,
		bool rawSourceQuaternions = false
	) {
		struct Row {
			std::string_view Port;
			const Value *Data;
			std::optional<Quaternion> Projection = std::nullopt;
		};
		detail::EvaluationVector<Row> rows{detail::EvaluationAllocator<Row>(budget)};
		rows.reserve(
			base.Values.size() + local.Values.size() + base.DynamicInputs.size() +
			local.DynamicInputs.size() + 2
		);
		const auto localPort = [&](std::string_view port) {
			if (replayState && replayState->Binding(local.Id, port)) return false;
			if (port == "parent_value" && local.SourceParentInputBase.empty()) return true;
			const auto *owner = document ? EffectiveInputOwner(*document, local, port) : nullptr;
			return owner && owner->Id == local.Id;
		};
		for (const auto &value : base.Values)
			if (&base == &local || !localPort(value.Port))
				rows.push_back(
					{value.Port, SourceEmptyGroupVectorView(document, base, value.Port, &value.Data)}
				);
		if (&base != &local)
			for (const auto &value : local.Values)
				if (localPort(value.Port))
					rows.push_back(
						{value.Port, SourceEmptyGroupVectorView(document, local, value.Port, &value.Data)}
					);
		const auto defaultFor = [&](const DynamicInput &input) {
			if (!input.Default ||
				std::any_of(rows.begin(), rows.end(), [&](const Row &row) { return row.Port == input.Id; }))
				return;
			rows.push_back({input.Id, &*input.Default});
		};
		for (const auto &input : base.DynamicInputs)
			if (&base == &local || !localPort(input.Id)) defaultFor(input);
		if (&base != &local)
			for (const auto &input : local.DynamicInputs)
				if (localPort(input.Id)) defaultFor(input);
		const auto overrideLocal = [&](std::string_view port, const std::optional<Value> &value) {
			if (!value) return;
			const auto found =
				std::find_if(rows.begin(), rows.end(), [&](const auto &row) { return row.Port == port; });
			if (found == rows.end())
				rows.push_back({port, &*value});
			else
				found->Data = &*value;
		};
		if (replay) {
			overrideLocal("parent_value", replay->ParentReset);
			overrideLocal("subtype", replay->SubtypeStatic);
		}
		if (document) {
			const auto apply = [&](std::string_view port) {
				std::optional<Quaternion> projection;
				const auto *value = SharedGroupInputView(*document, replayState, local, port, &projection);
				const auto *binding = replayState && replayState->InstancesBound()
										  ? replayState->Binding(local.Id, port)
										  : nullptr;
				if (detail::InheritedMovedSourceGetter(local, port, binding)) binding = nullptr;
				const auto ownerPort = detail::SourceGetterPort(local, port, binding);
				const auto canonical =
					std::find_if(document->Nodes.begin(), document->Nodes.end(), [&](const auto &node) {
						return node.Id == local.Id;
					});
				const Node *owner = binding || canonical == document->Nodes.end()
										? nullptr
										: EffectiveInputOwner(*document, *canonical, port);
				if (binding) {
					const auto foundOwner =
						std::find_if(document->Nodes.begin(), document->Nodes.end(), [&](const auto &node) {
							return node.Id == binding->OwnerId;
						});
					if (foundOwner != document->Nodes.end()) owner = &*foundOwner;
				}
				if (owner && timelineOverrides) {
					const auto foundOwner =
						std::find_if(document->Nodes.begin(), document->Nodes.end(), [&](const auto &node) {
							return node.Id == owner->Id;
						});
					if (foundOwner == document->Nodes.end())
						owner = nullptr;
					else {
						const size_t ownerIndex = static_cast<size_t>(foundOwner - document->Nodes.begin());
						owner = &timelineOverrides->Find(ownerIndex, *foundOwner);
					}
				}
				const auto *authored = owner ? FindValue(*owner, ownerPort) : nullptr;
				if (!value && authored)
					value = SourceEmptyGroupVectorView(document, *owner, ownerPort, &authored->Data);
				if (!value && owner) {
					const auto dynamic = std::find_if(
						owner->DynamicInputs.begin(), owner->DynamicInputs.end(), [&](const auto &input) {
							return input.Id == ownerPort;
						}
					);
					if (dynamic != owner->DynamicInputs.end() && dynamic->Default) value = &*dynamic->Default;
				}
				const std::string_view ownerId = owner ? std::string_view(owner->Id) : local.Id;
				if (value && rawSourceQuaternions && !projection &&
					SourceQuaternionKeysPresent(*document, replayState, ownerId, ownerPort))
					projection = SourceQuaternionGetterProjection(*document, local.Id, port, *value);
				const auto found =
					std::find_if(rows.begin(), rows.end(), [&](const auto &row) { return row.Port == port; });
				if (!value) {
					if (rawSourceQuaternions && found != rows.end() &&
						SourceQuaternionKeysPresent(*document, replayState, ownerId, ownerPort))
						found->Projection =
							SourceQuaternionGetterProjection(*document, local.Id, port, *found->Data);
					return;
				}
				if (found == rows.end())
					rows.push_back({port, value, projection});
				else {
					found->Data = value;
					found->Projection = projection;
				}
			};
			if (const auto *entry = FindCatalogueEntry(local.Type))
				for (const auto &input : entry->Inputs)
					if (input.SourceIndex >= 0 && input.Id != "parent_value") apply(input.Id);
			for (const auto &input : local.DynamicInputs)
				apply(input.Id);
			if (local.Type == "pc.group_input") apply("parent_value");
		}

		uint64_t bytes = 0;
		if (!AddArrayBytes(bytes, rows.size(), sizeof(AuthoredValue))) goto refused;
		for (const Row &row : rows)
			if (!AddBytes(bytes, std::max(row.Port.size(), std::string{}.capacity())) ||
				!AddBytes(bytes, detail::RetainedPayloadBytes(*row.Data)))
				goto refused;
		{
			auto charge = budget.Reserve(bytes);
			if (!charge || !capture.Charge->Merge(std::move(*charge))) goto refused;
		}
		capture.Values->reserve(rows.size());
		for (const Row &row : rows)
			capture.Values->push_back(
				{std::string(row.Port), row.Projection ? Value(*row.Projection) : *row.Data}
			);
		for (auto &value : *capture.Values)
			detail::StripSourcePathShiftIdentities(value.Data);
		return true;
	refused:
		SetDiagnostic(
			diagnostic,
			Status::LimitExceeded,
			"resolved instance values exceed the live byte budget",
			local.Id
		);
		return false;
	}
	struct SchemaInputValue {
		std::string_view Port;
		const Value *Data = nullptr;
		bool Linked = false;
		std::optional<SourceSocketDomain> Source;
	};
	static bool CaptureSchemaNodeInputs(
		const Node &node,
		std::span<const SchemaInputValue> values,
		std::span<const std::pair<std::string_view, const Image *>> images,
		detail::EvaluationBudget &budget,
		NodeInputCapture &capture,
		Diagnostic &diagnostic
	) {
		uint64_t bytes = 0;
		for (const SchemaInputValue &value : values)
			if (!AddBytes(bytes, sizeof(EvaluationInputValue)) ||
				!AddBytes(bytes, std::max(value.Port.size(), std::string{}.capacity())) ||
				!AddBytes(bytes, detail::RetainedPayloadBytes(*value.Data))) {
				SetDiagnostic(
					diagnostic, Status::LimitExceeded, "node input snapshot size overflow", node.Id
				);
				return false;
			}
		if (!AddArrayBytes(bytes, images.size(), sizeof(EvaluationInputImage))) {
			SetDiagnostic(diagnostic, Status::LimitExceeded, "node input snapshot size overflow", node.Id);
			return false;
		}
		for (const auto &[port, image] : images)
			if (image && (!AddBytes(bytes, std::max(port.size(), std::string{}.capacity())) ||
						  !AddBytes(bytes, image->Pixels.capacity()))) {
				SetDiagnostic(
					diagnostic, Status::LimitExceeded, "node input snapshot size overflow", node.Id
				);
				return false;
			}
		auto charge = budget.Reserve(bytes);
		if (!charge || !capture.Charge->Merge(std::move(*charge))) {
			SetDiagnostic(
				diagnostic,
				Status::LimitExceeded,
				"node input snapshot exceeds the evaluation budget",
				node.Id
			);
			return false;
		}
		capture.Values->reserve(values.size());
		capture.Images->reserve(images.size());
		for (const SchemaInputValue &value : values)
			capture.Values->push_back({std::string(value.Port), *value.Data, value.Linked, value.Source});
		for (auto &value : *capture.Values)
			detail::StripSourcePathShiftIdentities(value.Data);
		for (const auto &[port, image] : images)
			if (image) capture.Images->push_back({std::string(port), *image, std::nullopt});
		return true;
	}

	static Status ValidateEvaluationRequest(const EvaluationRequest &request, Diagnostic &diagnostic) {
		if (request.SourceCacheProject &&
			(!ValidFrameTime(request.SourceCacheProject->ProjectFrame) ||
			 !std::isfinite(request.SourceCacheProject->ProjectLastFrame) ||
			 std::abs(request.SourceCacheProject->ProjectLastFrame) > double(Limits::MaximumTick))) {
			SetDiagnostic(
				diagnostic,
				Status::InvalidValue,
				"source cache project clock observation is noncanonical or unbounded",
				{},
				"source_cache_project"
			);
			return diagnostic.Code;
		}
		if (request.SourceFrameCacheLoads &&
			request.SourceFrameCacheLoads->Entries.size() > Limits::MaximumArrayElements) {
			SetDiagnostic(
				diagnostic, Status::LimitExceeded, "loaded frame cache receipt count exceeds bounds"
			);
			return diagnostic.Code;
		}
		if (request.SourceFontHostResidentBytes > Limits::MaximumEvaluationBytes) {
			SetDiagnostic(
				diagnostic,
				Status::LimitExceeded,
				"font host residency exceeds evaluation bounds",
				{},
				"font_inputs"
			);
			return diagnostic.Code;
		}
		const auto fontStatus = ValidateSourceFontObservations(
			request.SourceFonts, request.FontObservations, Limits::MaximumEvaluationBytes, diagnostic
		);
		if (fontStatus != Status::Ok) return fontStatus;
		uint64_t builtinBytes = 0;
		const auto builtinStatus = ValidateBuiltinRandomCaptures(
			request.BuiltinRandomCaptures, Limits::MaximumEvaluationBytes, builtinBytes, diagnostic
		);
		if (builtinStatus != Status::Ok) return builtinStatus;
		if (request.SimulationCacheCaptures.size() > Limits::MaximumNodes) {
			SetDiagnostic(diagnostic, Status::LimitExceeded, "simulation cache actions exceed node bounds");
			return diagnostic.Code;
		}
		for (size_t index = 0; index < request.SimulationCacheCaptures.size(); ++index) {
			const auto id = request.SimulationCacheCaptures[index];
			if (id.empty() || id.size() > Limits::MaximumTextBytes) {
				SetDiagnostic(
					diagnostic, Status::InvalidValue, "simulation cache action identity is invalid"
				);
				return diagnostic.Code;
			}
			for (size_t earlier = 0; earlier < index; ++earlier)
				if (request.SimulationCacheCaptures[earlier] == id) {
					SetDiagnostic(
						diagnostic, Status::DuplicateId, "simulation cache actions repeat node identity"
					);
					return diagnostic.Code;
				}
		}
		if (request.MaximumImageDimension == 0 || request.MaximumImageDimension > Limits::MaximumDimension) {
			SetDiagnostic(
				diagnostic,
				Status::InvalidValue,
				"evaluation image dimension cap is outside native limits",
				{},
				"maximum_image_dimension"
			);
			return diagnostic.Code;
		}
		if (request.PixelBuilderCirclePrecision < 4 || request.PixelBuilderCirclePrecision > 64 ||
			request.PixelBuilderCirclePrecision % 4 != 0) {
			SetDiagnostic(
				diagnostic,
				Status::InvalidValue,
				"pixel builder circle precision requires a multiple of four from 4 through 64",
				{},
				"pixel_builder_circle_precision"
			);
			return diagnostic.Code;
		}
		if (request.Tick > Limits::MaximumTick) {
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
			(request.Tick == Limits::MaximumTick && request.Subframe != 0)) {
			SetDiagnostic(
				diagnostic, Status::InvalidValue, "subframe must stay within the bounded tick", {}, "subframe"
			);
			return diagnostic.Code;
		}
		if (!ValidFrameTime(GetFrameTime(request))) {
			SetDiagnostic(
				diagnostic,
				Status::InvalidValue,
				"evaluation time must be canonical and bounded",
				{},
				"negative_frame"
			);
			return diagnostic.Code;
		}
		diagnostic = {};
		return Status::Ok;
	}

	static bool CaptureVector2Row(detail::NodeContext &context, void *state) {
		auto &capture = *static_cast<Vector2PresentationCapture *>(state);
		auto &controls = capture.Controls;
		uint64_t bytes = sizeof(Vector2Presentation);
		for (const auto &value : context.OutputValues)
			bytes += detail::ValuePayloadBytes(value.Data);
		if (bytes > context.ByteBudget)
			return context.Fail(Status::LimitExceeded, "Vector2 presentation exceeds byte budget");
		const auto flag = [&](std::string_view port, bool &output) {
			const Value *value = context.Find(port);
			if (!value) return context.Fail(Status::InvalidValue, "Vector2 control is missing", port);
			return std::visit(
				[&](const auto &leaf) {
					using T = std::decay_t<decltype(leaf)>;
					if constexpr (std::is_same_v<T, bool> || std::is_same_v<T, double> ||
								  std::is_same_v<T, int64_t>) {
						const double number = static_cast<double>(leaf);
						if (!std::isfinite(number))
							return context.Fail(Status::InvalidValue, "Vector2 flag is nonfinite", port);
						output = number > .5;
						return true;
					} else if constexpr (std::is_same_v<T, EnumValue>) {
						output = leaf.Value > .5;
						return true;
					} else
						return context.Fail(
							Status::UnsupportedExecution, "Vector2 flag needs a selected numeric value", port
						);
				},
				*value
			);
		};
		const auto vector = [&](std::string_view port, Vector2 &output) {
			const Value *value = context.Find(port);
			if (!value) return context.Fail(Status::InvalidValue, "Vector2 control is missing", port);
			if (const auto *leaf = std::get_if<Vector2>(value))
				output = *leaf;
			else if (const auto *leaf = std::get_if<double>(value))
				output = {*leaf, *leaf};
			else if (const auto *leaf = std::get_if<int64_t>(value))
				output = {double(*leaf), double(*leaf)};
			else
				return context.Fail(
					Status::UnsupportedExecution, "Vector2 control needs a selected vector", port
				);
			if (!std::isfinite(output.X) || !std::isfinite(output.Y))
				return context.Fail(Status::InvalidValue, "Vector2 control is nonfinite", port);
			return true;
		};
		const auto computed = std::find_if(
			context.OutputValues.begin(), context.OutputValues.end(), [](const AuthoredValue &value) {
				return value.Port == "vector";
			}
		);
		if (computed == context.OutputValues.end() || !std::holds_alternative<Vector2>(computed->Data))
			return context.Fail(
				Status::UnsupportedExecution, "Vector2 presentation needs a computed vector", "vector"
			);
		const auto position = std::get<Vector2>(computed->Data);
		controls.X = position.X;
		controls.Y = position.Y;
		if (!flag("integer", controls.Integer) || !flag("show_on_global", controls.ShowOnGlobal) ||
			!flag("relative_unit", controls.RelativeUnit) || !vector("gizmo_offset", controls.Offset) ||
			!vector("gizmo_size", controls.Size))
			return false;
		controls.DisplayType = context.SourceChoice("display_type");
		controls.Style = context.SourceChoice("gizmo_style");
		controls.Shape = context.SourceChoice("gizmo_shape");
		controls.Scale = context.Scalar("gizmo_scale", 1);
		if (!std::isfinite(controls.Scale))
			return context.Fail(Status::InvalidValue, "Vector2 scale is nonfinite", "gizmo_scale");
		if (context.FailureCode != Status::Ok) return false;
		controls.ProjectWidth = context.Project.SurfaceWidth;
		controls.ProjectHeight = context.Project.SurfaceHeight;
		controls.XLinked = context.IsLinked("x");
		controls.YLinked = context.IsLinked("y");
		++controls.ProcessorCount;
		capture.Sprite = context.Input("gizmo_sprite");
		return true;
	}

	struct GroupRefreshCapture {
		const GroupRefreshEvent *Event;
		detail::GroupReplayAccess::Owner *Owner;
		bool Captured = false;
	};

	struct SimulationCapture {
		SimulationReplayState *Replay;
		detail::AllocationReservation *Charge;
		SurfaceFrameReplayState *Surfaces = nullptr;
		RandomReplayState *Random = nullptr;
		DataReplayState *Data = nullptr;
		RigidReplayState *Rigid = nullptr;
	};
	struct StatefulOutputCapture {
		std::span<const std::string> Ids;
		std::vector<StatefulNamedOutput> *Outputs;
		detail::AllocationReservation *Charge;
	};

	static bool GroupHeldPortCompatible(const Document &, const Node &, const CacheGroupReplayOutput &);
	struct GroupProcessCapture {
		std::span<const uint8_t> Run;
		CacheGroupReplayState *Outputs;
		std::vector<GroupRenderReadiness> *Nodes;
		detail::AllocationReservation *Charge = nullptr;
		detail::SourceCommonInvocationMode Invocation = detail::SourceCommonInvocationMode::SourceDoUpdate;
		std::optional<size_t> DirectTarget{};
		SourceCommonSocketSession *Common = nullptr;
		detail::SourceCommonAdmission *CommonAdmission = nullptr;
		GroupRenderSession *Session = nullptr;
	};

	template <class Entry>
	static bool GrowReplayEntries(
		std::vector<Entry> &entries,
		detail::EvaluationBudget &budget,
		detail::AllocationReservation &charge,
		Diagnostic &diagnostic,
		const std::string &nodeId
	) {
		if (entries.size() < entries.capacity()) return true;
		if (entries.size() >= Limits::MaximumArrayElements) {
			SetDiagnostic(diagnostic, Status::LimitExceeded, "replay entry count exceeds bounds", nodeId);
			return false;
		}
		const size_t previousCapacity = entries.capacity();
		const size_t capacity =
			std::min(Limits::MaximumArrayElements, std::max<size_t>(1, previousCapacity * 2));
		auto admission = budget.Reserve(capacity * sizeof(Entry));
		if (!admission || !charge.Merge(std::move(*admission))) {
			SetDiagnostic(
				diagnostic, Status::LimitExceeded, "replay slot growth exceeds byte bounds", nodeId
			);
			return false;
		}
		entries.reserve(capacity);
		auto released = charge.Split(previousCapacity * sizeof(Entry));
		if (!released) std::terminate();
		return true;
	}

	static Status ValidateSimulationCacheActions(
		const Document &document, const EvaluationRequest &request, Diagnostic &diagnostic
	) {
		for (const auto id : request.SimulationCacheCaptures) {
			const auto node =
				std::find_if(document.Nodes.begin(), document.Nodes.end(), [&](const auto &item) {
					return item.Id == id;
				});
			if (node == document.Nodes.end() || node->Type != "pc.verlet_sim_mesh_cache") {
				SetDiagnostic(
					diagnostic,
					Status::InvalidValue,
					"simulation cache action must name an authored Cache Mesh node",
					std::string(id)
				);
				return diagnostic.Code;
			}
		}
		return Status::Ok;
	}

	static bool HasAuthoredCacheGroups(const Document &document) {
		return std::any_of(document.Nodes.begin(), document.Nodes.end(), [](const Node &node) {
			if ((node.Type != "pc.cache" && node.Type != "pc.cache_array") ||
				node.SourceProperties.size() > Limits::MaximumPropertiesPerNode)
				return false;
			return std::any_of(
				node.SourceProperties.begin(),
				node.SourceProperties.end(),
				[](const AuthoredValue &property) {
					const auto *group = std::get_if<ArrayValue>(&property.Data);
					return property.Port == "cache_group" && group && group->ElementType == ValueType::Text &&
						   !group->Elements.empty();
				}
			);
		});
	}

	static Status EvaluateGraph(
		const Document &document,
		const Plan &authoredPlan,
		const std::string &outputId,
		const EvaluationRequest &request,
		NodeResult &outputValue,
		Diagnostic &diagnostic,
		detail::EvaluationBudget &budget,
		detail::AllocationReservation &outputCharge,
		WavPreviewCapture *wavPreview = nullptr,
		Vector2PresentationCapture *vectorPreview = nullptr,
		NodeInputCapture *nodeInputs = nullptr,
		NodeValuesCapture *nodeValues = nullptr,
		std::string_view targetNodeId = {},
		std::string_view requiredOutputId = {},
		bool planAlreadyValidated = false,
		GroupRefreshCapture *groupRefresh = nullptr,
		SimulationCapture *simulation = nullptr,
		StatefulOutputCapture *batch = nullptr,
		GroupProcessCapture *groupProcess = nullptr
	) {

		const bool ownedRequestResidency = groupProcess && groupProcess->CommonAdmission;
		auto observerCharge = budget.Reserve(
			!ownedRequestResidency && request.SourceInputObserver
				? request.SourceInputObserver->RetainedBytes()
				: 0
		);
		if (!observerCharge) {
			SetDiagnostic(
				diagnostic, Status::LimitExceeded, "source processing receipt exceeds evaluation budget"
			);
			if (!request.SourceInputObserver ||
				request.SourceInputObserver->CaptureRefused({}, diagnostic) != Status::Ok)
				return diagnostic.Code;
			observerCharge = budget.Reserve(request.SourceInputObserver->RetainedBytes());
			if (!observerCharge) {
				SetDiagnostic(
					diagnostic,
					Status::LimitExceeded,
					"source processing receipt could not release its storage"
				);
				return diagnostic.Code;
			}
		}

		const bool hasFrameCaches =
			std::any_of(document.Nodes.begin(), document.Nodes.end(), [](const auto &node) {
				return node.Type == "pc.cache" || node.Type == "pc.cache_array";
			});
		detail::AllocationReservation temporaryDataShadow, temporaryDataCharge;
		DataReplayState temporaryData;
		DataReplayState *currentData = simulation ? simulation->Data : nullptr;
		detail::AllocationReservation *dataCharge = currentData ? simulation->Charge : nullptr;
		if (!currentData &&
			(hasFrameCaches || (request.DataReplay && !request.DataReplay->CacheGroups.Nodes.empty()))) {
			const uint64_t retained =
				request.DataReplay ? RetainedDataReplayBytes(*request.DataReplay) : sizeof(DataReplayState);
			auto shadow = budget.Reserve(request.DataReplay ? retained : 0),
				 copied = budget.Reserve(retained);
			if (!shadow || !copied) {
				SetDiagnostic(
					diagnostic, Status::LimitExceeded, "temporary data journal exceeds the live byte budget"
				);
				return diagnostic.Code;
			}
			temporaryDataShadow = std::move(*shadow);
			temporaryDataCharge = std::move(*copied);
			if (request.DataReplay) {
				auto workspace = budget.Reserve(DataReplayValidationWorkspaceBytes(*request.DataReplay));
				if (!workspace) {
					SetDiagnostic(
						diagnostic,
						Status::LimitExceeded,
						"temporary data validation exceeds the live byte budget"
					);
					return diagnostic.Code;
				}
				if (ValidateDataReplay(*request.DataReplay, Limits::MaximumEvaluationBytes, diagnostic) !=
					Status::Ok)
					return diagnostic.Code;
				temporaryData = *request.DataReplay;
			}
			if (!temporaryData.CacheGroups.Nodes.empty()) {
				const uint64_t groupBytes = RetainedCacheGroupReplayBytes(temporaryData.CacheGroups);
				const uint64_t cloneBytes = detail::CacheGroupReplayCloneBytes(temporaryData.CacheGroups);
				size_t ports = 0;
				for (const auto &node : temporaryData.CacheGroups.Nodes)
					ports = std::max(ports, node.Outputs.size());
				const uint64_t workspaceBytes = ports * sizeof(size_t);
				auto replacement = budget.Reserve(cloneBytes), workspace = budget.Reserve(workspaceBytes);
				if (!replacement || !workspace || groupBytes > Limits::MaximumEvaluationBytes ||
					cloneBytes > Limits::MaximumEvaluationBytes - groupBytes ||
					workspaceBytes > Limits::MaximumEvaluationBytes - groupBytes - cloneBytes) {
					SetDiagnostic(
						diagnostic,
						Status::LimitExceeded,
						"temporary group replacement exceeds the live byte budget"
					);
					return diagnostic.Code;
				}
				if (ReconcileCacheGroupReplay(
						document,
						temporaryData.CacheGroups,
						temporaryData.CacheGroups,
						request.SourceCacheProject,
						groupBytes + cloneBytes + workspaceBytes,
						diagnostic
					) != Status::Ok)
					return diagnostic.Code;
				if (!temporaryDataCharge.Resize(RetainedDataReplayBytes(temporaryData))) std::terminate();
			}
			if (HasAuthoredCacheGroups(document) && temporaryData.CacheGroups.Owners.empty()) {
				CacheGroupReplayState initialized;
				if (InitializeAuthoredCacheGroupReplay(
						document, temporaryData.CacheGroups, initialized, budget.Available(), diagnostic
					) != Status::Ok)
					return diagnostic.Code;
				auto initializedCharge = budget.Reserve(RetainedCacheGroupReplayBytes(initialized));
				if (!initializedCharge || !temporaryDataCharge.Merge(std::move(*initializedCharge))) {
					SetDiagnostic(
						diagnostic,
						Status::LimitExceeded,
						"temporary group initialization exceeds the live byte budget"
					);
					return diagnostic.Code;
				}
				temporaryData.CacheGroups = std::move(initialized);
			}
			for (auto &node : temporaryData.CacheGroups.Nodes)
				for (auto &output : node.Outputs)
					if (output.Data) detail::StripSourcePathShiftIdentities(*output.Data);
			for (auto &entry : temporaryData.Entries)
				for (auto &frame : entry.Values)
					detail::StripSourcePathShiftIdentities(frame.Data);
			if (!temporaryDataCharge.Resize(RetainedDataReplayBytes(temporaryData))) std::terminate();
			currentData = &temporaryData;
			dataCharge = &temporaryDataCharge;
		}
		const CacheGroupReplayState *cacheGroups = currentData ? &currentData->CacheGroups : nullptr;

		const uint64_t loadBytes =
			request.SourceFrameCacheLoads ? RetainedDataReplayBytes(*request.SourceFrameCacheLoads) : 0;
		auto frameCacheLoadShadow = budget.Reserve(ownedRequestResidency ? 0 : loadBytes);
		if (!frameCacheLoadShadow) {
			SetDiagnostic(
				diagnostic,
				Status::LimitExceeded,
				"loaded frame cache residency exceeds live evaluation budget"
			);
			return diagnostic.Code;
		}
		if (request.SourceFrameCacheLoads) {
			auto workspace = budget.Reserve(request.SourceFrameCacheLoads->Entries.size() * sizeof(size_t));
			if (!workspace) {
				SetDiagnostic(
					diagnostic,
					Status::LimitExceeded,
					"loaded frame cache validation workspace exceeds budget"
				);
				return diagnostic.Code;
			}
			if (ValidateDataReplay(
					*request.SourceFrameCacheLoads, Limits::MaximumEvaluationBytes, diagnostic
				) != Status::Ok)
				return diagnostic.Code;
		}
		// Host ownership coexists with the held context and observations admitted below.
		const uint64_t fontProviderBytes = request.FontProvider ? request.FontProvider->RetainedBytes() : 0;
		auto fontHostShadow =
			ownedRequestResidency ? budget.Reserve(0)
			: request.SourceFontHostResidentBytes > budget.Available() ||
					fontProviderBytes > budget.Available() - request.SourceFontHostResidentBytes
				? std::optional<detail::AllocationReservation>{}
				: budget.Reserve(request.SourceFontHostResidentBytes + fontProviderBytes);
		if (!fontHostShadow) {
			SetDiagnostic(
				diagnostic,
				Status::LimitExceeded,
				"font host residency exceeds live evaluation budget",
				{},
				"font_inputs"
			);
			return diagnostic.Code;
		}
		if (request.SourceFonts) {
			uint64_t seedNameBytes = 0;
			for (const auto &seed : request.SourceFonts->InitialTextFonts)
				seedNameBytes += seed.NodeId.size();
			if (!document.Nodes.empty() && seedNameBytes > (16 * 1024 * 1024) / document.Nodes.size()) {
				SetDiagnostic(
					diagnostic,
					Status::LimitExceeded,
					"initial Text font node lookup exceeds work bounds",
					{},
					"font_inputs"
				);
				return diagnostic.Code;
			}
			for (const auto &seed : request.SourceFonts->InitialTextFonts) {
				const auto node =
					std::find_if(document.Nodes.begin(), document.Nodes.end(), [&](const Node &candidate) {
						return candidate.Id == seed.NodeId;
					});
				if (node == document.Nodes.end() || node->Type != "pc.text") {
					SetDiagnostic(
						diagnostic,
						Status::InvalidValue,
						"initial Text fonts require an existing Text node",
						seed.NodeId,
						"font_inputs"
					);
					return diagnostic.Code;
				}
			}
		}
		detail::SourcePathShiftMemo pathShiftMemo;
		// Ordinary evaluations share a temporary journal; stateful evaluations supply
		// their already admitted candidate. The borrowed prior is never mutated.
		detail::AllocationReservation rigidPriorShadow, temporaryRigidCharge;
		RigidReplayState temporaryRigid;
		RigidReplayState *currentRigid = simulation ? simulation->Rigid : nullptr;
		detail::AllocationReservation *rigidCharge = currentRigid ? simulation->Charge : nullptr;
		if (!currentRigid) {
			if (request.RigidReplay) {
				if (ValidateRigidReplay(*request.RigidReplay, budget.Available(), diagnostic) != Status::Ok)
					return diagnostic.Code;
				const uint64_t bytes = RetainedRigidReplayBytes(*request.RigidReplay);
				auto prior = budget.Reserve(bytes), copied = budget.Reserve(bytes);
				if (!prior || !copied) {
					SetDiagnostic(
						diagnostic, Status::LimitExceeded, "rigid journal copy exceeds live bounds"
					);
					return diagnostic.Code;
				}
				rigidPriorShadow = std::move(*prior);
				temporaryRigidCharge = std::move(*copied);
				temporaryRigid.Owners.reserve(request.RigidReplay->Owners.capacity());
				for (const auto &owner : request.RigidReplay->Owners)
					temporaryRigid.Owners.push_back(owner);
			}
			currentRigid = &temporaryRigid;
			rigidCharge = &temporaryRigidCharge;
		}
		uint64_t builtinBytes = 0;
		const auto builtinStatus = ValidateBuiltinRandomCaptures(
			request.BuiltinRandomCaptures, budget.Available(), builtinBytes, diagnostic
		);
		if (builtinStatus != Status::Ok) return builtinStatus;
		// Borrowed recordings remain live while evaluation owns its intermediate results.
		auto builtinShadow = budget.Reserve(ownedRequestResidency ? 0 : builtinBytes);
		if (!builtinShadow) {
			SetDiagnostic(
				diagnostic, Status::LimitExceeded, "builtin random recordings exceed live evaluation budget"
			);
			return diagnostic.Code;
		}
		uint64_t fontBytes = 0;
		const auto fontStatus = ValidateSourceFontObservations(
			request.SourceFonts, request.FontObservations, budget.Available(), diagnostic
		);
		if (fontStatus != Status::Ok) return fontStatus;
		if (request.SourceFonts)
			fontBytes +=
				SourceFontContextRetainedBytes(*request.SourceFonts).value_or(Limits::MaximumEvaluationBytes);
		for (const auto &observation : request.FontObservations)
			fontBytes +=
				SourceFontObservationRetainedBytes(observation).value_or(Limits::MaximumEvaluationBytes);
		auto fontShadow = budget.Reserve(ownedRequestResidency ? 0 : fontBytes);
		if (!fontShadow) {
			SetDiagnostic(
				diagnostic, Status::LimitExceeded, "font observations exceed live evaluation budget"
			);
			return diagnostic.Code;
		}
		const auto cacheActions = ValidateSimulationCacheActions(document, request, diagnostic);
		if (cacheActions != Status::Ok) return cacheActions;
		detail::AllocationReservation sliceStateShadow;
		if (request.SliceStackReplay) {
			if (ValidateSliceStackReplay(
					*request.SliceStackReplay, Limits::MaximumEvaluationBytes, diagnostic
				) != Status::Ok)
				return diagnostic.Code;
			auto charge = budget.Reserve(
				ownedRequestResidency ? 0 : RetainedSliceStackReplayBytes(*request.SliceStackReplay)
			);
			if (!charge) {
				SetDiagnostic(
					diagnostic, Status::LimitExceeded, "slice replay owner exceeds live evaluation budget"
				);
				return diagnostic.Code;
			}
			sliceStateShadow = std::move(*charge);
		}
		detail::AllocationReservation groupStateShadow;
		if (request.GroupReplay) {
			if (!request.GroupReplay->InstancesBound() &&
				std::any_of(document.Nodes.begin(), document.Nodes.end(), [](const auto &node) {
					return !node.InstanceBase.empty() || !node.SourceParentInputBase.empty();
				})) {
				SetDiagnostic(
					diagnostic, Status::InvalidValue, "Group instance animator binding has not been applied"
				);
				return diagnostic.Code;
			}
			if (request.GroupReplay->AuthoringRevision() != request.GroupAuthoringRevision) {
				SetDiagnostic(
					diagnostic,
					Status::InvalidValue,
					"group replay revision does not match the authored owner"
				);
				return diagnostic.Code;
			}
			if (!groupRefresh ||
				detail::GroupReplayAccess::Get(*request.GroupReplay) != groupRefresh->Owner) {
				auto shadow =
					budget.Reserve(ownedRequestResidency ? 0 : request.GroupReplay->RetainedBytes());
				if (!shadow) {
					SetDiagnostic(
						diagnostic,
						Status::LimitExceeded,
						"retained group replay state exceeds the live evaluation budget"
					);
					return diagnostic.Code;
				}
				groupStateShadow = std::move(*shadow);
			}
		}
		const Status captureStatus = ValidateAudioCaptureFrames(request.AudioFrames, diagnostic);
		if (captureStatus != Status::Ok) return captureStatus;
		detail::AllocationReservation currentPlanCharge;
		Plan currentPlan;
		if (!planAlreadyValidated) {
			const Status compileStatus = CompileWithBudget(
				document,
				currentPlan,
				diagnostic,
				budget,
				currentPlanCharge,
				authoredPlan.SourceCommonRuntimeOnly
			);
			if (compileStatus != Status::Ok) return compileStatus;
			if (currentPlan != authoredPlan) {
				SetDiagnostic(
					diagnostic, Status::InvalidOutput, "compile plan does not match the authored document"
				);
				return diagnostic.Code;
			}
		}
		std::vector<SourceTunnelRegistryObservation> sourceTunnelRegistry;
		uint64_t tunnelWork = 0;
		const bool tunnelRegistry =
			std::any_of(document.Nodes.begin(), document.Nodes.end(), [](const Node &node) {
				return node.Type == "pc.tunnel_in" || node.Type == "pc.tunnel_out";
			});
		if (tunnelRegistry) {
			if (planAlreadyValidated && CompileWithBudget(
											document,
											currentPlan,
											diagnostic,
											budget,
											currentPlanCharge,
											authoredPlan.SourceCommonRuntimeOnly
										) != Status::Ok)
				return diagnostic.Code;
			if (ResolveSourceTunnelRegistry(
					document,
					request,
					currentPlan,
					budget,
					currentPlanCharge,
					diagnostic,
					groupProcess != nullptr,
					&sourceTunnelRegistry
				) != Status::Ok)
				return diagnostic.Code;
		}
		const Plan &plan = tunnelRegistry ? currentPlan : authoredPlan;

		const auto findOutput = [&](std::string_view id) {
			return std::find_if(
				document.Outputs.begin(), document.Outputs.end(), [&](const Output &candidate) {
					return candidate.Id == id;
				}
			);
		};
		const bool captureTarget = !targetNodeId.empty();
		bool inputsCaptured = false;
		const auto output = captureTarget ? document.Outputs.end() : findOutput(outputId);
		const auto requiredOutput =
			requiredOutputId.empty() ? document.Outputs.end() : findOutput(requiredOutputId);
		if ((!groupProcess && !captureTarget && output == document.Outputs.end()) ||
			(!requiredOutputId.empty() && requiredOutput == document.Outputs.end())) {
			SetDiagnostic(
				diagnostic,
				Status::InvalidOutput,
				"selected output does not exist",
				{},
				captureTarget ? requiredOutputId : std::string_view(outputId)
			);
			return diagnostic.Code;
		}
		if (!groupProcess && !captureTarget && output != document.Outputs.end()) {
			const auto selector = detail::CommonSelector(output->Port);
			if (selector && detail::CommonOwner(document, output->NodeId)) {
				Value data;
				detail::AllocationReservation dataCharge;
				if (detail::ReadSourceCommonGetter(
						document,
						plan,
						detail::CommonOwner(document, output->NodeId)->SourceOwnerId,
						*selector,
						request,
						budget,
						data,
						dataCharge,
						diagnostic
					) != Status::Ok)
					return diagnostic.Code;
				auto metadata = budget.Reserve(
					sizeof(AuthoredValue) + std::max(output->Port.size(), std::string{}.capacity())
				);
				if (!metadata) {
					SetDiagnostic(
						diagnostic,
						Status::LimitExceeded,
						"common output metadata exceeds live bytes",
						output->NodeId
					);
					return diagnostic.Code;
				}
				ValueOutputs values;
				values.push_back({output->Port, std::move(data)});
				outputValue = std::move(values);
				if (!outputCharge.Merge(std::move(dataCharge)) || !outputCharge.Merge(std::move(*metadata)))
					std::terminate();
				return Status::Ok;
			}
		}
		const uint64_t graphBytes =
			document.Nodes.size() * (sizeof(std::pair<std::string_view, size_t>) + sizeof(uint8_t) +
									 sizeof(detail::AllocationReservation) + sizeof(NodeResult));
		auto graphCharge = budget.Reserve(graphBytes);
		if (!graphCharge) {
			SetDiagnostic(diagnostic, Status::LimitExceeded, "graph workspace exceeds the live byte budget");
			return diagnostic.Code;
		}
		const EvaluationNodeIndices nodeIndices(document);
		detail::EvaluationVector<const CacheGroupReplayNode *> frozen(
			document.Nodes.size(), nullptr, detail::EvaluationAllocator<const CacheGroupReplayNode *>(budget)
		);
		if (cacheGroups)
			for (const auto &node : cacheGroups->Nodes)
				if (!CacheGroupReplayShouldRun(node)) frozen[nodeIndices.at(node.NodeId)] = &node;
		auto groupRenderShadow = budget.Reserve(
			request.GroupRender && !groupProcess ? RetainedGroupRenderSessionBytes(*request.GroupRender) : 0
		);
		if (!groupRenderShadow) {
			SetDiagnostic(diagnostic, Status::LimitExceeded, "held group sockets exceed live bytes");
			return diagnostic.Code;
		}
		const auto *heldGroups = groupProcess		   ? groupProcess->Outputs
								 : request.GroupRender ? &request.GroupRender->Outputs
													   : nullptr;
		if (heldGroups) {
			if (ValidateCacheGroupReplay(*heldGroups, Limits::MaximumEvaluationBytes, diagnostic) !=
				Status::Ok)
				return diagnostic.Code;
			for (const auto &held : heldGroups->Nodes) {
				const auto authored =
					std::find_if(document.Nodes.begin(), document.Nodes.end(), [&](const Node &node) {
						return node.Id == held.NodeId && node.Type == held.NodeType;
					});
				if (authored == document.Nodes.end()) {
					SetDiagnostic(
						diagnostic, Status::InvalidValue, "held group producer is stale", held.NodeId
					);
					return diagnostic.Code;
				}
				for (const auto &port : held.Outputs)
					if (!GroupHeldPortCompatible(document, *authored, port)) {
						SetDiagnostic(
							diagnostic,
							Status::InvalidOutput,
							"held group socket schema is stale",
							held.NodeId,
							port.Port
						);
						return diagnostic.Code;
					}
				const size_t index = nodeIndices.at(held.NodeId);
				if (groupProcess ? !groupProcess->Run[index] : held.NodeId != targetNodeId)
					frozen[index] = &held;
			}
		}
		detail::EvaluationVector<CacheGroupReplayNode *> tracked(
			currentData ? document.Nodes.size() : 0,
			nullptr,
			detail::EvaluationAllocator<CacheGroupReplayNode *>(budget)
		);
		if (currentData)
			for (auto &node : currentData->CacheGroups.Nodes)
				tracked[nodeIndices.at(node.NodeId)] = &node;

		detail::EvaluationVector<detail::SourceFrameCacheInputReads> frameCacheInputReads(
			hasFrameCaches ? document.Nodes.size() : 0,
			detail::SourceFrameCacheInputReads::All,
			detail::EvaluationAllocator<detail::SourceFrameCacheInputReads>(budget)
		);
		const bool cachePlaying = request.SourceCachePlayback && request.SourceCachePlayback->Playing;
		const auto refreshFrameCacheReads = [&] {
			const auto *currentFrameCacheData = currentData ? currentData : request.DataReplay;
			const detail::SourceFrameCacheInputIndex currentFrameCacheIndex(
				hasFrameCaches ? currentFrameCacheData : nullptr, budget
			);
			const detail::SourceFrameCacheInputIndex loadedFrameCacheIndex(
				hasFrameCaches ? request.SourceFrameCacheLoads : nullptr, budget
			);
			for (size_t index = 0; index < frameCacheInputReads.size(); ++index) {
				const bool hit = detail::SourceFrameCacheKnownHit(
					document.Nodes[index],
					request,
					currentFrameCacheIndex,
					loadedFrameCacheIndex,
					document.Timeline ? document.Timeline->Frames : 1
				);
				frameCacheInputReads[index] = detail::SourceFrameCacheReadPolicy(
					document.Nodes[index].Type, cachePlaying, false, true, hit
				);
			}
			for (const auto &link : plan.EffectiveLinks) {
				if (frameCacheInputReads.empty() || link.ToPort != "surface_in") continue;
				const size_t target = nodeIndices.at(link.ToNode), producer = nodeIndices.at(link.FromNode);
				const bool hit = document.Nodes[target].Type == "pc.cache" &&
								 frameCacheInputReads[target] == detail::SourceFrameCacheInputReads::None;
				frameCacheInputReads[target] = detail::SourceFrameCacheReadPolicy(
					document.Nodes[target].Type, cachePlaying, true, !frozen[producer], hit
				);
			}
		};
		refreshFrameCacheReads();
		detail::EvaluationVector<uint8_t> demandedTunnelGetters(
			document.Nodes.size(), 0, detail::EvaluationAllocator<uint8_t>(budget)
		);
		for (const auto &route : plan.PcxNamedDependencies)
			if (route.Name == detail::SourceTunnelRouteName) demandedTunnelGetters[route.Producer] = 1;
		const auto frozenTunnelGetter = [&](size_t index) {
			return frozen[index] && document.Nodes[index].Type == "pc.tunnel_in" &&
				   demandedTunnelGetters[index];
		};
		const auto frameCacheReads = [&](size_t index) {
			return frozenTunnelGetter(index) || frameCacheInputReads.empty()
					   ? detail::SourceFrameCacheInputReads::All
					   : frameCacheInputReads[index];
		};
		const auto targetNode =
			std::find_if(document.Nodes.begin(), document.Nodes.end(), [&](const Node &node) {
				return node.Id == targetNodeId;
			});
		if (captureTarget && targetNode == document.Nodes.end()) {
			SetDiagnostic(
				diagnostic, Status::InvalidValue, "selected node does not exist", std::string(targetNodeId)
			);
			return diagnostic.Code;
		}
		const size_t targetIndex =
			groupProcess ? 0 : nodeIndices.at(captureTarget ? targetNodeId : output->NodeId);
		if (captureTarget && !frameCacheInputReads.empty())
			frameCacheInputReads[targetIndex] = detail::SourceFrameCacheInputReads::All;
		if (captureTarget && frozen[targetIndex]) {
			SetDiagnostic(
				diagnostic,
				Status::UnsupportedExecution,
				"frozen node input inspection requires an explicit source render list",
				std::string(targetNodeId),
				"cache_group"
			);
			return diagnostic.Code;
		}

		detail::EvaluationVector<const Output *> selectedOutputs{
			detail::EvaluationAllocator<const Output *>(budget)
		};
		detail::EvaluationVector<uint8_t> retainedTargets{detail::EvaluationAllocator<uint8_t>(budget)};
		if (batch) {
			if ((captureTarget && !nodeInputs) || (!nodeInputs && batch->Ids.empty()) ||
				batch->Ids.size() > Limits::MaximumOutputs) {
				SetDiagnostic(diagnostic, Status::InvalidOutput, "batch needs bounded selected output IDs");
				return diagnostic.Code;
			}
			selectedOutputs.reserve(batch->Ids.size());
			retainedTargets.resize(document.Nodes.size(), 0);
			for (const auto &id : batch->Ids) {
				const auto selected = findOutput(id);
				if (selected == document.Outputs.end() ||
					std::find(selectedOutputs.begin(), selectedOutputs.end(), &*selected) !=
						selectedOutputs.end()) {
					SetDiagnostic(
						diagnostic, Status::InvalidOutput, "batch output ID is missing or duplicated"
					);
					return diagnostic.Code;
				}
				selectedOutputs.push_back(&*selected);
				retainedTargets[nodeIndices.at(selected->NodeId)] = 1;
			}
		}
		const std::array selectedInputPorts{nodeInputs ? nodeInputs->SourcePort : std::string_view{}};
		const detail::SourceInputSelection inputSelection =
			nodeInputs && !nodeInputs->SourcePort.empty()
				? detail::SourceInputSelection{targetIndex, selectedInputPorts}
				: detail::SourceInputSelection{};
		uint64_t scheduleWork = 0;
		detail::EvaluationVector<size_t> pendingRoots{detail::EvaluationAllocator<size_t>(budget)};
		if (groupProcess) {
			for (size_t index = 0; index < groupProcess->Run.size(); ++index)
				if (groupProcess->Run[index]) pendingRoots.push_back(index);
		} else
			pendingRoots.push_back(
				requiredOutput == document.Outputs.end() ? targetIndex
														 : nodeIndices.at(requiredOutput->NodeId)
			);
		for (const auto *selected : selectedOutputs)
			pendingRoots.push_back(nodeIndices.at(selected->NodeId));
		for (const auto id : request.SimulationCacheCaptures)
			pendingRoots.push_back(nodeIndices.at(id));
		detail::PendingGraph pendingGraph(budget);
		auto &upstream = pendingGraph.Upstream;
		auto &needed = pendingGraph.Needed;
		auto &remainingConsumers = pendingGraph.RemainingConsumers;
		if (pendingGraph.Rebuild(
				document,
				plan,
				frameCacheInputReads,
				frozen,
				pendingRoots,
				{},
				{},
				requiredOutput == document.Outputs.end(),
				scheduleWork,
				diagnostic,
				inputSelection
			) != Status::Ok)
			return diagnostic.Code;
		if (requiredOutput != document.Outputs.end()) {
			if (!needed[targetIndex]) {
				SetDiagnostic(
					diagnostic,
					Status::InvalidValue,
					"selected node is not reachable from the selected output",
					std::string(targetNodeId)
				);
				return diagnostic.Code;
			}
			pendingRoots.clear();
			pendingRoots.push_back(targetIndex);
			if (pendingGraph.Rebuild(
					document,
					plan,
					frameCacheInputReads,
					frozen,
					pendingRoots,
					{},
					{},
					true,
					scheduleWork,
					diagnostic,
					inputSelection
				) != Status::Ok)
				return diagnostic.Code;
		}
		if (!groupProcess && !request.GroupRender && !request.ForceGroupRender) {
			for (size_t index = 0; index < document.Nodes.size(); ++index) {
				if (!needed[index]) continue;
				std::string_view scope = document.Nodes[index].GroupId;
				for (size_t hop = 0; !scope.empty() && hop < document.Groups.size(); ++hop) {
					const auto group = std::find_if(
						document.Groups.begin(), document.Groups.end(), [&](const Group &candidate) {
							return candidate.Id == scope;
						}
					);
					if (group == document.Groups.end()) break;
					if (!group->RenderActive) {
						SetDiagnostic(
							diagnostic,
							Status::UnsupportedExecution,
							"disabled group requires held process sockets or explicit rendering",
							group->Id
						);
						return diagnostic.Code;
					}
					scope = group->ParentId;
				}
			}
		}
		detail::EvaluationVector<size_t> pending{detail::EvaluationAllocator<size_t>(budget)};
		const bool deferredTimeline =
			!nodeValues && cacheGroups &&
			std::any_of(cacheGroups->Owners.begin(), cacheGroups->Owners.end(), [](const auto &owner) {
				return !owner.Members.empty();
			});
		detail::EvaluationVector<uint8_t> timelineNeeded(
			document.Nodes.size(), 0, detail::EvaluationAllocator<uint8_t>(budget)
		);
		detail::EvaluationVector<uint8_t> timelineGetters(
			document.Nodes.size(), 0, detail::EvaluationAllocator<uint8_t>(budget)
		);
		const auto admitTimelineWork = [&](uint64_t work) -> bool {
			if (work > 64'000'000 - scheduleWork) {
				SetDiagnostic(
					diagnostic, Status::LimitExceeded, "pending timeline admission exceeds its work budget"
				);
				return false;
			}
			scheduleWork += work;
			return true;
		};
		const auto selectTimelineInputs = [&](std::span<const uint8_t> selected) -> bool {
			if (deferredTimeline &&
				!admitTimelineWork(document.Nodes.size() * 3 + plan.InlineOwnerDependencies.size()))
				return false;
			std::copy(selected.begin(), selected.end(), timelineNeeded.begin());
			for (size_t index = 0; index < frozen.size(); ++index)
				if ((frozen[index] && !frozenTunnelGetter(index)) ||
					frameCacheReads(index) == detail::SourceFrameCacheInputReads::None)
					timelineNeeded[index] = 0;
			// storage owners added below must not choose a getter mode for their borrowers.
			for (size_t index = 0; index < timelineNeeded.size(); ++index)
				timelineGetters[index] |= timelineNeeded[index];
			for (const auto &route : plan.InlineOwnerDependencies)
				if (route.ControlsOnly && selected[route.Consumer] && !frozen[route.Consumer] &&
					frameCacheReads(route.Consumer) != detail::SourceFrameCacheInputReads::None)
					timelineNeeded[route.Owner] = timelineGetters[route.Owner] = 1;
			for (size_t index = 0; index < needed.size(); ++index) {
				if (!timelineNeeded[index]) continue;
				const Node *parent = &document.Nodes[index];
				for (size_t hop = 0; !parent->SourceParentInputBase.empty() && hop < document.Nodes.size();
					 ++hop) {
					if (deferredTimeline && !admitTimelineWork(1)) return false;
					const size_t owner = nodeIndices.at(parent->SourceParentInputBase);
					timelineNeeded[owner] = 1;
					parent = &document.Nodes[owner];
				}
				const Node *current = &document.Nodes[index];
				for (size_t hop = 0; !current->InstanceBase.empty() && hop < document.Nodes.size(); ++hop) {
					if (deferredTimeline && !admitTimelineWork(1)) return false;
					const size_t base = nodeIndices.at(current->InstanceBase);
					timelineNeeded[base] = 1;
					current = &document.Nodes[base];
				}
			}
			return true;
		};
		detail::TimelineOverrides timelineOverrides;
		if (!deferredTimeline) {
			if (!selectTimelineInputs(needed)) return diagnostic.Code;
			const Status timelineStatus = detail::ResolveTimelineOverrides(
				document,
				timelineNeeded,
				request,
				budget,
				timelineOverrides,
				diagnostic,
				{},
				true,
				timelineGetters,
				frameCacheInputReads,
				nullptr,
				false,
				inputSelection
			);
			if (timelineStatus != Status::Ok) return timelineStatus;
		}
		if (nodeValues) {
			size_t valuesIndex = targetIndex;
			for (size_t hop = 0;
				 !document.Nodes[valuesIndex].InstanceBase.empty() && hop < document.Nodes.size();
				 ++hop)
				valuesIndex = nodeIndices.at(document.Nodes[valuesIndex].InstanceBase);
			if (!CaptureAuthoredValues(
					timelineOverrides.Find(valuesIndex, document.Nodes[valuesIndex]),
					timelineOverrides.Find(targetIndex, document.Nodes[targetIndex]),
					budget,
					*nodeValues,
					diagnostic,
					request.GroupReplay ? request.GroupReplay->Find(document.Nodes[targetIndex].Id) : nullptr,
					request.GroupReplay,
					&document,
					&timelineOverrides,
					true
				))
				return diagnostic.Code;
			return Status::Ok;
		}
		std::vector<detail::AllocationReservation> resultCharges(document.Nodes.size());
		std::vector<NodeResult> results(document.Nodes.size());
		std::optional<detail::HostCaptureReceiptSink> hostReceipts;
		std::optional<detail::SourceFontReceiptSink> fontReceipts;
		if (std::any_of(document.Nodes.begin(), document.Nodes.end(), [](const Node &node) {
				return node.Type == "pc.pixel_builder";
			})) {
			hostReceipts.emplace(budget);
			fontReceipts.emplace(budget);
		}
		const bool dynamicPcx =
			std::any_of(document.Nodes.begin(), document.Nodes.end(), [](const Node &node) {
				return node.Type == "pc.tunnel_in" || node.Type == "pc.tunnel_out" ||
					   node.Type == "pc.equation" || node.Type.starts_with("pc.pcx_") ||
					   std::any_of(
						   node.SourceInputExpressions.begin(),
						   node.SourceInputExpressions.end(),
						   [](const auto &program) { return program.Enabled; }
					   );
			});
		const bool mutableDispatch =
			dynamicPcx || (currentData && cacheGroups && !cacheGroups->Owners.empty());
		bool groupActivityChanged = false;
		detail::EvaluationVector<PcxNamedDependency> dynamicPcxRoutes{
			detail::EvaluationAllocator<PcxNamedDependency>(budget)
		};
		detail::AllocationReservation dynamicPcxCharge;
		std::optional<PcxNamedDependency> pendingPcxRoute;
		detail::EvaluationVector<uint8_t> completed(
			mutableDispatch ? document.Nodes.size() : 0, 0, detail::EvaluationAllocator<uint8_t>(budget)
		);
		const bool hasInlineOwners =
			std::any_of(
				document.Groups.begin(),
				document.Groups.end(),
				[](const Group &group) { return !group.OwnerNodeId.empty(); }
			) ||
			std::any_of(document.Nodes.begin(), document.Nodes.end(), [](const Node &node) {
				return node.Type == "pc.pixel_builder";
			});
		detail::EvaluationVector<InlineOwnerInputs> inlineInputs(
			(hasInlineOwners || dynamicPcx || !plan.PcxNamedDependencies.empty()) ? document.Nodes.size() : 0,
			detail::EvaluationAllocator<InlineOwnerInputs>(budget)
		);
		detail::EvaluationVector<detail::PixelBuilderDrawState> builderDrawing(
			hasInlineOwners ? document.Nodes.size() : 0,
			detail::PixelBuilderDrawState{request.PixelBuilderCirclePrecision},
			detail::EvaluationAllocator<detail::PixelBuilderDrawState>(budget)
		);
		std::vector<uint8_t> produced(document.Nodes.size(), 0);
		const auto stagePcxDependency = [&]() -> bool {
			if (!pendingPcxRoute) return false;
			const auto &route = *pendingPcxRoute;
			detail::EvaluationVector<uint8_t> seen(
				document.Nodes.size(), 0, detail::EvaluationAllocator<uint8_t>(budget)
			);
			pending.clear();
			pending.push_back(route.Producer);
			while (!pending.empty()) {
				const auto current = pending.back();
				pending.pop_back();
				if (++scheduleWork > 64'000'000) {
					SetDiagnostic(
						diagnostic, Status::LimitExceeded, "PCX scheduling exceeds its work budget"
					);
					return false;
				}
				if (current == route.Consumer) {
					SetDiagnostic(
						diagnostic,
						Status::Cycle,
						"computed PCX dependencies form a cycle",
						document.Nodes[route.Consumer].Id
					);
					return false;
				}
				if (seen[current]) continue;
				seen[current] = 1;
				for (const auto source : upstream[current])
					pending.push_back(source);
			}
			if (dynamicPcxRoutes.size() >= Limits::MaximumLinks ||
				std::find(dynamicPcxRoutes.begin(), dynamicPcxRoutes.end(), route) !=
					dynamicPcxRoutes.end()) {
				SetDiagnostic(
					diagnostic,
					Status::Cycle,
					"PCX named dependency cannot make progress",
					document.Nodes[route.Consumer].Id
				);
				return false;
			}
			dynamicPcxRoutes.push_back(route);
			completed[route.Consumer] = 0;
			if (pendingGraph.Rebuild(
					document,
					plan,
					frameCacheInputReads,
					frozen,
					pendingRoots,
					dynamicPcxRoutes,
					completed,
					true,
					scheduleWork,
					diagnostic,
					inputSelection
				) != Status::Ok)
				return false;
			if (!deferredTimeline) {
				if (!selectTimelineInputs(needed)) return false;
				const auto status = detail::ExtendTimelineOverrides(
					document,
					timelineNeeded,
					request,
					budget,
					timelineOverrides,
					diagnostic,
					true,
					timelineGetters,
					frameCacheInputReads,
					inputSelection
				);
				if (status != Status::Ok) return false;
			}
			pendingPcxRoute.reset();
			diagnostic = {};
			return true;
		};
		// Builder dimensions are a control snapshot, independent of the later layer composition.
		// Borrow the same producer results and timeline views that the normal input getter reads.
		const auto prepareBuilderControls = [&](size_t ownerIndex) -> bool {
			auto &captured = inlineInputs[ownerIndex];
			if (captured.Captured) return true;
			const Node &ownerNode = timelineOverrides.Find(ownerIndex, document.Nodes[ownerIndex]);
			const auto *entry = FindCatalogueEntry(ownerNode.Type);
			if (!entry || ownerNode.Type != "pc.pixel_builder") return false;
			auto workspace = budget.Reserve(
				2 * (sizeof(Value) + sizeof(std::pair<std::string_view, Value>) +
					 sizeof(std::pair<std::string_view, const Value *>))
			);
			if (!workspace) {
				SetDiagnostic(
					diagnostic,
					Status::LimitExceeded,
					"builder control workspace exceeds budget",
					ownerNode.Id
				);
				return false;
			}
			detail::NodeContext controls(ownerNode, *entry, request, budget);
			controls.EvaluationDocument = &document;
			if (document.Project) {
				controls.Project.SurfaceWidth = document.Project->SurfaceWidth;
				controls.Project.SurfaceHeight = document.Project->SurfaceHeight;
			}
			controls.Values.reserve(2);
			controls.ValueViews.reserve(2);
			controls.LinkedValues.reserve(2);
			std::array<detail::AllocationReservation, 2> commonControlCharges;
			size_t commonControlIndex = 0;
			for (std::string_view port : {"dimension", "dimension_unit"}) {
				if (const auto *common = detail::CommonInputRoute(document, plan, ownerNode.Id, port)) {
					Value data;
					if (detail::ReadSourceCommonGetter(
							document,
							plan,
							common->OwnerId,
							common->Selector,
							request,
							budget,
							data,
							commonControlCharges[commonControlIndex++],
							diagnostic
						) != Status::Ok)
						return false;
					controls.Values.emplace_back(port, std::move(data));
					controls.LinkedValues.push_back(port);
					continue;
				}
				const auto *input = FindCatalogueInput(*entry, port);
				const auto link = std::find_if(
					plan.EffectiveLinks.begin(), plan.EffectiveLinks.end(), [&](const Link &item) {
						return item.ToNode == ownerNode.Id && item.ToPort == port;
					}
				);
				if (link != plan.EffectiveLinks.end()) {
					const size_t producer = nodeIndices.at(link->FromNode);
					if (!produced[producer]) {
						SetDiagnostic(
							diagnostic,
							Status::InvalidOutput,
							"builder control producer was not evaluated",
							ownerNode.Id,
							port
						);
						return false;
					}
					controls.LinkedValues.push_back(port);
					if (const auto *refusal = FindOutputDiagnostic(results[producer], link->FromPort)) {
						diagnostic = *refusal;
						return false;
					}
					if (const Image *image = FindImageOutput(results[producer], link->FromPort);
						image && port == "dimension") {
						controls.Values.emplace_back(
							port,
							Vector2{static_cast<double>(image->Width), static_cast<double>(image->Height)}
						);
						continue;
					}
					const auto *values = FindValueOutputs(results[producer]);
					const AuthoredValue *value = nullptr;
					if (values)
						for (const auto &item : *values)
							if (item.Port == link->FromPort) value = &item;
					if (!value) {
						SetDiagnostic(
							diagnostic,
							Status::TypeMismatch,
							"builder control requires a typed value",
							ownerNode.Id,
							port
						);
						return false;
					}
					if (const auto *box = std::get_if<PixelBoxValue>(&value->Data);
						box && port == "dimension") {
						const auto bounds = detail::PixelBoxBounds(box->Data ? *box->Data : PixelBoxData{});
						controls.Values.emplace_back(
							port, Vector2{bounds[2] - bounds[0], bounds[3] - bounds[1]}
						);
					} else
						controls.ValueViews.emplace_back(port, &value->Data);
					continue;
				}
				std::optional<Quaternion> projection;
				if (const Value *shared =
						SharedGroupInputView(document, request.GroupReplay, ownerNode, port, &projection)) {
					controls.ValueViews.emplace_back(port, shared);
					continue;
				}
				const auto routed = std::find_if(
					plan.ResolvedInputs.begin(), plan.ResolvedInputs.end(), [&](const ResolvedInput &item) {
						return item.NodeId == ownerNode.Id && item.Port == port;
					}
				);
				if (routed != plan.ResolvedInputs.end()) {
					controls.LinkedValues.push_back(port);
					controls.ValueViews.emplace_back(port, &routed->Data);
					continue;
				}
				const auto *binding = request.GroupReplay && request.GroupReplay->InstancesBound()
										  ? request.GroupReplay->Binding(ownerNode.Id, port)
										  : nullptr;
				if (detail::InheritedMovedSourceGetter(ownerNode, port, binding)) binding = nullptr;
				const auto ownerPort = detail::SourceGetterPort(ownerNode, port, binding);
				const Node *authoredOwner = binding ? &document.Nodes[nodeIndices.at(binding->OwnerId)]
													: EffectiveInputOwner(document, ownerNode, port);
				if (authoredOwner)
					authoredOwner =
						&timelineOverrides.Find(nodeIndices.at(authoredOwner->Id), *authoredOwner);
				if (const auto *value = authoredOwner ? FindValue(*authoredOwner, ownerPort) : nullptr)
					controls.ValueViews.emplace_back(port, &value->Data);
				else if (input && !input->Default.empty()) {
					Value fallback;
					const Status status = detail::ReadValueText(input->Default, fallback, budget, *workspace);
					if (status != Status::Ok) {
						SetDiagnostic(
							diagnostic, status, "builder control default exceeds budget", ownerNode.Id, port
						);
						return false;
					}
					controls.Values.emplace_back(port, std::move(fallback));
				}
			}
			detail::SourceGetterProjection getter(controls);
			if (!getter.Prepare()) {
				SetDiagnostic(
					diagnostic,
					controls.FailureCode,
					controls.FailureMessage,
					ownerNode.Id,
					controls.FailurePort
				);
				return false;
			}
			const Value *dimensionValue = controls.Find("dimension");
			const auto *dimension = dimensionValue ? std::get_if<Vector2>(dimensionValue) : nullptr;
			const Value *unitValue = controls.Find("dimension_unit");
			const auto unit = unitValue ? detail::SourceChoiceNumber(*unitValue) : std::optional<double>{};
			if (!dimension || !unit || *unit < 0 || *unit > 2) {
				SetDiagnostic(
					diagnostic,
					Status::InvalidValue,
					"builder dimension controls are unresolved",
					ownerNode.Id,
					"dimension"
				);
				return false;
			}
			Vector2 pixels = *dimension;
			const bool linked =
				std::find(controls.LinkedValues.begin(), controls.LinkedValues.end(), "dimension") !=
				controls.LinkedValues.end();
			if (!linked && *unit == 1) {
				pixels.X *= controls.Project.SurfaceWidth;
				pixels.Y *= controls.Project.SurfaceHeight;
			}
			if (!linked && *unit == 2) {
				SetDiagnostic(
					diagnostic,
					Status::UnsupportedExecution,
					"builder Mask dimension requires a source mask input",
					ownerNode.Id,
					"dimension_unit"
				);
				return false;
			}
			if (!std::isfinite(pixels.X) || !std::isfinite(pixels.Y) ||
				std::abs(pixels.X) > Limits::MaximumDimension ||
				std::abs(pixels.Y) > Limits::MaximumDimension) {
				SetDiagnostic(
					diagnostic,
					Status::InvalidValue,
					"builder dimension is outside native bounds",
					ownerNode.Id,
					"dimension"
				);
				return false;
			}
			auto charge = budget.Reserve(
				sizeof(EvaluationInputValue) + sizeof(std::pair<std::string_view, const Value *>) + 16
			);
			if (!charge || !captured.Charge.Merge(std::move(*charge))) {
				SetDiagnostic(
					diagnostic,
					Status::LimitExceeded,
					"builder dimension snapshot exceeds budget",
					ownerNode.Id
				);
				return false;
			}
			captured.Values.reserve(1);
			captured.ValueViews.reserve(1);
			captured.Values.push_back({"dimension", pixels, linked, {}});
			captured.ValueViews.emplace_back(captured.Values.front().Port, &captured.Values.front().Data);
			captured.Captured = true;
			return true;
		};
		detail::EvaluationVector<uint8_t> timelineSelected(
			deferredTimeline ? document.Nodes.size() : 0, 0, detail::EvaluationAllocator<uint8_t>(budget)
		),
			timelineAdmitted(timelineSelected.size(), 0, detail::EvaluationAllocator<uint8_t>(budget)),
			timelineBarriers(timelineSelected.size(), 0, detail::EvaluationAllocator<uint8_t>(budget));
		if (deferredTimeline)
			for (const auto &owner : cacheGroups->Owners)
				if (!owner.Members.empty()) timelineBarriers[nodeIndices.at(owner.NodeId)] = 1;
		const auto admitPendingTimeline = [&](size_t index, size_t position) -> bool {
			if (!deferredTimeline || timelineAdmitted[index] ||
				(frozen[index] && !frozenTunnelGetter(index)) ||
				frameCacheReads(index) == detail::SourceFrameCacheInputReads::None)
				return true;
			ENGINE_PROFILE("imagegraph.timeline.dispatch_epoch");
			if (!admitTimelineWork(document.Nodes.size())) return false;
			std::fill(timelineSelected.begin(), timelineSelected.end(), 0);
			// Stop at the next possible activity change. Pending members beyond it keep unread drivers.
			for (; position < plan.NodeOrder.size(); ++position) {
				if (!admitTimelineWork(1)) return false;
				const auto candidate = plan.NodeOrder[position];
				if (!needed[candidate] || produced[candidate]) continue;
				if (!timelineAdmitted[candidate]) timelineSelected[candidate] = 1;
				// Computed routes can reorder readiness across an earlier unresolved barrier.
				if (dynamicPcx || groupActivityChanged) break;
				if (timelineBarriers[candidate]) break;
			}
			if (!selectTimelineInputs(timelineSelected)) return false;
			uint64_t scanWork = document.Nodes.size() + document.Keyframes.size() + document.Tracks.size();
			if (request.GroupReplay) {
				scanWork += request.GroupReplay->DetachedAnimators().size();
				for (const auto &entry : request.GroupReplay->Entries())
					scanWork += 1 + entry.SubtypeKeys.size() + entry.ParentKeys.size();
				for (const auto &overlay : request.GroupReplay->SharedSubtypes())
					scanWork += 1 + overlay.Keys.size();
			}
			if (!admitTimelineWork(scanWork)) return false;
			const auto status = detail::ExtendTimelineOverrides(
				document,
				timelineNeeded,
				request,
				budget,
				timelineOverrides,
				diagnostic,
				true,
				timelineGetters,
				frameCacheInputReads,
				inputSelection
			);
			if (status != Status::Ok) return false;
			for (size_t candidate = 0; candidate < timelineSelected.size(); ++candidate)
				if (timelineSelected[candidate] && (!frozen[candidate] || frozenTunnelGetter(candidate)) &&
					frameCacheReads(candidate) != detail::SourceFrameCacheInputReads::None)
					timelineAdmitted[candidate] = 1;
			return true;
		};
		uint64_t evaluationBytes = 0, captureComparisonWork = 0;
		size_t ordinaryCursor = 0;
		while (true) {
			size_t index = document.Nodes.size(), dispatchPosition = 0;
			bool unfinished = false;
			for (size_t position = mutableDispatch ? 0 : ordinaryCursor; position < plan.NodeOrder.size();
				 ++position) {
				const auto candidate = plan.NodeOrder[position];
				if (!mutableDispatch) ordinaryCursor = position + 1;
				if (++scheduleWork > 64'000'000) {
					SetDiagnostic(
						diagnostic, Status::LimitExceeded, "PCX scheduling exceeds its work budget"
					);
					return diagnostic.Code;
				}
				if (!needed[candidate] || (mutableDispatch && completed[candidate])) continue;
				unfinished = true;
				if (!mutableDispatch || (frozen[candidate] && !frozenTunnelGetter(candidate)) ||
					std::all_of(upstream[candidate].begin(), upstream[candidate].end(), [&](size_t source) {
						return completed[source] != 0;
					})) {
					index = candidate;
					dispatchPosition = position;
					break;
				}
			}
			if (index == document.Nodes.size()) {
				if (!unfinished) break;
				SetDiagnostic(diagnostic, Status::Cycle, "computed PCX dependencies form a cycle");
				return diagnostic.Code;
			}
			// grug refuse before frozen replay, input capture or placeholder values can stand in for source
			// behavior.
			if (detail::IsGroupCallbackOpaque(document.Nodes[index])) {
				SetDiagnostic(
					diagnostic,
					Status::UnsupportedExecution,
					"Group callback requires an opaque source producer",
					document.Nodes[index].Id
				);
				return diagnostic.Code;
			}
			if (mutableDispatch) completed[index] = 1;
			pendingPcxRoute.reset();
			const auto tunnelGetterRoute = [&](const auto &route) {
				return route.Producer == index && route.Name == detail::SourceTunnelRouteName;
			};
			const bool frozenTunnelGetter =
				frozen[index] && document.Nodes[index].Type == "pc.tunnel_in" &&
				(std::any_of(
					 plan.PcxNamedDependencies.begin(), plan.PcxNamedDependencies.end(), tunnelGetterRoute
				 ) ||
				 std::any_of(dynamicPcxRoutes.begin(), dynamicPcxRoutes.end(), tunnelGetterRoute));
			if (frozen[index] && !frozenTunnelGetter) {
				CatalogueOutputs restored;
				if (!RestoreFrozenCacheGroupOutputs(
						document.Nodes[index],
						*frozen[index],
						request.MaximumImageDimension,
						restored,
						budget,
						resultCharges[index],
						diagnostic
					))
					return diagnostic.Code;
				const uint64_t bytes = ResultBytes(restored);
				if (bytes > Limits::MaximumEvaluationBytes - evaluationBytes) {
					SetDiagnostic(
						diagnostic,
						Status::LimitExceeded,
						"frozen outputs exceed intermediate budget",
						document.Nodes[index].Id,
						"cache_group"
					);
					return diagnostic.Code;
				}
				evaluationBytes += bytes;
				results[index] = std::move(restored);
				produced[index] = 1;
				continue;
			}
			if (!admitPendingTimeline(index, dispatchPosition)) return diagnostic.Code;
			const Node &timelineNode = timelineOverrides.Find(index, document.Nodes[index]);
			detail::AllocationReservation commonNodeCharge;
			std::optional<Node> commonNode;
			if (!FindCatalogueEntry(timelineNode.Type)) {
				const auto *schema = FindSchema(timelineNode.Type);
				if (schema)
					for (const auto &port : schema->Ports) {
						if (port.Direction != PortDirection::Input) continue;
						const auto *common =
							detail::CommonInputRoute(document, plan, timelineNode.Id, port.Id);
						if (!common) continue;
						if (!commonNode) {
							const auto bytes = NodeClonePayloadBytes(timelineNode);
							auto storage =
								bytes ? budget.Reserve(
											*bytes + (timelineNode.Values.size() + schema->Ports.size()) *
														 sizeof(AuthoredValue)
										)
									  : std::nullopt;
							if (!storage) {
								SetDiagnostic(
									diagnostic,
									Status::LimitExceeded,
									"common legacy input clone exceeds live bytes",
									timelineNode.Id
								);
								return diagnostic.Code;
							}
							commonNode = timelineNode;
							commonNode->Values.reserve(timelineNode.Values.size() + schema->Ports.size());
							commonNodeCharge = std::move(*storage);
						}
						Value data;
						detail::AllocationReservation dataCharge;
						if (detail::ReadSourceCommonGetter(
								document,
								plan,
								common->OwnerId,
								common->Selector,
								request,
								budget,
								data,
								dataCharge,
								diagnostic
							) != Status::Ok)
							return diagnostic.Code;
						auto nameCharge = budget.Reserve(std::max(port.Id.size(), std::string{}.capacity()));
						if (!nameCharge) {
							SetDiagnostic(
								diagnostic,
								Status::LimitExceeded,
								"common legacy input name exceeds live bytes",
								timelineNode.Id
							);
							return diagnostic.Code;
						}
						const auto authored = std::find_if(
							commonNode->Values.begin(), commonNode->Values.end(), [&](const auto &v) {
								return v.Port == port.Id;
							}
						);
						if (authored != commonNode->Values.end())
							authored->Data = std::move(data);
						else
							commonNode->Values.push_back({std::string(port.Id), std::move(data)});
						if (!commonNodeCharge.Merge(std::move(dataCharge)) ||
							!commonNodeCharge.Merge(std::move(*nameCharge)))
							std::terminate();
					}
			}
			const Node &node = commonNode ? *commonNode : timelineNode;
			size_t valuesIndex = index;
			for (size_t hop = 0;
				 !document.Nodes[valuesIndex].InstanceBase.empty() && hop < document.Nodes.size();
				 ++hop)
				valuesIndex = nodeIndices.at(document.Nodes[valuesIndex].InstanceBase);
			const Node &valueNode = timelineOverrides.Find(valuesIndex, document.Nodes[valuesIndex]);
			const auto inputOwner = [&](std::string_view port) -> const Node & {
				const auto *binding = request.GroupReplay && request.GroupReplay->InstancesBound()
										  ? request.GroupReplay->Binding(node.Id, port)
										  : nullptr;
				if (binding && !detail::InheritedMovedSourceGetter(node, port, binding)) {
					const size_t ownerIndex = nodeIndices.at(binding->OwnerId);
					return timelineOverrides.Find(ownerIndex, document.Nodes[ownerIndex]);
				}
				const auto *owner = EffectiveInputOwner(document, node, port);
				if (!owner) return valueNode;
				const size_t ownerIndex = nodeIndices.at(owner->Id);
				return timelineOverrides.Find(ownerIndex, document.Nodes[ownerIndex]);
			};
			const auto inputAnimatorPort = [&](std::string_view port) {
				const auto *binding = request.GroupReplay && request.GroupReplay->InstancesBound()
										  ? request.GroupReplay->Binding(node.Id, port)
										  : nullptr;
				return detail::SourceGetterPort(node, port, binding);
			};
			// Only admitted connected getters propagate their refusals, including animator aliases.
			for (const auto &link : plan.EffectiveLinks) {
				if (link.ToNode != node.Id && inputOwner(link.ToPort).Id != link.ToNode) continue;
				if ((frozenTunnelGetter && link.ToPort != "value_in") ||
					!detail::SourceFrameCacheReadsPort(frameCacheReads(index), link.ToPort))
					continue;
				const size_t producer = nodeIndices.at(link.FromNode);
				if (!produced[producer]) continue;
				if (const auto *refusal = FindOutputDiagnostic(results[producer], link.FromPort)) {
					diagnostic = *refusal;
					return diagnostic.Code;
				}
			}
			detail::AllocationReservation currentCharge;
			detail::AllocationReservation scratchCharge;
			const auto admit =
				[&](uint64_t bytes, detail::AllocationReservation &destination, std::string_view port = {}) {
					auto charge = budget.Reserve(bytes);
					if (!charge) {
						SetDiagnostic(
							diagnostic,
							Status::LimitExceeded,
							"node storage exceeds the live evaluation byte budget",
							node.Id,
							std::string(port)
						);
						return false;
					}
					if (!destination.Merge(std::move(*charge))) std::terminate();
					return true;
				};
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
			CatalogueOutputs catalogueResult;
			bool producedCatalogue = false;
			if (!FindCatalogueEntry(node.Type)) {
				for (const Link &link : plan.EffectiveLinks) {
					if (link.ToNode != node.Id) continue;
					const ValueOutputs *source = FindValueOutputs(results[nodeIndices.at(link.FromNode)]);
					if (!source) continue;
					for (const AuthoredValue &value : *source) {
						const auto *array = std::get_if<ArrayValue>(&value.Data);
						if (value.Port == link.FromPort && array &&
							(!array->Nested.empty() || !array->Items.empty())) {
							SetDiagnostic(
								diagnostic,
								Status::UnsupportedExecution,
								"legacy node input does not support nested runtime arrays",
								node.Id,
								link.ToPort
							);
							return diagnostic.Code;
						}
					}
				}
			}

			// Legacy arithmetic is still byte based; pure collectors and copies
			// preserve raw formats.
			if (!FindCatalogueEntry(node.Type) && node.Type != "value.array" &&
				node.Type != "value.array_get" && node.Type != "image.passthrough" &&
				node.Type != "image.transform_3d") {
				for (const Link &link : plan.EffectiveLinks) {
					if (link.ToNode != node.Id) continue;
					const NodeResult &source = results[nodeIndices.at(link.FromNode)];
					const Image *image = FindImageOutput(source, link.FromPort);
					const ImageArray *images = FindImageArrayOutput(source, link.FromPort);
					const bool typed =
						(image && image->Format != SurfaceFormat::RGBA8Unorm) ||
						(images &&
						 std::any_of(images->Images.begin(), images->Images.end(), [](const Image &item) {
							 return item.Format != SurfaceFormat::RGBA8Unorm;
						 }));
					if (typed) {
						SetDiagnostic(
							diagnostic,
							Status::UnsupportedExecution,
							"legacy executor requires RGBA8 pending format migration",
							node.Id,
							link.ToPort
						);
						return diagnostic.Code;
					}
				}
			}
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
				if (!resolved && !required &&
					(document.Nodes[sourceIndex].Type == "pc.group_input" ||
					 document.Nodes[sourceIndex].Type == "pc.group_output")) {
					if (const auto *values = FindValueOutputs(results[sourceIndex]))
						for (const auto &value : *values)
							if (value.Port == link->FromPort) {
								const auto *integer = std::get_if<int64_t>(&value.Data);
								const auto *scalar = std::get_if<double>(&value.Data);
								if ((integer && *integer == -4) || (scalar && *scalar == -4)) return true;
							}
				}
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
			if (nodeInputs && node.Id == nodeInputs->NodeId && !FindCatalogueEntry(node.Type) &&
				node.Type != "image.transform_3d") {
				const NodeSchema *schema = FindSchema(node.Type);
				if (!schema) {
					SetDiagnostic(
						diagnostic,
						Status::UnsupportedExecution,
						"node input snapshot needs a declared schema",
						node.Id
					);
					return diagnostic.Code;
				}
				size_t imageInputCount = 0;
				for (const PortSchema &port : schema->Ports)
					if (port.Direction == PortDirection::Input && port.Type == ValueType::Image)
						++imageInputCount;
				if (imageInputCount > std::numeric_limits<uint64_t>::max() /
										  sizeof(std::pair<std::string_view, const Image *>)) {
					SetDiagnostic(
						diagnostic,
						Status::LimitExceeded,
						"node input workspace exceeds the live byte budget",
						node.Id
					);
					return diagnostic.Code;
				}
				auto imageInputsCharge =
					budget.Reserve(imageInputCount * sizeof(std::pair<std::string_view, const Image *>));
				if (!imageInputsCharge) {
					SetDiagnostic(
						diagnostic,
						Status::LimitExceeded,
						"node input workspace exceeds the live byte budget",
						node.Id
					);
					return diagnostic.Code;
				}
				std::vector<std::pair<std::string_view, const Image *>> images;
				images.reserve(imageInputCount);
				for (const PortSchema &port : schema->Ports) {
					if (port.Direction != PortDirection::Input || port.Type != ValueType::Image) continue;
					const auto link = std::find_if(
						plan.EffectiveLinks.begin(), plan.EffectiveLinks.end(), [&](const Link &candidate) {
							return candidate.ToNode == node.Id && candidate.ToPort == port.Id;
						}
					);
					if (link == plan.EffectiveLinks.end()) continue;
					const Image *image = nullptr;
					if (!resolveImage(port.Id, false, image)) return diagnostic.Code;
					images.emplace_back(port.Id, image);
				}
				detail::EvaluationVector<SchemaInputValue> values{
					detail::EvaluationAllocator<SchemaInputValue>(budget)
				};
				values.reserve(node.Values.size() + schema->Ports.size());
				for (const auto &value : node.Values) {
					const auto *common = detail::CommonInputRoute(document, plan, node.Id, value.Port);
					values.push_back(
						{value.Port,
						 &value.Data,
						 common != nullptr,
						 common ? std::optional{detail::CommonSelectorDomain(common->Selector)}
								: std::nullopt}
					);
				}
				for (const PortSchema &port : schema->Ports) {
					if (port.Direction != PortDirection::Input || port.Type == ValueType::Image) continue;
					const auto link = std::find_if(
						plan.EffectiveLinks.begin(), plan.EffectiveLinks.end(), [&](const Link &candidate) {
							return candidate.ToNode == node.Id && candidate.ToPort == port.Id;
						}
					);
					if (link == plan.EffectiveLinks.end()) continue;
					const size_t sourceIndex = nodeIndices.at(link->FromNode);
					const auto *outputs =
						produced[sourceIndex] ? FindValueOutputs(results[sourceIndex]) : nullptr;
					const AuthoredValue *source = nullptr;
					if (outputs)
						for (const auto &output : *outputs)
							if (output.Port == link->FromPort) {
								source = &output;
								break;
							}
					if (!source) {
						SetDiagnostic(
							diagnostic,
							Status::InvalidOutput,
							"linked control did not produce a typed value",
							node.Id,
							std::string(port.Id)
						);
						return diagnostic.Code;
					}
					const auto domain =
						FindOutputDomain(document.Nodes[sourceIndex], results[sourceIndex], link->FromPort);
					const auto authored =
						std::find_if(values.begin(), values.end(), [&](const SchemaInputValue &value) {
							return value.Port == port.Id;
						});
					const SchemaInputValue resolved{port.Id, &source->Data, true, domain};
					if (authored == values.end())
						values.push_back(resolved);
					else
						*authored = resolved;
				}
				if (!CaptureSchemaNodeInputs(node, values, images, budget, *nodeInputs, diagnostic))
					return diagnostic.Code;
				inputsCaptured = true;
				if (!batch) return Status::Ok;
				if (!retainedTargets[index] && remainingConsumers[index] == 0) continue;
			}
			// The native fixed-plane schema shares the borrowed host ports, while the
			// renderer keeps its raster profile distinct from the source node.
			const CatalogueEntry *hostEntry = FindCatalogueEntry(node.Type);
			if (node.Type == "image.transform_3d") hostEntry = FindCatalogueEntry("pc.3_d_transform_image");
			if (const CatalogueEntry *catalogueEntry = hostEntry) {
				const detail::Executor executor = node.Type == "image.transform_3d"
													  ? detail::ReplayRecordedHostOutputs
													  : detail::FindExecutor(node.Type);
				const size_t inputCount = catalogueEntry->Inputs.size() + node.DynamicInputs.size();
				const uint64_t inputBytes =
					inputCount *
					(sizeof(std::pair<std::string_view, const Image *>) +
					 sizeof(std::pair<std::string_view, const ImageArray *>) +
					 sizeof(std::pair<std::string_view, Value>) +
					 sizeof(std::pair<std::string_view, const Value *>) + sizeof(std::string_view) +
					 sizeof(std::pair<std::string_view, SourceSocketDomain>) + sizeof(std::string_view) +
					 sizeof(std::pair<std::string_view, std::string_view>));
				auto inputCharge = budget.Reserve(inputBytes);
				if (!inputCharge) {
					SetDiagnostic(
						diagnostic,
						Status::LimitExceeded,
						"node input workspace exceeds the live byte budget",
						node.Id
					);
					return diagnostic.Code;
				}
				detail::NodeContext context(node, *catalogueEntry, request, budget);
				if (frozenTunnelGetter) context.SelectedSourceInput = "value_in";
				if (groupProcess) context.SourceActivity = &(*groupProcess->Nodes)[index].FrameActivity;
				context.NoiseFieldRequested =
					(output != document.Outputs.end() && output->NodeId == node.Id &&
					 output->Port == "field") ||
					std::any_of(
						selectedOutputs.begin(),
						selectedOutputs.end(),
						[&](const auto *selected) {
							return selected->NodeId == node.Id && selected->Port == "field";
						}
					) ||
					std::any_of(
						plan.EffectiveLinks.begin(), plan.EffectiveLinks.end(), [&](const auto &link) {
							return link.FromNode == node.Id && link.FromPort == "field";
						}
					);
				context.EvaluationDocument = &document;
				context.FontHostResidency =
					ownedRequestResidency ? &groupProcess->CommonAdmission->FontHost : &*fontHostShadow;
				context.PathShiftMemo = &pathShiftMemo;
				if (hostReceipts) {
					context.HostReceipts = &*hostReceipts;
					context.FontReceipts = &*fontReceipts;
					context.ObservedFonts = {fontReceipts->Records.data(), fontReceipts->Records.size()};
					context.ObservedHostCaptures = {
						hostReceipts->Captures.data(), hostReceipts->Captures.size()
					};
				}
				PcxEvaluationNames pcxNames(
					index,
					plan,
					document,
					results,
					{inlineInputs.data(), inlineInputs.size()},
					{dynamicPcxRoutes.data(), dynamicPcxRoutes.size()},
					dynamicPcx ? &pendingPcxRoute : nullptr,
					&budget,
					&dynamicPcxCharge
				);
				pcxNames.Produced = produced;
				pcxNames.CommonRequest = &request;
				context.PcxNames = &pcxNames;
				const bool sourceInputCapture = index == inputSelection.NodeIndex;
				if (sourceInputCapture) {
					context.ObservedSourceInputs = nodeInputs->ObservedSourceInputs;
					context.RequireObservedSourceInputs = true;
					context.SelectedSourceInput = nodeInputs->SourcePort;
					context.ObservedSourceInputOwner = nodeInputs->ObservedSourceInputOwner;
				}
				const auto inlineRoute = std::find_if(
					plan.InlineOwnerDependencies.begin(),
					plan.InlineOwnerDependencies.end(),
					[&](const auto &route) { return route.Consumer == index; }
				);
				if (!sourceInputCapture && inlineRoute != plan.InlineOwnerDependencies.end()) {
					if (inlineRoute->ControlsOnly && !prepareBuilderControls(inlineRoute->Owner))
						return diagnostic.Code;
					const auto &owner = inlineInputs[inlineRoute->Owner];
					if (!owner.Captured) {
						SetDiagnostic(
							diagnostic,
							Status::InvalidOutput,
							"inline owner inputs were not evaluated",
							node.Id
						);
						return diagnostic.Code;
					}
					if (inlineRoute->ControlsOnly &&
						document.Nodes[inlineRoute->Owner].Type == "pc.pixel_builder")
						context.PixelBuilderDrawing = &builderDrawing[inlineRoute->Owner];
					context.InlineOwnerId = document.Nodes[inlineRoute->Owner].Id;
					context.InlineOwnerType = document.Nodes[inlineRoute->Owner].Type;
					context.InlineOwnerLinkedValues = owner.LinkedValues;
					context.InlineOwnerValues = owner.ValueViews;
					context.InlineOwnerImages = owner.ImageViews;
					context.InlineOwnerImageArrays = owner.ImageArrayViews;
				}

				detail::EvaluationVector<detail::PixelBuilderLayer> builderLayers{
					detail::EvaluationAllocator<detail::PixelBuilderLayer>(budget)
				};
				if (!sourceInputCapture && node.Type == "pc.pixel_builder") {
					if (!prepareBuilderControls(index)) return diagnostic.Code;
					context.PixelBuilderCanvas = std::get<Vector2>(inlineInputs[index].Values.front().Data);
					const size_t layerCount = std::count_if(
						plan.InlineOwnerDependencies.begin(),
						plan.InlineOwnerDependencies.end(),
						[&](const auto &route) {
							return route.Owner == index && route.ControlsOnly &&
								   document.Nodes[route.Consumer].Type == "pc.pb_output";
						}
					);
					builderLayers.reserve(layerCount);
					for (const auto &route : plan.InlineOwnerDependencies) {
						if (route.Owner != index || !route.ControlsOnly ||
							document.Nodes[route.Consumer].Type != "pc.pb_output")
							continue;
						const auto *child = produced[route.Consumer]
												? std::get_if<CatalogueOutputs>(&results[route.Consumer])
												: nullptr;
						if (!child || !child->PixelBuilderUpdate) {
							SetDiagnostic(
								diagnostic,
								Status::InvalidOutput,
								"builder layer command was not evaluated",
								node.Id
							);
							return diagnostic.Code;
						}
						builderLayers.push_back(*child->PixelBuilderUpdate);
					}
					context.PixelBuilderLayers = builderLayers;
				}
				context.CurrentSimulation = simulation ? simulation->Replay : request.SimulationReplay;
				context.CurrentRandom =
					simulation && simulation->Random ? simulation->Random : request.RandomReplay;
				context.CurrentData = currentData ? currentData : request.DataReplay;
				context.FrameCacheInputReads = frameCacheReads(index);
				context.FrameCacheGroupActionsAvailable = currentData && mutableDispatch;
				if (node.Type == "pc.cache" || node.Type == "pc.cache_array") {
					const auto surface = std::find_if(
						plan.EffectiveLinks.begin(), plan.EffectiveLinks.end(), [&](const Link &link) {
							return link.ToNode == node.Id && link.ToPort == "surface_in";
						}
					);
					context.FrameCacheSurfaceLinked = surface != plan.EffectiveLinks.end();
					context.FrameCacheProducerActive =
						!context.FrameCacheSurfaceLinked || !frozen[nodeIndices.at(surface->FromNode)];
				}
				context.CurrentRigid = currentRigid;
				context.CurrentRigidCharge = rigidCharge;
				context.CurrentSurfaces =
					simulation && simulation->Surfaces ? simulation->Surfaces : request.SurfaceReplay;
				context.GroupReplay = request.GroupReplay ? request.GroupReplay->Find(node.Id) : nullptr;
				const auto depthRoute = detail::FindGroupInputDepth(document, valueNode.GroupId);
				if (context.FrameCacheInputReads == detail::SourceFrameCacheInputReads::None)
					context.InheritedSurfaceFormat.reset();
				else if (depthRoute.Source == detail::GroupInputDepth::Kind::Concrete)
					context.InheritedSurfaceFormat = SourceSurfaceFormat(depthRoute.Choice);
				else if (depthRoute.Source == detail::GroupInputDepth::Kind::AuthoredValue)
					context.InheritedSurfaceFormat = SurfaceFormat::RGBA8Unorm;
				else if (!sourceInputCapture &&
						 depthRoute.Source == detail::GroupInputDepth::Kind::NodeOutput &&
						 std::any_of(
							 plan.GroupSurfaceDependencies.begin(),
							 plan.GroupSurfaceDependencies.end(),
							 [&](const auto &route) { return route.Consumer == index; }
						 )) {
					const size_t source = nodeIndices.at(depthRoute.NodeId);
					if (!produced[source]) {
						SetDiagnostic(
							diagnostic,
							Status::InvalidOutput,
							"group depth dependency was not evaluated",
							node.Id
						);
						return diagnostic.Code;
					}
					if (const auto *surface = FindImageOutput(results[source], depthRoute.Port))
						context.InheritedSurfaceFormat = surface->Format;
					else if (const auto *array = FindImageArrayOutput(results[source], depthRoute.Port)) {
						const std::vector<ImageArrayItem> *items = &array->Items;
						const ImageArrayItem *first = items->empty() ? nullptr : &items->front();
						for (size_t hop = 0; first && hop < Limits::MaximumArrayElements; ++hop) {
							if (const auto *leaf = std::get_if<size_t>(&first->Data)) {
								if (*leaf >= array->Images.size()) {
									SetDiagnostic(
										diagnostic,
										Status::InvalidValue,
										"group depth image-array leaf is invalid",
										node.Id
									);
									return diagnostic.Code;
								}
								context.InheritedSurfaceFormat = array->Images[*leaf].Format;
								break;
							}
							const auto &children = std::get<std::vector<ImageArrayItem>>(first->Data);
							first = children.empty() ? nullptr : &children.front();
						}
						if (!first) context.InheritedSurfaceFormat = SurfaceFormat::RGBA8Unorm;
					} else
						context.InheritedSurfaceFormat = SurfaceFormat::RGBA8Unorm;
				} else
					context.InheritedSurfaceFormat.reset();
				// Image-capable contexts reserve all slots before borrowed views are
				// formed; scalar-only contexts need no image storage.
				const auto imageInput = [](const auto &input) {
					return input.Type == ValueType::Image || input.Type == ValueType::Any ||
						   input.Type == ValueType::Material3D;
				};
				if (std::any_of(catalogueEntry->Inputs.begin(), catalogueEntry->Inputs.end(), imageInput) ||
					std::any_of(node.DynamicInputs.begin(), node.DynamicInputs.end(), imageInput)) {
					context.Images.reserve(inputCount);
					context.ImageArrays.reserve(inputCount);
				}
				context.Values.reserve(inputCount);
				context.ValueViews.reserve(inputCount);
				context.LinkedValues.reserve(inputCount);
				context.CatalogueDefaultInputs.reserve(inputCount);
				context.InputOwnerIds.reserve(inputCount);
				context.EffectiveInputLinks = plan.EffectiveLinks;
				for (const auto &input : catalogueEntry->Inputs)
					context.InputOwnerIds.emplace_back(input.Id, inputOwner(input.Id).Id);
				for (const auto &input : node.DynamicInputs)
					context.InputOwnerIds.emplace_back(input.Id, inputOwner(input.Id).Id);
				context.InputDomains.reserve(inputCount);
				for (const auto &link : plan.EffectiveLinks)
					if (link.ToNode == node.Id) {
						const size_t source = nodeIndices.at(link.FromNode);
						if (produced[source])
							if (const auto domain =
									FindOutputDomain(document.Nodes[source], results[source], link.FromPort))
								context.InputDomains.emplace_back(link.ToPort, *domain);
					}
				context.Timeline = document.Timeline ? &*document.Timeline : nullptr;
				if (document.Project) {
					// Only evaluation attributes enter node contexts; authored editor
					// guides stay with the document.
					const auto &project = *document.Project;
					context.Project.SurfaceWidth = project.SurfaceWidth;
					context.Project.SurfaceHeight = project.SurfaceHeight;
					context.Project.Interpolation = project.Interpolation;
					context.Project.Oversample = project.Oversample;
					context.Project.ColorDepth = project.ColorDepth;
					context.Project.Shader3D = project.Shader3D;
					context.Project.Palette = project.Palette;
				}
				const auto inheritedInterpolation =
					detail::FindGroupSampling(document, valueNode.GroupId, true);
				const auto inheritedOversample =
					detail::FindGroupSampling(document, valueNode.GroupId, false);
				if (!inheritedInterpolation || !inheritedOversample) {
					SetDiagnostic(
						diagnostic, Status::InvalidGroup, "group sampling inheritance is invalid", node.Id
					);
					return diagnostic.Code;
				}
				context.InheritedInterpolation = *inheritedInterpolation;
				context.InheritedOversample = *inheritedOversample;
				context.ByteBudget = budget.Available();
				detail::AllocationReservation colliderIdsCharge;
				std::vector<std::string_view> colliderIds;
				if (!sourceInputCapture &&
					(node.Type == "pc.verlet_sim_step" || node.Type == "pc.verlet_sim_render")) {
					const size_t count = std::count_if(
						plan.InlineControlDependencies.begin(),
						plan.InlineControlDependencies.end(),
						[&](const auto &route) {
							return route.Consumer == index &&
								   document.Nodes[route.Producer].Type == "pc.verlet_sim_collide";
						}
					);
					if (count && !simulation) {
						SetDiagnostic(
							diagnostic,
							Status::UnsupportedExecution,
							"source collider registrations require EvaluateSimulation or stateful evaluation",
							node.Id,
							"mesh"
						);
						return diagnostic.Code;
					}
					if (count) {
						auto charge = context.ReserveWorkspace(count * sizeof(std::string_view), "mesh");
						if (!charge) {
							SetDiagnostic(
								diagnostic,
								context.FailureCode,
								context.FailureMessage,
								node.Id,
								context.FailurePort
							);
							return diagnostic.Code;
						}
						colliderIdsCharge = std::move(*charge);
						colliderIds.reserve(count);
						for (const auto &route : plan.InlineControlDependencies)
							if (route.Consumer == index &&
								document.Nodes[route.Producer].Type == "pc.verlet_sim_collide")
								colliderIds.push_back(document.Nodes[route.Producer].Id);
						std::sort(colliderIds.begin(), colliderIds.end(), [&](auto left, auto right) {
							return nodeIndices.at(left) < nodeIndices.at(right);
						});
						context.SimulationColliderIds = colliderIds;
					}
				}
				for (const CatalogueInput &input : catalogueEntry->Inputs) {
					if ((frozenTunnelGetter && input.Id != "value_in") ||
						!detail::ReadsSourceInput(inputSelection, index, input.Id))
						continue;
					if (!detail::SourceFrameCacheReadsPort(context.FrameCacheInputReads, input.Id)) continue;
					if (const auto *common = detail::CommonInputRoute(document, plan, node.Id, input.Id)) {
						Value value;
						detail::AllocationReservation commonCharge;
						if (detail::ReadSourceCommonGetter(
								document,
								plan,
								common->OwnerId,
								common->Selector,
								request,
								budget,
								value,
								commonCharge,
								diagnostic
							) != Status::Ok)
							return diagnostic.Code;
						context.Values.emplace_back(input.Id, std::move(value));
						context.LinkedValues.emplace_back(input.Id);
						context.InputDomains.emplace_back(
							input.Id, detail::CommonSelectorDomain(common->Selector)
						);
						if (!scratchCharge.Merge(std::move(commonCharge))) std::terminate();
						continue;
					}
					const auto link = std::find_if(
						plan.EffectiveLinks.begin(), plan.EffectiveLinks.end(), [&](const Link &candidate) {
							return candidate.ToNode == node.Id && candidate.ToPort == input.Id;
						}
					);
					const bool linked = link != plan.EffectiveLinks.end();
					if (linked) context.LinkedValues.emplace_back(input.Id);
					// A junction default is a resolved link to the local parent,
					// so its GET precedes the inactive local animator as a producer would.
					if (!linked && node.Type == "pc.group_input" && input.Id == "parent_value") {
						const auto routed = std::find_if(
							plan.ResolvedInputs.begin(),
							plan.ResolvedInputs.end(),
							[&](const ResolvedInput &value) {
								return value.NodeId == node.Id && value.Port == input.Id;
							}
						);
						if (routed != plan.ResolvedInputs.end()) {
							context.LinkedValues.emplace_back(input.Id);
							context.ValueViews.emplace_back(input.Id, &routed->Data);
							continue;
						}
					}
					if (!linked && groupRefresh && groupRefresh->Event->NodeId == node.Id &&
						(groupRefresh->Event->EditedPort == input.Id ||
						 (groupRefresh->Event->Reason == GroupRefreshReason::ParentEdit &&
						  input.Id == "parent_value")) &&
						groupRefresh->Event->LocalValue) {
						context.ValueViews.emplace_back(input.Id, groupRefresh->Event->LocalValue);
						continue;
					}
					if (!linked) {
						// Declaration refresh clears Trigger keys before its parent getter is used.
						if (groupRefresh && node.Type == "pc.group_input" && input.Id == "parent_value" &&
							groupRefresh->Event->Reason != GroupRefreshReason::Restore &&
							groupRefresh->Event->Reason != GroupRefreshReason::ParentEdit) {
							const auto *type = context.Find("input_type");
							if (type && detail::SourceChoiceNumber(*type) == std::optional<double>{19}) {
								context.Values.emplace_back(input.Id, false);
								continue;
							}
						}
						bool unsupportedTriggerClock = false;
						if (const auto trigger = SourceTriggerInputValue(
								document, node, input, request, unsupportedTriggerClock
							)) {
							if (unsupportedTriggerClock) {
								SetDiagnostic(
									diagnostic,
									Status::UnsupportedExecution,
									"source Trigger map requires a nonnegative integer frame",
									node.Id,
									std::string(input.Id)
								);
								return diagnostic.Code;
							}
							context.Values.emplace_back(input.Id, *trigger);
							continue;
						}
					}
					if (!linked && context.GroupReplay && input.Id == "parent_value" &&
						context.GroupReplay->ParentReset) {
						context.ValueViews.emplace_back(input.Id, &*context.GroupReplay->ParentReset);
						continue;
					}
					if (!linked) {
						std::optional<Quaternion> projection;
						if (const auto *view = SharedGroupInputView(
								document, request.GroupReplay, node, input.Id, &projection
							)) {
							if (projection)
								context.Values.emplace_back(input.Id, *projection);
							else
								context.ValueViews.emplace_back(input.Id, view);
							continue;
						}
						if (request.GroupReplay && request.GroupReplay->Binding(node.Id, input.Id)) {
							const Node &owner = inputOwner(input.Id);
							if (const auto *value = FindValue(owner, inputAnimatorPort(input.Id))) {
								if (const auto converted = SourceQuaternionGetterProjection(
										document, node.Id, input.Id, value->Data
									)) {
									context.Values.emplace_back(input.Id, *converted);
									continue;
								}
								context.ValueViews.emplace_back(
									input.Id,
									SourceEmptyGroupVectorView(&document, owner, input.Id, &value->Data)
								);
								continue;
							}
						}
					}

					if (!linked && context.GroupReplay && input.Id == "subtype" &&
						context.GroupReplay->SubtypeStatic &&
						!(groupRefresh && groupRefresh->Event->NodeId == node.Id &&
						  groupRefresh->Event->EditedPort == "subtype")) {
						context.ValueViews.emplace_back(input.Id, &*context.GroupReplay->SubtypeStatic);
						continue;
					}
					if (input.Type == ValueType::Image) {
						if (!linked) {
							const auto resolved = std::find_if(
								plan.ResolvedInputs.begin(),
								plan.ResolvedInputs.end(),
								[&](const ResolvedInput &value) {
									return value.NodeId == node.Id && value.Port == input.Id;
								}
							);
							if (resolved != plan.ResolvedInputs.end()) {
								const auto *array = std::get_if<ArrayValue>(&resolved->Data);
								// Junction defaults retain the same owned Atlas identity as producer links.
								if ((std::holds_alternative<AtlasValue>(resolved->Data) ||
									 (array && array->ElementType == ValueType::Atlas)) &&
									detail::ValidRuntimeValue(resolved->Data)) {
									context.LinkedValues.emplace_back(input.Id);
									context.ValueViews.emplace_back(input.Id, &resolved->Data);
									continue;
								}
							}
						}
						if (linked) {
							const size_t sourceIndex = nodeIndices.at(link->FromNode);
							if (const auto *values = FindValueOutputs(results[sourceIndex])) {
								for (const auto &value : *values) {
									if (value.Port != link->FromPort) continue;
									if ((node.Type == "pc.sequence_anim" || node.Type == "pc.cache_results" ||
										 node.Type == "pc.cache" || node.Type == "pc.cache_array") &&
										input.Id == "surface_in" && detail::ValidRuntimeValue(value.Data)) {
										context.ValueViews.emplace_back(input.Id, &value.Data);
										break;
									}
									const auto *array = std::get_if<ArrayValue>(&value.Data);
									// Draw Surface can synchronously rasterize a retained Builder recipe.
									const bool dynamicBuilder =
										node.Type == "pc.pb_draw_surface" && input.Id == "surface" &&
										std::holds_alternative<DynamicSurfaceValue>(value.Data);
									if (dynamicBuilder && detail::ValidRuntimeValue(value.Data)) {
										context.ValueViews.emplace_back(input.Id, &value.Data);
										break;
									}
									if ((std::holds_alternative<AtlasValue>(value.Data) ||
										 (array && array->ElementType == ValueType::Atlas)) &&
										detail::ValidRuntimeValue(value.Data)) {
										context.ValueViews.emplace_back(input.Id, &value.Data);
										break;
									}
								}
								if (context.Find(input.Id)) continue;
							}
							if (const ImageArray *array =
									FindImageArrayOutput(results[sourceIndex], link->FromPort)) {
								context.ImageArrays.emplace_back(input.Id, array);
								continue;
							}
						}
						const Image *image = nullptr;
						if (!resolveImage(input.Id, false, image)) return diagnostic.Code;
						if (image) context.Images.emplace_back(input.Id, image);
						continue;
					}
					// An Any input takes whatever its link carries: an image or a typed
					// value.
					if (input.Type == ValueType::Any && linked) {
						const size_t sourceIndex = nodeIndices.at(link->FromNode);
						if (!produced[sourceIndex]) {
							SetDiagnostic(
								diagnostic,
								Status::InvalidOutput,
								"source was not evaluated",
								node.Id,
								std::string(input.Id)
							);
							return diagnostic.Code;
						}
						if (const ImageArray *array =
								FindImageArrayOutput(results[sourceIndex], link->FromPort)) {
							context.ImageArrays.emplace_back(input.Id, array);
							continue;
						}
						if (const Image *image = FindImageOutput(results[sourceIndex], link->FromPort)) {
							context.Images.emplace_back(input.Id, image);
							continue;
						}
						if (const ValueOutputs *source = FindValueOutputs(results[sourceIndex])) {
							const auto found =
								std::find_if(source->begin(), source->end(), [&](const AuthoredValue &entry) {
									return entry.Port == link->FromPort;
								});
							if (found != source->end()) {
								context.ValueViews.emplace_back(input.Id, &found->Data);
								continue;
							}
						}
					}
					// D3Material clones a linked surface into its default material before
					// source row scheduling. Borrow it here; the mesh executor admits and
					// owns the selected row's bytes.
					if (linked && input.Type == ValueType::Material3D && input.SourceIndex >= 0 &&
						input.SourceKind == "D3Material") {
						const size_t sourceIndex = nodeIndices.at(link->FromNode);
						if (produced[sourceIndex]) {
							if (const auto *array =
									FindImageArrayOutput(results[sourceIndex], link->FromPort)) {
								context.ImageArrays.emplace_back(input.Id, array);
								continue;
							}
							if (const auto *image = FindImageOutput(results[sourceIndex], link->FromPort)) {
								context.Images.emplace_back(input.Id, image);
								continue;
							}
						}
					}
					if (input.Type != ValueType::Any && !IsAuthoredValueType(input.Type)) {
						// Compile resolves an unconnected junction's owned default without
						// a producer edge. Admit it before runtime-only inputs skip local values.
						if (!linked) {
							const auto resolved = std::find_if(
								plan.ResolvedInputs.begin(),
								plan.ResolvedInputs.end(),
								[&](const ResolvedInput &value) {
									return value.NodeId == node.Id && value.Port == input.Id;
								}
							);
							if (resolved != plan.ResolvedInputs.end()) {
								const auto type = detail::PayloadType(resolved->Data);
								const auto *array = std::get_if<ArrayValue>(&resolved->Data);
								const bool compatible =
									type == input.Type ||
									(input.Type == ValueType::Object && type == ValueType::Struct) ||
									((input.Type == ValueType::Mesh || input.Type == ValueType::Scene3D) &&
									 (type == ValueType::Mesh || type == ValueType::Light3D ||
									  type == ValueType::Scene3D)) ||
									(array && (array->ElementType == input.Type || !array->Items.empty()));
								if (!detail::ValidRuntimeValue(resolved->Data) || !compatible) {
									SetDiagnostic(
										diagnostic,
										Status::TypeMismatch,
										"runtime junction default requires a bounded owned typed value",
										node.Id,
										std::string(input.Id)
									);
									return diagnostic.Code;
								}
								context.LinkedValues.emplace_back(input.Id);
								context.ValueViews.emplace_back(input.Id, &resolved->Data);
								continue;
							}
						}
						if (linked &&
							(input.Type == ValueType::Mesh || input.Type == ValueType::Mesh2D ||
							 input.Type == ValueType::Material3D || input.Type == ValueType::Light3D ||
							 input.Type == ValueType::Scene3D || input.Type == ValueType::Sdf ||
							 input.Type == ValueType::Buffer || input.Type == ValueType::Struct ||
							 input.Type == ValueType::Object || input.Type == ValueType::PcxNode ||
							 input.Type == ValueType::NodeRef || input.Type == ValueType::FluidDomain ||
							 input.Type == ValueType::Particle || input.Type == ValueType::Tileset ||
							 input.Type == ValueType::Rigid || input.Type == ValueType::Atlas ||
							 input.Type == ValueType::Strand || input.Type == ValueType::PixelBox ||
							 input.Type == ValueType::DynamicSurface || input.Type == ValueType::Path3D)) {
							const size_t sourceIndex = nodeIndices.at(link->FromNode);
							const auto *source =
								produced[sourceIndex] ? FindValueOutputs(results[sourceIndex]) : nullptr;
							const AuthoredValue *found = nullptr;
							if (source)
								for (const auto &value : *source)
									if (value.Port == link->FromPort) found = &value;
							const auto *array = found ? std::get_if<ArrayValue>(&found->Data) : nullptr;
							if (found && detail::ValidRuntimeValue(found->Data) &&
								(detail::PayloadType(found->Data) == input.Type ||
								 (input.Type == ValueType::Object &&
								  std::holds_alternative<StructValue>(found->Data)) ||
								 ((input.Type == ValueType::Mesh || input.Type == ValueType::Scene3D) &&
								  (detail::PayloadType(found->Data) == ValueType::Mesh ||
								   detail::PayloadType(found->Data) == ValueType::Light3D ||
								   detail::PayloadType(found->Data) == ValueType::Scene3D)) ||
								 (array && (array->ElementType == input.Type || !array->Items.empty())))) {
								context.ValueViews.emplace_back(input.Id, &found->Data);
								continue;
							}
							SetDiagnostic(
								diagnostic,
								Status::TypeMismatch,
								"runtime input requires a bounded owned typed source",
								node.Id,
								std::string(input.Id)
							);
							return diagnostic.Code;
						}
						if (input.Type == ValueType::AudioBit && linked) {
							const size_t sourceIndex = nodeIndices.at(link->FromNode);
							const ValueOutputs *source =
								produced[sourceIndex] ? FindValueOutputs(results[sourceIndex]) : nullptr;
							const AuthoredValue *found = nullptr;
							if (source)
								for (const auto &value : *source)
									if (value.Port == link->FromPort) found = &value;
							if (found && std::holds_alternative<AudioBit>(found->Data) &&
								IsFinite(found->Data)) {

								context.ValueViews.emplace_back(input.Id, &found->Data);
								continue;
							}
							SetDiagnostic(
								diagnostic,
								Status::InvalidValue,
								"audio input requires finite bounded typed audio "
								"with a positive sample rate",
								node.Id,
								std::string(input.Id)
							);
							return diagnostic.Code;
						}
						if (linked) {
							SetDiagnostic(
								diagnostic,
								Status::UnsupportedExecution,
								"runtime-only input has no native producer",
								node.Id,
								std::string(input.Id)
							);
							return diagnostic.Code;
						}
						continue;
					}
					const Value *value = nullptr;
					if (linked) {
						const size_t sourceIndex = nodeIndices.at(link->FromNode);
						const bool patternSurfaceGetter =
							(node.Type == "pc.herringbone_tile" || node.Type == "pc.honeycomb_noise") &&
							input.SourceKind == "Vec2" && (input.Id == "position" || input.Id == "scale");
						if (patternSurfaceGetter && produced[sourceIndex]) {
							const auto domain = FindOutputDomain(
								document.Nodes[sourceIndex], results[sourceIndex], link->FromPort
							);
							const bool nativeSurface =
								!domain &&
								FindPortType(
									document.Nodes[sourceIndex], link->FromPort, PortDirection::Output
								) == ValueType::Image;
							if (nativeSurface || (domain && domain->Kind == SourceSocketKind::Surface)) {
								const Image *surface = FindImageOutput(results[sourceIndex], link->FromPort);
								const ImageArray *surfaces =
									surface ? nullptr
											: FindImageArrayOutput(results[sourceIndex], link->FromPort);
								if (surface || surfaces) {
									// The source getter sees the whole array before processor row selection.
									context.Values.emplace_back(
										input.Id,
										Vector2{
											surface ? double(surface->Width) : 1.,
											surface ? double(surface->Height) : 1.
										}
									);
									if (nativeSurface)
										context.InputDomains.emplace_back(
											input.Id,
											SourceSocketDomain{
												ValueType::Image, std::nullopt, SourceSocketKind::Surface
											}
										);
									continue;
								}
							}
						}
						const bool heightmapSurfaceGetter =
							node.Type == "pc.heightmap_project_3_d" &&
							((input.SourceKind == "Vec3" &&
							  (input.Id == "view_angle" || input.Id == "position")) ||
							 (input.SourceKind == "Range" &&
							  (input.Id == "height_range" || input.Id == "depth_range")) ||
							 (input.SourceKind == "Slider" && (input.Id == "fov" || input.Id == "shift")) ||
							 (input.SourceKind == "Float" &&
							  (input.Id == "distance" || input.Id == "scale")));
						const bool cylinderSurfaceGetter =
							node.Type == "pc.surface_project_cylinder_3_d" &&
							((input.SourceKind == "Vec3" &&
							  (input.Id == "view_angle" || input.Id == "position")) ||
							 (input.SourceKind == "Range" && input.Id == "depth_range") ||
							 (input.SourceKind == "RotRange" && input.Id == "angle_range") ||
							 (input.SourceKind == "Slider" && input.Id == "fov") ||
							 (input.SourceKind == "Float" &&
							  (input.Id == "distance" || input.Id == "scale")));
						const bool surfaceProjectGetter =
							node.Type == "pc.surface_project_3_d" &&
							((input.SourceKind == "Vec3" &&
							  (input.Id == "view_angle" || input.Id == "position")) ||
							 (input.SourceKind == "Range" && input.Id == "depth_range") ||
							 (input.SourceKind == "Slider" &&
							  (input.Id == "fov" || input.Id == "threshold")) ||
							 (input.SourceKind == "Float" &&
							  (input.Id == "distance" || input.Id == "scale")));
						if ((cylinderSurfaceGetter || heightmapSurfaceGetter || surfaceProjectGetter) &&
							produced[sourceIndex]) {
							const auto domain = FindOutputDomain(
								document.Nodes[sourceIndex], results[sourceIndex], link->FromPort
							);
							// Source getters test the declared Surface type, not Atlas payload shape.
							const bool nativeProjectorSurface =
								(heightmapSurfaceGetter || surfaceProjectGetter) && !domain &&
								FindPortType(
									document.Nodes[sourceIndex], link->FromPort, PortDirection::Output
								) == ValueType::Image;
							if ((!domain && cylinderSurfaceGetter) || nativeProjectorSurface ||
								(domain && domain->Kind == SourceSocketKind::Surface)) {
								const Image *surface = FindImageOutput(results[sourceIndex], link->FromPort);
								const ImageArray *surfaces =
									surface ? nullptr
											: FindImageArrayOutput(results[sourceIndex], link->FromPort);
								if (surface || surfaces) {
									const double width = surface ? double(surface->Width) : 1.;
									const double height = surface ? double(surface->Height) : 1.;
									if (input.SourceKind == "Float" || input.SourceKind == "Slider") {
										auto projectionCharge = budget.Reserve(2 * sizeof(ElementValue));
										if (!projectionCharge ||
											!inputCharge->Merge(std::move(*projectionCharge))) {
											SetDiagnostic(
												diagnostic,
												Status::LimitExceeded,
												surfaceProjectGetter
													? "surface projection getter exceeds input storage budget"
												: heightmapSurfaceGetter
													? "heightmap surface getter exceeds input storage budget"
													: "cylinder surface getter exceeds input storage budget",
												node.Id,
												std::string(input.Id)
											);
											return diagnostic.Code;
										}
										ArrayValue dimensions;
										dimensions.ElementType = ValueType::Scalar;
										dimensions.Elements.reserve(2);
										if (dimensions.Elements.capacity() > 2) {
											auto excess = budget.Reserve(
												(dimensions.Elements.capacity() - 2) * sizeof(ElementValue)
											);
											if (!excess || !inputCharge->Merge(std::move(*excess))) {
												SetDiagnostic(
													diagnostic,
													Status::LimitExceeded,
													heightmapSurfaceGetter ? "heightmap surface getter "
																			 "capacity exceeds input budget"
																		   : "cylinder surface getter "
																			 "capacity exceeds input budget",
													node.Id,
													std::string(input.Id)
												);
												return diagnostic.Code;
											}
										}
										dimensions.Elements.emplace_back(width);
										dimensions.Elements.emplace_back(height);
										context.Values.emplace_back(input.Id, std::move(dimensions));
									} else if (input.SourceKind == "Vec3")
										context.Values.emplace_back(input.Id, Vector3{width, height, 0.});
									else
										context.Values.emplace_back(input.Id, Vector2{width, height});
									continue;
								}
							}
						}
						// These source Vec3 getters resize whole-surface dimensions to three axes.
						const bool sourceSurfaceVec3 =
							input.SourceKind == "Vec3" &&
							((node.Type == "pc.quarternion_lookat" &&
							  (input.Id == "origin" || input.Id == "target" || input.Id == "up")) ||
							 ((node.Type == "pc.gradient_cube" || node.Type == "pc.perlin_cube" ||
							   node.Type == "pc.cellular_cube" || node.Type == "pc.simplex_cube") &&
							  (input.Id == "rotation" || input.Id == "rotation_2" || input.Id == "scale_2" ||
							   ((node.Type == "pc.perlin_cube" || node.Type == "pc.cellular_cube" ||
								 node.Type == "pc.simplex_cube") &&
								input.Id == "position"))));
						if (sourceSurfaceVec3 && produced[sourceIndex]) {
							const Image *surface = FindImageOutput(results[sourceIndex], link->FromPort);
							const ImageArray *surfaces =
								surface ? nullptr
										: FindImageArrayOutput(results[sourceIndex], link->FromPort);
							if (surface || surfaces) {
								// surface_get_dimension sees an entire array as a nonsurface, before
								// batching.
								context.Values.emplace_back(
									input.Id,
									Vector3{
										surface ? double(surface->Width) : 1.,
										surface ? double(surface->Height) : 1.,
										0.
									}
								);
								continue;
							}
						}
						if (node.Type == "pc.path_shape_3_d" && input.SourceKind == "Vec3" &&
							(input.Id == "position" || input.Id == "half_size") && produced[sourceIndex]) {
							const Image *surface = FindImageOutput(results[sourceIndex], link->FromPort);
							const ImageArray *surfaces =
								surface ? nullptr
										: FindImageArrayOutput(results[sourceIndex], link->FromPort);
							if (surface || surfaces) {
								context.Values.emplace_back(
									input.Id,
									Vector3{
										surface ? double(surface->Width) : 1.,
										surface ? double(surface->Height) : 1.,
										0.
									}
								);
								continue;
							}
						}
						// Source numeric surface getters return dimensions before units.
						const bool sourceSurfaceScalar =
							(node.Type == "pc.kuwahara" &&
							 ((input.SourceKind == "Int" && input.Id == "radius") ||
							  (input.SourceKind == "Float" &&
							   (input.Id == "unused" || input.Id == "hardness" || input.Id == "sharpness")) ||
							  (input.SourceKind == "Slider" &&
							   (input.Id == "uv_mix" || input.Id == "mix" || input.Id == "mask_feather" ||
								input.Id == "alpha" || input.Id == "zero_crossing")))) ||
							(node.Type == "pc.blobify" &&
							 ((input.SourceKind == "Int" && input.Id == "radius") ||
							  (input.SourceKind == "Slider" &&
							   (input.Id == "threshold" || input.Id == "smoothness" || input.Id == "mix" ||
								input.Id == "mask_feather")))) ||
							(node.Type == "pc.xdo_g_threshold" &&
							 ((input.SourceKind == "Float" &&
							   (input.Id == "radius" || input.Id == "k" || input.Id == "gamma")) ||
							  (input.SourceKind == "Slider" &&
							   (input.Id == "mix" || input.Id == "mask_feather" || input.Id == "epsilon" ||
								input.Id == "smoothness")))) ||
							(node.Type == "pc.refract" &&
							 ((input.SourceKind == "Float" &&
							   (input.Id == "height" || input.Id == "distance" || input.Id == "ior" ||
								input.Id == "perspective")) ||
							  (input.SourceKind == "Slider" &&
							   (input.Id == "uv_mix" || input.Id == "mix" || input.Id == "mask_feather")))) ||
							(node.Type == "pc.ambient_occlusion" &&
							 ((input.Id == "height" && input.SourceKind == "Float") ||
							  (input.Id == "intensity" && input.SourceKind == "Slider"))) ||
							(node.Type == "pc.julia_set" &&
							 ((input.Id == "max_iteration" && input.SourceKind == "Int") ||
							  (input.Id == "diverge_threshold" && input.SourceKind == "Float") ||
							  (input.Id == "uv_mix" && input.SourceKind == "Slider"))) ||
							(node.Type == "pc.gabor_noise" && input.SourceKind == "Slider" &&
							 (input.Id == "density" || input.Id == "sharpness" || input.Id == "uv_mix")) ||
							(node.Type == "pc.flow_noise" &&
							 ((input.Id == "progress" && input.SourceKind == "Float") ||
							  (input.Id == "uv_mix" && input.SourceKind == "Slider"))) ||
							(node.Type == "pc.noise_bubble" && input.SourceKind == "Slider" &&
							 (input.Id == "density" || input.Id == "thickness" || input.Id == "uv_mix")) ||
							(node.Type == "pc.noise_cristal" &&
							 ((input.Id == "iteration" && input.SourceKind == "Int") ||
							  ((input.Id == "gamma" || input.Id == "uv_mix") &&
							   input.SourceKind == "Slider"))) ||
							(node.Type == "pc.fold_noise" && input.SourceKind == "ISlider") ||
							(node.Type == "pc.cellular" && input.Id == "scale" &&
							 input.SourceKind == "Float") ||
							((node.Type == "pc.perlin_extra" || node.Type == "pc.wavelet_noise" ||
							  node.Type == "pc.noise_scratch" || node.Type == "pc.fold_noise" ||
							  node.Type == "pc.noise_gaussian" || node.Type == "pc.noise_aniso" ||
							  node.Type == "pc.perlin_cube" || node.Type == "pc.cellular_cube" ||
							  node.Type == "pc.simplex_cube") &&
							 (input.SourceKind == "Slider" || input.SourceKind == "Float" ||
							  input.SourceKind == "Rotation" || input.SourceKind == "Int")) ||
							(node.Type == "pc.pytagorean_tile" &&
							 (input.SourceKind == "Slider" || input.SourceKind == "Rotation" ||
							  input.SourceKind == "Int")) ||
							(node.Type == "pc.weave" && input.SourceKind == "Slider" &&
							 (input.Id == "uv_mix" || input.Id == "shift" || input.Id == "shade_span" ||
							  input.Id == "shading")) ||
							(node.Type == "pc.noise_strand" &&
							 ((input.SourceKind == "Slider" &&
							   (input.Id == "density" || input.Id == "slope" || input.Id == "thickness" ||
								input.Id == "uv_mix")) ||
							  (input.SourceKind == "Float" &&
							   (input.Id == "curve_scale" || input.Id == "curve_shift")))) ||
							(node.Type == "pc.shard_noise" &&
							 ((input.Id == "progress" && input.SourceKind == "Float") ||
							  ((input.Id == "sharpness" || input.Id == "uv_mix") &&
							   input.SourceKind == "Slider"))) ||
							(node.Type == "pc.voronoi_extra" &&
							 ((input.Id == "progress" && input.SourceKind == "Float") ||
							  ((input.Id == "parameter_a" || input.Id == "uv_mix") &&
							   input.SourceKind == "Slider"))) ||
							(node.Type == "pc.perlin" &&
							 ((input.Id == "scaling" && input.SourceKind == "Float") ||
							  ((input.Id == "amplitude" || input.Id == "uv_mix") &&
							   input.SourceKind == "Slider"))) ||
							(node.Type == "pc.gradient_cube" &&
							 ((input.Id == "scale" && input.SourceKind == "Float") ||
							  (input.Id == "position" && input.SourceKind == "Slider")));
						if (sourceSurfaceScalar && produced[sourceIndex]) {
							const Image *surface = FindImageOutput(results[sourceIndex], link->FromPort);
							const ImageArray *surfaces =
								surface ? nullptr
										: FindImageArrayOutput(results[sourceIndex], link->FromPort);
							if (surface || surfaces) {
								auto projectionCharge = budget.Reserve(2 * sizeof(ElementValue));
								if (!projectionCharge || !inputCharge->Merge(std::move(*projectionCharge))) {
									SetDiagnostic(
										diagnostic,
										Status::LimitExceeded,
										"source surface getter exceeds input storage budget",
										node.Id,
										std::string(input.Id)
									);
									return diagnostic.Code;
								}
								ArrayValue dimensions;
								dimensions.ElementType = ValueType::Scalar;
								dimensions.Elements.reserve(2);
								if (dimensions.Elements.capacity() > 2) {
									auto excess = budget.Reserve(
										(dimensions.Elements.capacity() - 2) * sizeof(ElementValue)
									);
									if (!excess || !inputCharge->Merge(std::move(*excess))) {
										SetDiagnostic(
											diagnostic,
											Status::LimitExceeded,
											"source surface getter capacity exceeds input budget",
											node.Id,
											std::string(input.Id)
										);
										return diagnostic.Code;
									}
								}
								// surface_get_dimension takes a whole surface array as a nonsurface.
								dimensions.Elements.emplace_back(surface ? double(surface->Width) : 1.);
								dimensions.Elements.emplace_back(surface ? double(surface->Height) : 1.);
								context.Values.emplace_back(input.Id, std::move(dimensions));
								const SourceSocketDomain domain{
									ValueType::Image, std::nullopt, SourceSocketKind::Surface
								};
								const auto foundDomain = std::find_if(
									context.InputDomains.begin(),
									context.InputDomains.end(),
									[&](const auto &entry) { return entry.first == input.Id; }
								);
								if (foundDomain == context.InputDomains.end())
									context.InputDomains.emplace_back(input.Id, domain);
								else
									foundDomain->second = domain;
								continue;
							}
						}
						const bool simpleShape = node.Type == "pc.shape_ellipse" ||
												 node.Type == "pc.shape_rectangle" ||
												 node.Type == "pc.shape_half";
						// Source Vec2 surface getters bypass numeric unit conversion.
						const bool shapeSurfaceVector =
							input.SourceKind == "Vec2" &&
							(node.Type == "pc.mirror_polar" || node.Type == "pc.path_repeat" ||
							 (node.Type == "pc.text" &&
							  (input.Id == "fixed_dimension" || input.Id == "offset" ||
							   input.Id == "character_range")) ||
							 (simpleShape && (input.Id == "center" || input.Id == "half_size")));
						// Dimension projects surfaces before processor selection. Equal sizes collapse.
						if ((simpleShape || node.Type == "pc.flow_noise" || node.Type == "pc.noise_bubble" ||
							 node.Type == "pc.noise_cristal" || node.Type == "pc.gradient_cube" ||
							 node.Type == "pc.cellular" || node.Type == "pc.perlin" ||
							 node.Type == "pc.voronoi_extra" || node.Type == "pc.shard_noise" ||
							 node.Type == "pc.noise_strand" || node.Type == "pc.weave" ||
							 node.Type == "pc.pytagorean_tile" || node.Type == "pc.perlin_extra" ||
							 node.Type == "pc.wavelet_noise" || node.Type == "pc.noise_scratch" ||
							 node.Type == "pc.fold_noise" || node.Type == "pc.noise_gaussian" ||
							 node.Type == "pc.noise_aniso" || node.Type == "pc.perlin_cube" ||
							 node.Type == "pc.cellular_cube" || node.Type == "pc.simplex_cube") &&
							input.Id == "dimension" && input.SourceKind == "Dimension" &&
							produced[sourceIndex]) {
							if (const ImageArray *images =
									FindImageArrayOutput(results[sourceIndex], link->FromPort)) {
								if (images->Items.size() > Limits::MaximumArrayElements) {
									SetDiagnostic(
										diagnostic,
										Status::LimitExceeded,
										"source dimension surface rows exceed element budget",
										node.Id,
										std::string(input.Id)
									);
									return diagnostic.Code;
								}
								Vector2 first{};
								size_t count = 0;
								size_t sourceRow = 0;
								bool equal = true;
								for (const auto &item : images->Items) {
									const size_t row = sourceRow++;
									const auto *index = std::get_if<size_t>(&item.Data);
									if (!index)
										continue; // Source is_surface skips nested non-surface entries.
									if (*index >= images->Images.size()) {
										SetDiagnostic(
											diagnostic,
											Status::InvalidValue,
											"source dimension surface index is invalid",
											node.Id,
											std::string(input.Id)
										);
										return diagnostic.Code;
									}
									const auto &image = images->Images[*index];
									const Vector2 size{double(image.Width), double(image.Height)};
									if (row && (!count || size != first)) equal = false;
									if (!count) first = size;
									++count;
								}
								if (!count || count > Limits::MaximumArrayElements) {
									SetDiagnostic(
										diagnostic,
										Status::UnsupportedExecution,
										"source dimension needs bounded observed surface rows",
										node.Id,
										std::string(input.Id)
									);
									return diagnostic.Code;
								}
								if (equal)
									context.Values.emplace_back(input.Id, first);
								else {
									auto projectionCharge = budget.Reserve(count * sizeof(ElementValue));
									if (!projectionCharge ||
										!inputCharge->Merge(std::move(*projectionCharge))) {
										SetDiagnostic(
											diagnostic,
											Status::LimitExceeded,
											"source dimension projection exceeds live byte budget",
											node.Id,
											std::string(input.Id)
										);
										return diagnostic.Code;
									}
									ArrayValue dimensions;
									dimensions.ElementType = ValueType::Vector2;
									dimensions.Elements.reserve(count);
									const size_t capacity = dimensions.Elements.capacity();
									if (capacity > count) {
										auto excess =
											budget.Reserve((capacity - count) * sizeof(ElementValue));
										if (!excess || !inputCharge->Merge(std::move(*excess))) {
											SetDiagnostic(
												diagnostic,
												Status::LimitExceeded,
												"source dimension capacity exceeds live byte budget",
												node.Id,
												std::string(input.Id)
											);
											return diagnostic.Code;
										}
									}
									for (const auto &item : images->Items) {
										const auto *index = std::get_if<size_t>(&item.Data);
										if (!index) continue;
										const auto &image = images->Images[*index];
										dimensions.Elements.emplace_back(
											Vector2{double(image.Width), double(image.Height)}
										);
									}
									context.Values.emplace_back(input.Id, std::move(dimensions));
								}
								continue;
							}
						}
						// Rearrange's Int-array getter reads source surfaces as dimensions.
						const bool rearrangeOrders = node.Type == "pc.array_rearrange" &&
													 input.Id == "orders" && input.Type == ValueType::Array &&
													 input.SourceKind == "Int";
						if ((input.Type == ValueType::Any || rearrangeOrders || shapeSurfaceVector) &&
							produced[sourceIndex]) {
							if (const ImageArray *array =
									FindImageArrayOutput(results[sourceIndex], link->FromPort)) {
								context.ImageArrays.emplace_back(input.Id, array);
								continue;
							}
							if (const Image *image = FindImageOutput(results[sourceIndex], link->FromPort)) {
								context.Images.emplace_back(input.Id, image);
								continue;
							}
						}
						const ValueOutputs *source =
							produced[sourceIndex] ? FindValueOutputs(results[sourceIndex]) : nullptr;
						const auto found =
							source
								? std::find_if(
									  source->begin(),
									  source->end(),
									  [&](const AuthoredValue &entry) { return entry.Port == link->FromPort; }
								  )
								: ValueOutputs::const_iterator{};
						// These source getters return surface dimensions before Vec2 unit conversion.
						const Image *surface = produced[sourceIndex]
												   ? FindImageOutput(results[sourceIndex], link->FromPort)
												   : nullptr;
						const bool sourceSurfaceVec2 =
							input.SourceKind == "Vec2" &&
							(node.Type == "pc.path_repeat" ||
							 (node.Type == "pc.text" &&
							  (input.Id == "fixed_dimension" || input.Id == "offset" ||
							   input.Id == "character_range")) ||
							 (node.Type == "pc.padding" && input.Id == "dimension") ||
							 (node.Type == "pc.cellular" && (input.Id == "position" || input.Id == "size")) ||
							 ((node.Type == "pc.stripe" || node.Type == "pc.noise_strand") &&
							  input.Id == "position") ||
							 (node.Type == "pc.julia_set" &&
							  (input.Id == "c" || input.Id == "position" || input.Id == "scale")) ||
							 (node.Type == "pc.gabor_noise" &&
							  (input.Id == "position" || input.Id == "scale" || input.Id == "augment")) ||
							 ((node.Type == "pc.flow_noise" || node.Type == "pc.noise_cristal" ||
							   node.Type == "pc.caustic" || node.Type == "pc.perlin" ||
							   node.Type == "pc.voronoi_extra" || node.Type == "pc.shard_noise" ||
							   node.Type == "pc.weave" || node.Type == "pc.pytagorean_tile" ||
							   node.Type == "pc.perlin_extra" || node.Type == "pc.wavelet_noise" ||
							   node.Type == "pc.noise_scratch" || node.Type == "pc.fold_noise" ||
							   node.Type == "pc.noise_gaussian" || node.Type == "pc.noise_aniso") &&
							  (input.Id == "position" || input.Id == "scale")) ||
							 (node.Type == "pc.noise_scratch" && input.Id == "octave_shift") ||
							 (node.Type == "pc.fold_noise" && input.Id == "detail") ||
							 (node.Type == "pc.weave" && input.Id == "width"));
						const bool generatorSurfaceRange =
							(node.Type == "pc.gabor_noise" || node.Type == "pc.flow_noise" ||
							 node.Type == "pc.noise_bubble" || node.Type == "pc.noise_cristal" ||
							 node.Type == "pc.noise" || node.Type == "pc.cellular" ||
							 node.Type == "pc.perlin" || node.Type == "pc.voronoi_extra" ||
							 node.Type == "pc.shard_noise" || node.Type == "pc.noise_strand" ||
							 node.Type == "pc.pytagorean_tile" || node.Type == "pc.perlin_extra" ||
							 node.Type == "pc.wavelet_noise" || node.Type == "pc.noise_scratch" ||
							 node.Type == "pc.fold_noise" || node.Type == "pc.noise_gaussian" ||
							 node.Type == "pc.noise_aniso" || node.Type == "pc.perlin_cube" ||
							 node.Type == "pc.cellular_cube" || node.Type == "pc.simplex_cube") &&
							input.SourceKind == "SliRange" &&
							(node.Type == "pc.noise" || node.Type == "pc.cellular" ||
							 node.Type == "pc.perlin" || node.Type == "pc.perlin_extra" ||
							 node.Type == "pc.wavelet_noise" || node.Type == "pc.noise_scratch" ||
							 node.Type == "pc.fold_noise" || node.Type == "pc.noise_gaussian" ||
							 node.Type == "pc.noise_aniso" || node.Type == "pc.perlin_cube" ||
							 node.Type == "pc.cellular_cube" || node.Type == "pc.simplex_cube" ||
							 node.Type == "pc.noise_strand" || input.Id == "level_in" ||
							 input.Id == "level_out" ||
							 (node.Type == "pc.flow_noise" && input.Id == "detail") ||
							 (node.Type == "pc.noise_bubble" &&
							  (input.Id == "scale" || input.Id == "opacity")));
						const ImageArray *generatorSurfaceArray =
							(sourceSurfaceVec2 &&
							 (node.Type == "pc.julia_set" || node.Type == "pc.gabor_noise" ||
							  node.Type == "pc.flow_noise" || node.Type == "pc.noise_cristal" ||
							  node.Type == "pc.caustic" || node.Type == "pc.cellular" ||
							  node.Type == "pc.perlin" || node.Type == "pc.voronoi_extra" ||
							  node.Type == "pc.shard_noise" || node.Type == "pc.noise_strand" ||
							  node.Type == "pc.weave" || node.Type == "pc.pytagorean_tile" ||
							  node.Type == "pc.perlin_extra" || node.Type == "pc.wavelet_noise" ||
							  node.Type == "pc.noise_scratch" || node.Type == "pc.fold_noise" ||
							  node.Type == "pc.noise_gaussian" || node.Type == "pc.noise_aniso")) ||
									generatorSurfaceRange
								? FindImageArrayOutput(results[sourceIndex], link->FromPort)
								: nullptr;
						if ((!source || found == source->end()) && (surface || generatorSurfaceArray) &&
							(input.SourceKind == "Dimension" || sourceSurfaceVec2 || generatorSurfaceRange)) {
							context.Values.emplace_back(
								input.Id,
								Vector2{
									surface ? static_cast<double>(surface->Width) : 1.,
									surface ? static_cast<double>(surface->Height) : 1.
								}
							);
							if (sourceSurfaceVec2 && surface) context.Images.emplace_back(input.Id, surface);
							if (generatorSurfaceArray && sourceSurfaceVec2) {
								const SourceSocketDomain domain{
									ValueType::Image, std::nullopt, SourceSocketKind::Surface
								};
								const auto foundDomain = std::find_if(
									context.InputDomains.begin(),
									context.InputDomains.end(),
									[&](const auto &item) { return item.first == input.Id; }
								);
								if (foundDomain == context.InputDomains.end())
									context.InputDomains.emplace_back(input.Id, domain);
								else
									foundDomain->second = domain;
							}
							continue;
						}
						if (!source || found == source->end()) {
							SetDiagnostic(
								diagnostic,
								Status::UnsupportedExecution,
								"value input needs a typed source",
								node.Id,
								std::string(input.Id)
							);
							return diagnostic.Code;
						}
						value = &found->Data;
					} else {
						const auto resolved = std::find_if(
							plan.ResolvedInputs.begin(),
							plan.ResolvedInputs.end(),
							[&](const ResolvedInput &entry) {
								return entry.NodeId == node.Id && entry.Port == input.Id;
							}
						);
						// Parent animators keep their local controls while explicit aliases borrow storage.
						const AuthoredValue *parentAnimator =
							node.Type == "pc.group_input" && input.Id == "parent_value"
								? FindValue(inputOwner(input.Id), inputAnimatorPort(input.Id))
								: nullptr;
						if (parentAnimator) {
							value = &parentAnimator->Data;
						} else if (resolved != plan.ResolvedInputs.end()) {
							context.LinkedValues.emplace_back(input.Id);
							value = &resolved->Data;
						} else if (const AuthoredValue *authored =
									   FindValue(inputOwner(input.Id), inputAnimatorPort(input.Id)))
							value = &authored->Data;
					}
					if (value) {
						if (input.Type == ValueType::Text && !detail::SourceFontInput(node.Type, input.Id) &&
							detail::ContainsFontLiteral(*value)) {
							SetDiagnostic(
								diagnostic,
								Status::TypeMismatch,
								"owned Font cannot enter an unrelated Text getter",
								node.Id,
								input.Id
							);
							return diagnostic.Code;
						}
						// Heightmap's Gradient getter converts the complete declared Color value before row
						// selection.
						const auto colourDomain = context.InputDomain(input.Id);
						bool heightmapJunctionColour = false;
						if (!colourDomain && node.Type == "pc.heightmap_project_3_d" &&
							input.Id == "height_color") {
							// A resolved junction default has no producer result, but retains its authored
							// socket type.
							const auto incoming = std::find_if(
								document.Links.begin(), document.Links.end(), [&](const Link &route) {
									return route.ToNode == inputOwner(input.Id).Id &&
										   route.ToPort == input.Id;
								}
							);
							if (incoming != document.Links.end() && incoming->FromPort == "value") {
								const auto junction = std::find_if(
									document.Junctions.begin(),
									document.Junctions.end(),
									[&](const Junction &candidate) {
										return candidate.Id == incoming->FromNode;
									}
								);
								heightmapJunctionColour = junction != document.Junctions.end() &&
														  junction->Type == ValueType::Colour;
							}
						}
						if (context.IsLinked(input.Id) && node.Type == "pc.heightmap_project_3_d" &&
							input.Id == "height_color" && input.SourceKind == "Gradient" &&
							((colourDomain && colourDomain->Kind == SourceSocketKind::Colour) ||
							 heightmapJunctionColour) &&
							!std::holds_alternative<Gradient>(*value)) {
							const auto *colour = std::get_if<Colour>(value);
							const auto *colours = std::get_if<ArrayValue>(value);
							const size_t keys = colour	  ? 1
												: colours ? (colours->Items.empty() ? colours->Elements.size()
																					: colours->Items.size())
														  : 0;
							if (!keys || keys > 64 || (colours && !colours->Nested.empty())) {
								SetDiagnostic(
									diagnostic,
									Status::UnsupportedExecution,
									"heightmap Color gradient needs one to 64 flat native colors",
									node.Id,
									std::string(input.Id)
								);
								return diagnostic.Code;
							}
							const auto keyColour = [&](size_t i) -> const Colour * {
								if (colour) return colour;
								const ElementValue *leaf =
									colours->Items.empty()
										? &colours->Elements[i]
										: std::get_if<ElementValue>(&colours->Items[i].Data);
								return leaf ? std::get_if<Colour>(leaf) : nullptr;
							};
							for (size_t i = 0; i < keys; ++i)
								if (!keyColour(i)) {
									SetDiagnostic(
										diagnostic,
										Status::UnsupportedExecution,
										"heightmap Color gradient needs flat native colors",
										node.Id,
										std::string(input.Id)
									);
									return diagnostic.Code;
								}
							if (colours && !detail::ValidPayload(*colours, true)) {
								SetDiagnostic(
									diagnostic,
									Status::InvalidValue,
									"heightmap Color gradient payload exceeds native limits",
									node.Id,
									std::string(input.Id)
								);
								return diagnostic.Code;
							}
							auto projectedCharge = budget.Reserve(keys * sizeof(GradientKey));
							if (!projectedCharge || !inputCharge->Merge(std::move(*projectedCharge))) {
								SetDiagnostic(
									diagnostic,
									Status::LimitExceeded,
									"heightmap Color gradient exceeds input storage",
									node.Id,
									std::string(input.Id)
								);
								return diagnostic.Code;
							}
							Gradient projected;
							projected.Keys.reserve(keys);
							if (projected.Keys.capacity() > keys) {
								auto excess =
									budget.Reserve((projected.Keys.capacity() - keys) * sizeof(GradientKey));
								if (!excess || !inputCharge->Merge(std::move(*excess))) {
									SetDiagnostic(
										diagnostic,
										Status::LimitExceeded,
										"heightmap Color gradient capacity exceeds input storage",
										node.Id,
										std::string(input.Id)
									);
									return diagnostic.Code;
								}
							}
							for (size_t i = 0; i < keys; ++i)
								projected.Keys.push_back({colour ? 0. : double(i) / keys, *keyColour(i)});
							context.Values.emplace_back(input.Id, std::move(projected));
							continue;
						}
						if (linked ||
							SourceQuaternionKeysPresent(document, request.GroupReplay, node.Id, input.Id)) {
							if (const auto converted = SourceQuaternionGetterProjection(
									document, node.Id, input.Id, *value, linked
								)) {
								context.Values.emplace_back(input.Id, *converted);
								continue;
							}
						}
						const Node &owner = inputOwner(input.Id);
						context.ValueViews.emplace_back(
							input.Id,
							linked ? value : SourceEmptyGroupVectorView(&document, owner, input.Id, value)
						);
					} else if (!input.Default.empty()) {
						Value fallback;
						const Status parsed =
							detail::ReadValueText(input.Default, fallback, budget, *inputCharge);
						if (parsed != Status::Ok) {
							SetDiagnostic(
								diagnostic,
								parsed,
								"catalogue default could not be admitted and parsed",
								node.Id,
								std::string(input.Id)
							);
							return diagnostic.Code;
						}
						context.Values.emplace_back(input.Id, std::move(fallback));
						context.CatalogueDefaultInputs.emplace_back(input.Id);
					}
				}
				// Dynamic group inputs follow the same order: a link, then the instance
				// default.
				for (const DynamicInput &input : node.DynamicInputs) {
					if ((frozenTunnelGetter && input.Id != "value_in") ||
						!detail::ReadsSourceInput(inputSelection, index, input.Id))
						continue;
					if (!detail::SourceFrameCacheReadsPort(context.FrameCacheInputReads, input.Id)) continue;
					if (const auto *common = detail::CommonInputRoute(document, plan, node.Id, input.Id)) {
						Value value;
						detail::AllocationReservation commonCharge;
						if (detail::ReadSourceCommonGetter(
								document,
								plan,
								common->OwnerId,
								common->Selector,
								request,
								budget,
								value,
								commonCharge,
								diagnostic
							) != Status::Ok)
							return diagnostic.Code;
						context.Values.emplace_back(input.Id, std::move(value));
						context.LinkedValues.emplace_back(input.Id);
						context.InputDomains.emplace_back(
							input.Id, detail::CommonSelectorDomain(common->Selector)
						);
						if (!scratchCharge.Merge(std::move(commonCharge))) std::terminate();
						continue;
					}
					const auto link = std::find_if(
						plan.EffectiveLinks.begin(), plan.EffectiveLinks.end(), [&](const Link &candidate) {
							return candidate.ToNode == node.Id && candidate.ToPort == input.Id;
						}
					);
					if (input.Type == ValueType::Image) {
						if (node.Type == "pc.hlsl" && link == plan.EffectiveLinks.end()) {
							const Node &owner = inputOwner(input.Id);
							const auto declared = std::find_if(
								owner.DynamicInputs.begin(),
								owner.DynamicInputs.end(),
								[&](const auto &candidate) {
									return candidate.Id == inputAnimatorPort(input.Id);
								}
							);
							if (declared != owner.DynamicInputs.end() && declared->Default &&
								detail::SourceHlslArgumentValue(
									node,
									input.Id,
									*declared->Default,
									context.Find(
										"argument_type_" +
										input.Id.substr(std::string_view("argument_value_").size())
									)
								)) {
								if (const auto *surface = std::get_if<SurfaceValue>(&*declared->Default))
									context.Images.emplace_back(input.Id, &surface->Data);
								else
									context.ValueViews.emplace_back(input.Id, &*declared->Default);
								continue;
							}
						}
						if (link == plan.EffectiveLinks.end() && input.Default &&
							detail::SourceLuaArgumentType(node, input.Id)) {
							context.ValueViews.emplace_back(input.Id, &*input.Default);
							continue;
						}
						if (link != plan.EffectiveLinks.end()) {
							const size_t sourceIndex = nodeIndices.at(link->FromNode);
							if (produced[sourceIndex])
								if (const ImageArray *array =
										FindImageArrayOutput(results[sourceIndex], link->FromPort)) {
									context.ImageArrays.emplace_back(input.Id, array);
									continue;
								}
						}
						const Image *image = nullptr;
						if (!resolveImage(input.Id, false, image)) return diagnostic.Code;
						if (image) context.Images.emplace_back(input.Id, image);
						continue;
					}
					if (link != plan.EffectiveLinks.end()) {
						context.LinkedValues.emplace_back(input.Id);
						const size_t sourceIndex = nodeIndices.at(link->FromNode);
						if (input.Type == ValueType::Any && produced[sourceIndex]) {
							if (const ImageArray *array =
									FindImageArrayOutput(results[sourceIndex], link->FromPort)) {
								context.ImageArrays.emplace_back(input.Id, array);
								continue;
							}
							if (const Image *image = FindImageOutput(results[sourceIndex], link->FromPort)) {
								context.Images.emplace_back(input.Id, image);
								continue;
							}
						}
						const ValueOutputs *source =
							produced[sourceIndex] ? FindValueOutputs(results[sourceIndex]) : nullptr;
						const AuthoredValue *found = nullptr;
						if (source)
							for (const AuthoredValue &entry : *source)
								if (entry.Port == link->FromPort) found = &entry;
						if (!found) {
							SetDiagnostic(
								diagnostic,
								Status::UnsupportedExecution,
								"value input needs a typed source",
								node.Id,
								input.Id
							);
							return diagnostic.Code;
						}
						context.ValueViews.emplace_back(input.Id, &found->Data);
					} else {
						{
							const auto resolvedAt = [&](std::string_view ownerId) {
								return std::find_if(
									plan.ResolvedInputs.begin(),
									plan.ResolvedInputs.end(),
									[&](const auto &value) {
										return value.NodeId == ownerId && value.Port == input.Id;
									}
								);
							};
							auto resolved = resolvedAt(node.Id);
							if (resolved == plan.ResolvedInputs.end())
								resolved = resolvedAt(inputOwner(input.Id).Id);
							if (resolved != plan.ResolvedInputs.end()) {
								context.LinkedValues.emplace_back(input.Id);
								context.ValueViews.emplace_back(input.Id, &resolved->Data);
								continue;
							}
						}
						std::optional<Quaternion> projection;
						if (const auto *view = SharedGroupInputView(
								document, request.GroupReplay, node, input.Id, &projection
							)) {
							if (projection)
								context.Values.emplace_back(input.Id, *projection);
							else
								context.ValueViews.emplace_back(input.Id, view);
							continue;
						}
						const Node &defaultsNode = inputOwner(input.Id);
						if (const auto *sampled = FindValue(defaultsNode, inputAnimatorPort(input.Id))) {
							context.ValueViews.emplace_back(input.Id, &sampled->Data);
							continue;
						}
						const auto selected = std::find_if(
							defaultsNode.DynamicInputs.begin(),
							defaultsNode.DynamicInputs.end(),
							[&](const auto &candidate) { return candidate.Id == inputAnimatorPort(input.Id); }
						);
						if (selected != defaultsNode.DynamicInputs.end() && selected->Default)
							context.ValueViews.emplace_back(input.Id, &*selected->Default);
					}
				}
				detail::EvaluationVector<Value> separatedSamples{detail::EvaluationAllocator<Value>(budget)};
				const auto axisReadDiagnostic = [&](const detail::SourceAxisStorageView &source,
													std::string_view port) {
					const bool initialization = source.Code == Status::SourceAxisInitializationRequired;
					SetDiagnostic(
						diagnostic,
						source.Code,
						std::string(source.Message),
						initialization && source.Owner ? source.Owner->Id : node.Id,
						std::string(initialization ? source.Port : port)
					);
				};
				const auto separatedInput = [&](std::string_view port) -> detail::SourceAxisStorageView {
					if ((frozenTunnelGetter && port != "value_in") ||
						!detail::ReadsSourceInput(inputSelection, index, port) ||
						detail::SourceConsumerVectorIndex(node.Type, port) || context.IsLinked(port) ||
						!detail::SourceFrameCacheReadsPort(context.FrameCacheInputReads, port) ||
						!detail::SourceSeparatedVec2Input(node, port))
						return {};
					const auto &owner = inputOwner(port);
					const auto ownerPort = inputAnimatorPort(port);
					const auto *binding = request.GroupReplay && request.GroupReplay->InstancesBound()
											  ? request.GroupReplay->Binding(node.Id, port)
											  : nullptr;
					const bool writerAnimated =
						binding && !detail::InheritedMovedSourceGetter(node, port, binding)
							? binding->Writer == GroupSubtypeAnimator::Animated
							: std::find(
								  owner.SourceAnimatedInputs.begin(),
								  owner.SourceAnimatedInputs.end(),
								  ownerPort
							  ) != owner.SourceAnimatedInputs.end();
					return detail::ResolveSourceGetterAxes(
						document,
						document.Nodes[index],
						port,
						request.GroupReplay,
						owner.Id,
						ownerPort,
						writerAnimated,
						scheduleWork
					);
				};
				size_t separatedCount = 0;
				const auto countSeparatedInput = [&](std::string_view port) {
					const auto source = separatedInput(port);
					if (source.Code != Status::Ok) {
						axisReadDiagnostic(source, port);
						return false;
					}
					separatedCount += source.Axes != nullptr;
					return true;
				};
				for (const auto &input : catalogueEntry->Inputs)
					if (!countSeparatedInput(input.Id)) return diagnostic.Code;
				for (const auto &input : node.DynamicInputs)
					if (!countSeparatedInput(input.Id)) return diagnostic.Code;
				separatedSamples.reserve(separatedCount);
				const auto sampleSeparatedInput = [&](std::string_view port) {
					const auto source = separatedInput(port);
					if (source.Code != Status::Ok) {
						axisReadDiagnostic(source, port);
						return false;
					}
					if (!source.Axes) return true;
					const auto *axes = source.Axes;
					const bool getterAnimated = detail::SourcePropertyGetterAnimated(
													document, document.Nodes[index], port, request.GroupReplay
					)
													.value_or(false);
					Vector2 pair;
					for (size_t axis = 0; axis < 2; ++axis) {
						double value = 0;
						const auto status = detail::SampleSeparatedScalar(
							axes->Axes[axis],
							source.Track,
							document.Timeline ? &*document.Timeline : nullptr,
							request,
							getterAnimated,
							source.WriterAnimated,
							budget,
							value,
							diagnostic
						);
						if (status != Status::Ok) return false;
						if (axis == 0)
							pair.X = value;
						else
							pair.Y = value;
					}
					separatedSamples.emplace_back(pair);
					bool replaced = false;
					for (auto &view : context.ValueViews)
						if (view.first == port) {
							view.second = &separatedSamples.back();
							replaced = true;
						}
					if (!replaced) context.ValueViews.emplace_back(port, &separatedSamples.back());
					return true;
				};
				for (const auto &input : catalogueEntry->Inputs)
					if (!sampleSeparatedInput(input.Id)) return diagnostic.Code;
				for (const auto &input : node.DynamicInputs)
					if (!sampleSeparatedInput(input.Id)) return diagnostic.Code;
				std::array<Value, 7> mirrorAxisSamples{};
				const auto consumerVectorPorts = detail::SourceConsumerVectorPorts(node.Type);
				if (!consumerVectorPorts.empty()) {
					for (size_t i = 0; i < consumerVectorPorts.size(); ++i) {
						const auto port = consumerVectorPorts[i];
						if ((frozenTunnelGetter && port != "value_in") ||
							!detail::ReadsSourceInput(inputSelection, index, port))
							continue;
						const auto *binding = request.GroupReplay && request.GroupReplay->InstancesBound()
												  ? request.GroupReplay->Binding(node.Id, port)
												  : nullptr;
						if (detail::InheritedMovedSourceGetter(node, port, binding)) binding = nullptr;
						const auto rawPort = detail::SourceGetterPort(node, port, binding);
						const Node &rawNode = binding ? timelineOverrides.Find(
															nodeIndices.at(binding->OwnerId),
															document.Nodes[nodeIndices.at(binding->OwnerId)]
														)
													  : node;
						const Value *raw =
							binding ? SharedGroupInputView(document, request.GroupReplay, node, port)
									: nullptr;
						const auto mode = detail::SourceMirrorGetterAnimated(
							document, document.Nodes[index], port, request.GroupReplay
						);
						if (!raw && (!mode || *mode))
							if (const auto *value = FindValue(rawNode, rawPort)) raw = &value->Data;
						if (!raw)
							for (const auto &key : document.Keyframes)
								if (key.NodeId == rawNode.Id && key.Port == rawPort) {
									raw = &key.Data;
									break;
								}
						if (!raw)
							if (const auto *value = FindValue(rawNode, rawPort)) raw = &value->Data;
						const bool writerAnimated = binding
														? binding->Writer == GroupSubtypeAnimator::Animated
														: std::find(
															  rawNode.SourceAnimatedInputs.begin(),
															  rawNode.SourceAnimatedInputs.end(),
															  rawPort
														  ) != rawNode.SourceAnimatedInputs.end();
						detail::SourceAxisStorageView source;
						const auto *linkedValue = context.IsLinked(port) ? context.Find(port) : nullptr;
						const bool linkedPath = linkedValue && std::holds_alternative<Path2D>(*linkedValue);
						if (detail::SourceFrameCacheReadsPort(context.FrameCacheInputReads, port)) {
							if (linkedPath)
								source = detail::ResolveLocalSourceAxes(
									document,
									document.Nodes[index],
									port,
									request.GroupReplay && request.GroupReplay->InstancesBound()
										? request.GroupReplay->Bindings()
										: std::span<const GroupSubtypeBinding>{},
									request.GroupReplay ? request.GroupReplay->SharedSubtypes()
														: std::span<const GroupSubtypeOverlay>{},
									request.GroupReplay ? request.GroupReplay->DetachedAnimators()
														: std::span<const DetachedSourceAnimator>{},
									rawNode.Id,
									rawPort,
									writerAnimated,
									scheduleWork
								);
							else if (!context.IsLinked(port))
								source = detail::ResolveSourceGetterAxes(
									document,
									document.Nodes[index],
									port,
									request.GroupReplay,
									rawNode.Id,
									rawPort,
									writerAnimated,
									scheduleWork
								);
						}
						if (source.Code != Status::Ok) {
							axisReadDiagnostic(source, port);
							return diagnostic.Code;
						}
						if (const auto *axes = source.Axes) {
							bool axisGetterAnimated = mode.value_or(false);
							if (linkedPath) {
								if (!detail::AdmitSourceAxisWork(
										scheduleWork,
										document.Links.size() +
											document.Nodes[index].SourceAnimatedInputs.size()
									)) {
									SetDiagnostic(
										diagnostic,
										Status::LimitExceeded,
										"source path axis mode lookup exceeds work bounds",
										node.Id,
										std::string(port)
									);
									return diagnostic.Code;
								}
								const bool localLink = std::any_of(
									document.Links.begin(), document.Links.end(), [&](const auto &link) {
										return link.ToNode == node.Id && link.ToPort == port;
									}
								);
								if (localLink)
									axisGetterAnimated =
										std::find(
											document.Nodes[index].SourceAnimatedInputs.begin(),
											document.Nodes[index].SourceAnimatedInputs.end(),
											port
										) != document.Nodes[index].SourceAnimatedInputs.end();
							}
							Vector2 pair;
							for (size_t axis = 0; axis < 2; ++axis) {
								double value = 0;
								const Status status = detail::SampleSeparatedScalar(
									axes->Axes[axis],
									source.Track,
									document.Timeline ? &*document.Timeline : nullptr,
									request,
									axisGetterAnimated,
									source.WriterAnimated,
									budget,
									value,
									diagnostic
								);
								if (status != Status::Ok) return status;
								if (axis == 0)
									pair.X = value;
								else
									pair.Y = value;
							}
							mirrorAxisSamples[i] = pair;
							raw = &mirrorAxisSamples[i];
							if (!context.IsLinked(port)) {
								// The separated animator replaces local raw storage, never a linked producer.
								for (auto &view : context.ValueViews)
									if (view.first == port) view.second = raw;
							}
						}
						context.MirrorRawAnimators[i] = raw;
					}
				}
				detail::EvaluationVector<detail::PcxInputProgram> pcxPrograms{
					detail::EvaluationAllocator<detail::PcxInputProgram>(budget)
				};
				pcxPrograms.reserve(inputCount);
				const auto appendPcxProgram = [&](std::string_view port) {
					if ((node.Type == "pc.tunnel_in" || (groupProcess && node.Type == "pc.tunnel_out")) &&
						(port == "name" || port == "scope"))
						return;
					if ((frozenTunnelGetter && port != "value_in") ||
						!detail::ReadsSourceInput(inputSelection, index, port))
						return;
					const Node *expressionOwner =
						SourceExpressionOwner(document, plan, node, port, request.GroupReplay);
					if (!expressionOwner) return;
					const auto &owner = *expressionOwner;
					const auto found = std::find_if(
						owner.SourceInputExpressions.begin(),
						owner.SourceInputExpressions.end(),
						[&](const auto &program) { return program.Port == port; }
					);
					if (found != owner.SourceInputExpressions.end() && found->Enabled)
						pcxPrograms.push_back({&owner, &*found});
				};
				for (const auto &input : catalogueEntry->Inputs)
					appendPcxProgram(input.Id);
				for (const auto &input : node.DynamicInputs)
					appendPcxProgram(input.Id);
				context.InputProvenanceResolved = true;
				if (!detail::ApplyPcxInputExpressions(context, pcxPrograms)) {
					if (pendingPcxRoute) {
						if (!stagePcxDependency()) return diagnostic.Code;
						continue;
					}
					SetDiagnostic(
						diagnostic, context.FailureCode, context.FailureMessage, node.Id, context.FailurePort
					);
					return diagnostic.Code;
				}
				if (!detail::StampSourcePathShiftInputs(context)) {
					diagnostic = {context.FailureCode, node.Id, context.FailurePort, context.FailureMessage};
					return diagnostic.Code;
				}
				if (!detail::ResolveSimulationInputAliases(context)) {
					SetDiagnostic(
						diagnostic, context.FailureCode, context.FailureMessage, node.Id, context.FailurePort
					);
					return diagnostic.Code;
				}
				detail::SourceMirrorPathProjection mirrorPaths(context);
				if (!mirrorPaths.Prepare()) {
					diagnostic = {context.FailureCode, node.Id, context.FailurePort, context.FailureMessage};
					return diagnostic.Code;
				}
				if (groupRefresh && node.Id == groupRefresh->Event->NodeId) {
					if (!detail::ApplyGroupRefreshContext(
							context, *groupRefresh->Event, *groupRefresh->Owner, document
						)) {
						SetDiagnostic(
							diagnostic,
							context.FailureCode,
							context.FailureMessage,
							node.Id,
							context.FailurePort
						);
						return diagnostic.Code;
					}
					groupRefresh->Captured = true;
					diagnostic = {};
					return Status::Ok;
				}
				if (node.Type == "pc.tunnel_in") {
					SourceSocketDomain domain;
					if (!detail::SourceTunnelSenderDomain(context, !frozenTunnelGetter, domain)) {
						diagnostic = {
							context.FailureCode, node.Id, context.FailurePort, context.FailureMessage
						};
						return diagnostic.Code;
					}
					std::erase_if(context.InputDomains, [](const auto &entry) {
						return entry.first == "value_in";
					});
					context.InputDomains.emplace_back("value_in", domain);
				}
				if ((dynamicPcx ||
					 std::any_of(
						 document.Groups.begin(),
						 document.Groups.end(),
						 [&](const Group &group) {
							 return group.OwnerNodeId == node.Id && node.Type != "pc.pixel_builder";
						 }
					 ) ||
					 std::any_of(
						 plan.PcxNamedDependencies.begin(),
						 plan.PcxNamedDependencies.end(),
						 [&](const auto &route) { return route.Producer == index && route.Input; }
					 )) &&
					!inlineInputs[index].Captured) {
					auto &owner = inlineInputs[index];
					NodeInputCapture capture{
						node.Id,
						&owner.Values,
						&owner.Images,
						&owner.SurfacePolicy,
						&owner.Charge,
						&owner.ImageArrays,
						nullptr,
						false
					};
					detail::SourceGetterProjection projection(context);
					if (!projection.Prepare() ||
						!CaptureNodeInputs(
							context,
							capture,
							dynamicPcx ? std::span<NodeResult>{} : std::span<NodeResult>{results},
							dynamicPcx ? std::span<detail::AllocationReservation>{}
									   : std::span<detail::AllocationReservation>{resultCharges}
						)) {
						SetDiagnostic(
							diagnostic,
							context.FailureCode,
							context.FailureMessage,
							node.Id,
							context.FailurePort
						);
						return diagnostic.Code;
					}
					const uint64_t viewBytes =
						owner.Values.size() *
							(sizeof(std::pair<std::string_view, const Value *>) + sizeof(std::string_view)) +
						owner.Images.size() * sizeof(std::pair<std::string_view, const Image *>) +
						owner.ImageArrays.size() * sizeof(std::pair<std::string_view, const ImageArray *>);
					auto viewsCharge = budget.Reserve(viewBytes);
					if (!viewsCharge || !owner.Charge.Merge(std::move(*viewsCharge))) {
						SetDiagnostic(
							diagnostic,
							Status::LimitExceeded,
							"inline owner input views exceed evaluation budget",
							node.Id
						);
						return diagnostic.Code;
					}
					owner.ValueViews.reserve(owner.Values.size());
					owner.LinkedValues.reserve(owner.Values.size());
					owner.ImageViews.reserve(owner.Images.size());
					owner.ImageArrayViews.reserve(owner.ImageArrays.size());
					for (const auto &value : owner.Values) {
						owner.ValueViews.emplace_back(value.Port, &value.Data);
						if (value.Linked) owner.LinkedValues.emplace_back(value.Port);
					}
					for (const auto &image : owner.Images)
						owner.ImageViews.emplace_back(image.Port, &image.Data);
					for (const auto &array : owner.ImageArrays)
						owner.ImageArrayViews.emplace_back(array.Port, &array.Data);
					owner.Captured = true;
				}

				CacheGroupReplayOutput tunnelGetter, tunnelKey;
				detail::AllocationReservation tunnelGetterCharge, tunnelKeyCharge;
				if (node.Type == "pc.tunnel_out") {
					std::string_view key;
					if (groupProcess) {
						EvaluationRequest keyRequest = request;
						keyRequest.DataReplay = currentData;

						if (ResolveSourceTunnelRegistry(
								document,
								keyRequest,
								currentPlan,
								budget,
								tunnelKeyCharge,
								diagnostic,
								true,
								nullptr,
								&node,
								&tunnelKey,
								{results.data(), results.size()},
								{produced.data(), produced.size()},
								currentData,
								"name",
								&tunnelWork
							) != Status::Ok)
							return diagnostic.Code;
						const auto *text = std::get_if<std::string>(&*tunnelKey.Data);
						if (!text) {
							diagnostic = {
								Status::InvalidValue,
								node.Id,
								"name",
								"current source tunnel key must be text"
							};
							return diagnostic.Code;
						}
						key = *text;
						context.ValueViews.emplace_back("name", &*tunnelKey.Data);
					} else if (const auto *value = context.Find("name")) {
						const auto *text = std::get_if<std::string>(value);
						if (!text) {
							diagnostic = {
								Status::TypeMismatch,
								node.Id,
								"name",
								"current source tunnel key must be text"
							};
							return diagnostic.Code;
						}
						key = *text;
					}
					if (context.FailureCode != Status::Ok) {
						diagnostic = {context.FailureCode, node.Id, "name", context.FailureMessage};
						return diagnostic.Code;
					}
					std::optional<size_t> local, global;
					for (const auto &sender : sourceTunnelRegistry) {
						const uint64_t units =
							1 + std::min(key.size(), sender.Name.size()) + sender.NodeId.size();
						if (units > 64'000'000 - scheduleWork) {
							diagnostic = {
								Status::LimitExceeded,
								node.Id,
								"name",
								"current tunnel lookup exceeds bounded text work"
							};
							return diagnostic.Code;
						}
						scheduleWork += units;
						if (key.empty() || sender.Name != key) continue;
						const auto producer = nodeIndices.at(sender.NodeId);
						if (sender.Scope == 1.) {
							const uint64_t groupUnits =
								1 + std::min(document.Nodes[producer].GroupId.size(), node.GroupId.size());
							if (groupUnits > 64'000'000 - scheduleWork) {
								diagnostic = {
									Status::LimitExceeded,
									node.Id,
									"name",
									"current tunnel scope lookup exceeds bounded text work"
								};
								return diagnostic.Code;
							}
							scheduleWork += groupUnits;
							if (document.Nodes[producer].GroupId == node.GroupId) local = producer;
						} else
							global = producer;
					}
					const auto selected = local ? local : global;
					if (selected) {
						const auto &captured = inlineInputs[*selected];
						if (groupProcess) {
							EvaluationRequest getterRequest = request;
							getterRequest.DataReplay = currentData;

							if (ResolveSourceTunnelRegistry(
									document,
									getterRequest,
									currentPlan,
									budget,
									tunnelGetterCharge,
									diagnostic,
									true,
									nullptr,
									&document.Nodes[*selected],
									&tunnelGetter,
									{results.data(), results.size()},
									{produced.data(), produced.size()},
									currentData,
									"value_in",
									&tunnelWork
								) != Status::Ok)
								return diagnostic.Code;
							context.TunnelInput.Matched = true;
							context.TunnelInput.Data = &*tunnelGetter.Data;
							context.TunnelInput.Domain = tunnelGetter.Domain;
							context.TunnelInput.DataImageArray = tunnelGetter.ImageArrayPayload;
						} else if (!captured.Captured) {
							demandedTunnelGetters[*selected] = 1;
							pcxNames.Await(
								*selected, detail::SourceTunnelRouteName, "value_in", true, diagnostic
							);
							if (!stagePcxDependency()) return diagnostic.Code;
							continue;
						}
						if (!groupProcess) {
							context.TunnelInput.Matched = true;
							for (const auto &input : captured.Values)
								if (input.Port == "value_in") {
									context.TunnelInput.Data = &input.Data;
									context.TunnelInput.Domain = input.Domain;
								}
							for (const auto &input : captured.Images)
								if (input.Port == "value_in") {
									context.TunnelInput.Surface = &input.Data;
									context.TunnelInput.Domain = input.Domain;
								}
							for (const auto &input : captured.ImageArrays)
								if (input.Port == "value_in") {
									context.TunnelInput.Surfaces = &input.Data;
									context.TunnelInput.Domain = input.Domain;
								}
						}
					}
				}

				if (nodeInputs && node.Id == nodeInputs->NodeId) {
					const bool executeCaptured =
						batch && (retainedTargets[index] || remainingConsumers[index] != 0);
					{
						detail::SourceGetterProjection projection(context);
						// Execution still borrows upstream payloads, so capture must copy rather than
						// transfer them.
						const auto captureResults =
							executeCaptured ? std::span<NodeResult>{} : std::span<NodeResult>{results};
						const auto captureCharges =
							executeCaptured ? std::span<detail::AllocationReservation>{}
											: std::span<detail::AllocationReservation>{resultCharges};
						if (!projection.Prepare() ||
							!CaptureNodeInputs(context, *nodeInputs, captureResults, captureCharges)) {
							SetDiagnostic(
								diagnostic,
								context.FailureCode,
								context.FailureMessage,
								node.Id,
								context.FailurePort
							);
							return diagnostic.Code;
						}
					}
					inputsCaptured = true;
					if (!batch) return Status::Ok;
					if (!executeCaptured) {
						for (const size_t source : upstream[index]) {
							if (!mutableDispatch && --remainingConsumers[source] == 0 &&
								source != targetIndex && !retainedTargets[source]) {
								evaluationBytes -= ResultBytes(results[source]);
								results[source] = Image{};
								resultCharges[source].Reset();
							}
						}
						continue;
					}
				}
				if (frozenTunnelGetter) {
					CatalogueOutputs restored;
					if (!RestoreFrozenCacheGroupOutputs(
							document.Nodes[index],
							*frozen[index],
							request.MaximumImageDimension,
							restored,
							budget,
							resultCharges[index],
							diagnostic
						))
						return diagnostic.Code;
					const uint64_t bytes = ResultBytes(restored);
					if (bytes > Limits::MaximumEvaluationBytes - evaluationBytes) {
						SetDiagnostic(
							diagnostic,
							Status::LimitExceeded,
							"frozen tunnel outputs exceed intermediate budget",
							node.Id
						);
						return diagnostic.Code;
					}
					evaluationBytes += bytes;
					results[index] = std::move(restored);
					produced[index] = 1;
					continue;
				}
				if (!executor) {
					SetDiagnostic(
						diagnostic, Status::UnsupportedExecution, "node has no native executor", node.Id
					);
					return diagnostic.Code;
				}
				if (wavPreview && node.Id == wavPreview->NodeId) {
					// Preview resolves exactly the same linked and animated controls,
					// without cloning the data output.
					for (const char *port :
						 {"path", "attribute_play", "attribute_preview_gain", "attribute_preview_shift"}) {
						const Value *value = context.Find(port);
						if (value && std::holds_alternative<ArrayValue>(*value)) {
							SetDiagnostic(
								diagnostic,
								Status::UnsupportedExecution,
								"audio preview controls cannot be arrays",
								node.Id,
								port
							);
							return diagnostic.Code;
						}
					}
					const std::string path = context.Get<std::string>("path");
					const AudioClipSource *clip = nullptr;
					if (request.AudioClips.size() > Limits::MaximumNodes) {
						SetDiagnostic(
							diagnostic, Status::LimitExceeded, "too many WAV sources", node.Id, "path"
						);
						return diagnostic.Code;
					}
					for (const auto &source : request.AudioClips) {
						if (source.SourceId != path) continue;
						if (clip) {
							SetDiagnostic(
								diagnostic, Status::DuplicateId, "duplicate WAV source name", node.Id, "path"
							);
							return diagnostic.Code;
						}
						clip = &source;
					}
					if (path.empty() || !clip) {
						SetDiagnostic(
							diagnostic,
							Status::InvalidValue,
							"WAV preview needs an explicitly loaded source",
							node.Id,
							"path"
						);
						return diagnostic.Code;
					}
					const auto &audio = clip->Data;
					if (!detail::ValidAudioPlanes(
							audio.Samples, audio.Channels, Limits::MaximumAudioClipSamples
						) ||
						!std::isfinite(audio.SampleRate) || audio.SampleRate <= 0 ||
						std::floor(audio.SampleRate) != audio.SampleRate ||
						audio.SampleRate > std::numeric_limits<uint32_t>::max()) {
						SetDiagnostic(
							diagnostic,
							Status::InvalidValue,
							"WAV preview needs finite planar audio",
							node.Id,
							"path"
						);
						return diagnostic.Code;
					}
					const double gain = context.Scalar("attribute_preview_gain", .5);
					const double shift = context.Scalar("attribute_preview_shift");
					if (!std::isfinite(gain) || !std::isfinite(shift)) {
						SetDiagnostic(
							diagnostic, Status::InvalidValue, "WAV preview controls must be finite", node.Id
						);
						return diagnostic.Code;
					}
					wavPreview->Controls = {
						path,
						context.Boolean("attribute_play", true),
						gain,
						shift,
						static_cast<uint32_t>(audio.SampleRate),
						detail::AudioChannel(audio, 0).size()
					};
					wavPreview->Captured = true;
					return Status::Ok;
				}

				if (vectorPreview && node.Id == vectorPreview->NodeId) {
					if (!detail::RunProcessorBatch(context, executor, CaptureVector2Row, vectorPreview) ||
						context.FailureCode != Status::Ok) {
						SetDiagnostic(
							diagnostic,
							context.FailureCode,
							context.FailureMessage,
							node.Id,
							context.FailurePort
						);
						return diagnostic.Code;
					}
					auto &controls = vectorPreview->Controls;
					if (!controls.ProcessorCount) {
						SetDiagnostic(
							diagnostic,
							Status::UnsupportedExecution,
							"Vector2 presentation has no scheduled value",
							node.Id
						);
						return diagnostic.Code;
					}
					// Source gizmos are hidden for multiple processor rows. Avoid copying
					// their sprite.
					if (controls.ProcessorCount == 1 && controls.Style == 2 && vectorPreview->Sprite) {
						const Image &sprite = *vectorPreview->Sprite;
						uint64_t outputBytes = sizeof(Vector2Presentation);
						for (const auto &value : context.OutputValues)
							outputBytes += detail::ValuePayloadBytes(value.Data);
						if (sprite.Width > request.MaximumImageDimension ||
							sprite.Height > request.MaximumImageDimension ||
							outputBytes > context.ByteBudget ||
							sprite.Pixels.size() > context.ByteBudget - outputBytes) {
							SetDiagnostic(
								diagnostic,
								Status::LimitExceeded,
								"Vector2 sprite exceeds presentation budget",
								node.Id,
								"gizmo_sprite"
							);
							return diagnostic.Code;
						}
						controls.Sprite = sprite;
					}
					return Status::Ok;
				}
				struct ProcessingCapture {
					SourceInputProcessingObserver *Observer;
					detail::EvaluationBudget &Budget;
					detail::AllocationReservation &Charge;
					Diagnostic &Error;
					FrameTime Frame;
				} processingCapture{
					request.SourceInputObserver,
					budget,
					ownedRequestResidency ? groupProcess->CommonAdmission->Observer : *observerCharge,
					diagnostic,
					GetFrameTime(request)
				};
				const auto observeProcessing = [](detail::NodeContext &row, void *opaque) {
					auto &state = *static_cast<ProcessingCapture *>(opaque);
					if (!state.Observer || !state.Observer->ObservesFrame(state.Frame) ||
						!state.Observer->ObservesNode(row.Authored.Id))
						return true;
					std::vector<EvaluationInputValue> values;
					std::vector<EvaluationInputImage> images;
					std::vector<SnapshotImageArray> arrays;
					std::optional<SurfaceFormat> policy;
					int64_t interpolation = 1;
					auto charge = state.Budget.Reserve(0);
					NodeInputCapture capture{
						row.Authored.Id, &values, &images, &policy, &*charge, &arrays, &interpolation
					};
					// The processor has already selected and projected this successful row.
					if (!CaptureNodeInputs(row, capture, {}, {})) {
						Diagnostic refused{
							row.FailureCode, row.Authored.Id, row.FailurePort, row.FailureMessage
						};
						if (state.Observer->CaptureRefused(row.Authored.Id, refused) != Status::Ok)
							return false;
						row.FailureCode = Status::Ok;
						row.FailureMessage.clear();
						row.FailurePort.clear();
						return true;
					}
					const auto status = state.Observer->ObserveNode(
						row.Authored.Id,
						state.Frame,
						values,
						images,
						arrays,
						state.Error,
						state.Budget.Available()
					);
					if (status != Status::Ok) return row.Fail(status, state.Error.Message);
					if (!state.Charge.Resize(state.Observer->RetainedBytes())) {
						Diagnostic refused{
							Status::LimitExceeded,
							{},
							{},
							"source processing receipt exceeds evaluation budget"
						};
						if (state.Observer->CaptureRefused(row.Authored.Id, refused) != Status::Ok)
							return row.Fail(refused.Code, refused.Message);
						if (!state.Charge.Resize(state.Observer->RetainedBytes())) {
							refused = {
								Status::LimitExceeded,
								{},
								{},
								"source processing receipt exceeds evaluation budget"
							};
							if (state.Observer->CaptureRefused({}, refused) != Status::Ok ||
								!state.Charge.Resize(state.Observer->RetainedBytes()))
								return row.Fail(
									Status::LimitExceeded,
									"source processing receipt could not release its storage"
								);
						}
					}
					return true;
				};
				if (groupProcess && groupProcess->Session) {
					if (const auto *owner = detail::CommonOwner(document, node.Id)) {
						const auto profile = detail::SourceCommonOwnerDispatch(
							document, request, size_t(owner - document.SourceCommonOwners.data())
						);
						if (groupProcess->DirectTarget == index ||
							profile.Wrapper == SourceCommonWrapperKind::Full) {
							if (detail::CaptureSourceCommonInputMap(
									context,
									*owner,
									*groupProcess->Session,
									budget,
									*groupProcess->Charge,
									diagnostic
								) != Status::Ok)
								return diagnostic.Code;
						}
					}
				}
				const auto *commonWrapper = groupProcess && groupProcess->DirectTarget != index
												? detail::CommonOwner(document, node.Id)
												: nullptr;
				const bool heldGraph =
					commonWrapper && !commonWrapper->UpdateGraph &&
					detail::SourceCommonOwnerDispatch(
						document, request, size_t(commonWrapper - document.SourceCommonOwners.data())
					)
							.Wrapper != SourceCommonWrapperKind::Unsupported;
				if (heldGraph) {
					CatalogueOutputs held;
					detail::AllocationReservation heldCharge;
					if (!RestoreFrozenCacheGroupOutputs(
							node,
							groupProcess->Outputs->Nodes[index],
							request.MaximumImageDimension,
							held,
							budget,
							heldCharge,
							diagnostic
						))
						return diagnostic.Code;
					context.OutputImages = std::move(held.Images);
					context.OutputImageArrays = std::move(held.ImageArrays);
					context.OutputValues = std::move(held.Values);
					catalogueResult.FrozenDomains = std::move(held.FrozenDomains);
					context.OutputDiagnostics = std::move(held.Diagnostics);
					if (!context.OutputCharge.Merge(std::move(heldCharge))) std::terminate();
				}
				if ((!heldGraph &&
					 !detail::RunProcessorBatch(context, executor, observeProcessing, &processingCapture)) ||
					!detail::StampSourcePathShiftProducedValues(context) ||
					context.FailureCode != Status::Ok) {
					if (pendingPcxRoute) {
						if (!stagePcxDependency()) return diagnostic.Code;
						continue;
					}
					SetDiagnostic(
						diagnostic,
						context.FailureCode,
						context.FailureMessage,
						context.FailureNodeId.empty() ? node.Id : context.FailureNodeId,
						context.FailurePort
					);
					return diagnostic.Code;
				}
				if (context.FrameCacheGroupAction) {
					auto &data = *currentData;
					const uint64_t retained = RetainedDataReplayBytes(data);
					const uint64_t replacement =
						*context.FrameCacheGroupAction == CacheGroupReplayAction::Enable
							? ClearedSourceFrameCacheReplayBytes(node, data)
							: retained;
					const bool additionalRow =
						std::none_of(data.Entries.begin(), data.Entries.end(), [&](const auto &row) {
							return row.NodeId == node.Id;
						});
					const uint64_t workspaceBytes =
						DataReplayValidationWorkspaceBytes(data, additionalRow ? 1 : 0);
					auto replacementCharge = budget.Reserve(replacement),
						 workspace = budget.Reserve(workspaceBytes);
					if (!replacementCharge || !workspace || retained > Limits::MaximumEvaluationBytes ||
						replacement > Limits::MaximumEvaluationBytes - retained ||
						workspaceBytes > Limits::MaximumEvaluationBytes - retained - replacement) {
						SetDiagnostic(
							diagnostic,
							Status::LimitExceeded,
							"cache group action exceeds the live byte budget",
							node.Id,
							"cache_group"
						);
						return diagnostic.Code;
					}
					if (ApplySourceFrameCacheGroupReplay(
							node,
							*context.FrameCacheGroupAction,
							data,
							data,
							cachePlaying,
							request.SourceCacheProject,
							diagnostic,
							retained + replacement + workspaceBytes
						) != Status::Ok)
						return diagnostic.Code;
					if (!replacementCharge->Resize(RetainedDataReplayBytes(data))) std::terminate();
					auto released = dataCharge->Split(retained);
					if (!released || !dataCharge->Merge(std::move(*replacementCharge))) std::terminate();
					std::fill(frozen.begin(), frozen.end(), nullptr);
					std::fill(tracked.begin(), tracked.end(), nullptr);
					for (auto &member : data.CacheGroups.Nodes) {
						const auto memberIndex = nodeIndices.at(member.NodeId);
						tracked[memberIndex] = &member;
						if (!CacheGroupReplayShouldRun(member)) frozen[memberIndex] = &member;
					}
					refreshFrameCacheReads();
					if (captureTarget && !frameCacheInputReads.empty())
						frameCacheInputReads[targetIndex] = detail::SourceFrameCacheInputReads::All;
					if (pendingGraph.Rebuild(
							document,
							plan,
							frameCacheInputReads,
							frozen,
							pendingRoots,
							dynamicPcxRoutes,
							completed,
							true,
							scheduleWork,
							diagnostic,
							inputSelection
						) != Status::Ok)
						return diagnostic.Code;
					groupActivityChanged = true;
				}

				if (!context.PcxControlMessages.empty()) {
					std::string failure;
					if (!request.HostProvider ||
						!request.HostProvider->PcxMessages(node.Id, context.PcxControlMessages, failure)) {
						SetDiagnostic(
							diagnostic,
							Status::UnsupportedExecution,
							failure.empty() ? "PCX notifications require an explicit host" : failure,
							node.Id
						);
						return diagnostic.Code;
					}
				}
				if (simulation) {
					for (auto &update : context.SimulationUpdates) {
						const uint64_t bytes = RetainedSimulationEntryBytes(update);
						auto admitted = context.OutputCharge.Split(bytes);
						if (!admitted || !simulation->Charge->Merge(std::move(*admitted))) {
							SetDiagnostic(
								diagnostic,
								Status::LimitExceeded,
								"simulation state lacks allocation admission",
								node.Id
							);
							return diagnostic.Code;
						}
						auto &entries = simulation->Replay->Entries;
						const auto found =
							std::find_if(entries.begin(), entries.end(), [&](const auto &entry) {
								return entry.NodeId == update.NodeId &&
									   entry.ProcessorRow == update.ProcessorRow;
							});
						if (found != entries.end()) {
							const uint64_t replacedBytes = RetainedSimulationEntryBytes(*found);
							*found = std::move(update);
							// The displaced snapshot is gone; subsequent affectors reuse its admission.
							auto released = simulation->Charge->Split(replacedBytes);
							if (!released) std::terminate();
						} else {
							if (!GrowReplayEntries(entries, budget, *simulation->Charge, diagnostic, node.Id))
								return diagnostic.Code;
							entries.push_back(std::move(update));
						}
					}
				}
				if (simulation && simulation->Surfaces) {
					for (auto &update : context.SurfaceUpdates) {
						const uint64_t bytes = RetainedSurfaceFrameEntryBytes(update);
						auto admitted = context.OutputCharge.Split(bytes);
						if (!admitted || !simulation->Charge->Merge(std::move(*admitted))) {
							SetDiagnostic(
								diagnostic,
								Status::LimitExceeded,
								"surface cache update lacks admission",
								node.Id
							);
							return diagnostic.Code;
						}
						auto &entries = simulation->Surfaces->Entries;
						const auto found =
							std::find_if(entries.begin(), entries.end(), [&](const auto &entry) {
								return entry.NodeId == update.NodeId && entry.Frame == update.Frame &&
									   entry.ProcessorRow == update.ProcessorRow;
							});
						if (found != entries.end()) {
							const uint64_t oldBytes = RetainedSurfaceFrameEntryBytes(*found);
							*found = std::move(update);
							auto released = simulation->Charge->Split(oldBytes);
							if (!released) std::terminate();
						} else {
							if (!GrowReplayEntries(entries, budget, *simulation->Charge, diagnostic, node.Id))
								return diagnostic.Code;
							entries.push_back(std::move(update));
						}
					}
				}
				if (simulation && simulation->Random) {
					for (auto &update : context.RandomUpdates) {
						const uint64_t bytes = RetainedRandomEntryBytes(update);
						auto admitted = context.OutputCharge.Split(bytes);
						if (!admitted || !simulation->Charge->Merge(std::move(*admitted))) {
							SetDiagnostic(
								diagnostic,
								Status::LimitExceeded,
								"random replay update lacks admission",
								node.Id
							);
							return diagnostic.Code;
						}
						auto &entries = simulation->Random->Entries;
						const auto found =
							std::find_if(entries.begin(), entries.end(), [&](const auto &entry) {
								return entry.NodeId == update.NodeId &&
									   entry.ProcessorRow == update.ProcessorRow;
							});
						if (found != entries.end()) {
							const uint64_t oldBytes = RetainedRandomEntryBytes(*found);
							*found = std::move(update);
							auto released = simulation->Charge->Split(oldBytes);
							if (!released) std::terminate();
						} else {
							if (!GrowReplayEntries(entries, budget, *simulation->Charge, diagnostic, node.Id))
								return diagnostic.Code;
							entries.push_back(std::move(update));
						}
					}
				}
				if (currentData) {
					for (auto &update : context.DataUpdates) {
						// Current evaluation tags remain private until the complete source
						// sampling journal is synchronized into owned candidate receipts.
						const uint64_t bytes = RetainedDataReplayEntryBytes(update);
						auto admitted = context.OutputCharge.Split(bytes);
						if (!admitted || !dataCharge->Merge(std::move(*admitted))) {
							SetDiagnostic(
								diagnostic,
								Status::LimitExceeded,
								"data replay update lacks admission",
								node.Id
							);
							return diagnostic.Code;
						}
						auto &entries = currentData->Entries;
						const auto found =
							std::find_if(entries.begin(), entries.end(), [&](const auto &entry) {
								return entry.NodeId == update.NodeId &&
									   entry.ProcessorRow == update.ProcessorRow;
							});
						if (found != entries.end()) {
							const uint64_t oldBytes = RetainedDataReplayEntryBytes(*found);
							*found = std::move(update);
							auto released = dataCharge->Split(oldBytes);
							if (!released) std::terminate();
						} else {
							if (!GrowReplayEntries(entries, budget, *dataCharge, diagnostic, node.Id))
								return diagnostic.Code;
							entries.push_back(std::move(update));
						}
					}
				}
				size_t bypassImages = 0, bypassValues = 0;
				for (const Link &link : plan.EffectiveLinks) {
					if (link.FromNode != node.Id || !link.FromPort.ends_with(BYPASS_SUFFIX)) continue;
					const auto input = std::string_view(link.FromPort)
										   .substr(0, link.FromPort.size() - BYPASS_SUFFIX.size());
					if (context.Input(input))
						++bypassImages;
					else if (context.Find(input))
						++bypassValues;
				}
				// Replacement capacity coexists with the original reserved output slots
				// during reserve.
				const uint64_t replacementBytes =
					(bypassImages ? (context.OutputImages.size() + bypassImages) *
										sizeof(std::pair<std::string, Image>)
								  : 0) +
					(bypassValues ? (context.OutputValues.size() + bypassValues) * sizeof(AuthoredValue) : 0);
				if (replacementBytes && !context.ReserveOutput(replacementBytes)) {
					SetDiagnostic(
						diagnostic, context.FailureCode, context.FailureMessage, node.Id, context.FailurePort
					);
					return diagnostic.Code;
				}
				if (bypassImages) context.OutputImages.reserve(context.OutputImages.size() + bypassImages);
				if (bypassValues) context.OutputValues.reserve(context.OutputValues.size() + bypassValues);
				for (const Link &link : plan.EffectiveLinks) {
					if (link.FromNode != node.Id || !link.FromPort.ends_with(BYPASS_SUFFIX)) continue;
					const std::string_view input =
						std::string_view(link.FromPort)
							.substr(0, link.FromPort.size() - BYPASS_SUFFIX.size());
					if (const Image *image = context.Input(input)) {
						if (!context.ReserveOutput(
								image->Pixels.size() +
									std::max(link.FromPort.size(), std::string{}.capacity()),
								link.FromPort
							)) {
							SetDiagnostic(
								diagnostic,
								context.FailureCode,
								context.FailureMessage,
								node.Id,
								context.FailurePort
							);
							return diagnostic.Code;
						}
						context.OutputImages.emplace_back(link.FromPort, *image);
					} else if (const Value *value = context.Find(input)) {
						if (!context.ReserveOutput(
								detail::RetainedPayloadBytes(*value) +
									std::max(link.FromPort.size(), std::string{}.capacity()),
								link.FromPort
							)) {
							SetDiagnostic(
								diagnostic,
								context.FailureCode,
								context.FailureMessage,
								node.Id,
								context.FailurePort
							);
							return diagnostic.Code;
						}
						context.OutputValues.push_back({link.FromPort, *value});
					}
				}
				for (auto &[id, image] : context.OutputImages)
					image.Hash = detail::PixelHash(image);
				catalogueResult.Domains = std::move(context.OutputDomains);
				catalogueResult.Diagnostics = std::move(context.OutputDiagnostics);
				catalogueResult.Images = std::move(context.OutputImages);
				catalogueResult.Values = std::move(context.OutputValues);
				catalogueResult.PixelBuilderUpdate = context.PixelBuilderUpdate;
				catalogueResult.ImageArrays = std::move(context.OutputImageArrays);
				currentCharge = context.TakeOutputReservation();
				for (auto &[id, array] : catalogueResult.ImageArrays)
					for (Image &image : array.Images)
						image.Hash = detail::PixelHash(image);

				producedCatalogue = true;
			} else if (detail::FindValueNodeSchema(node.Type)) {
				producedValue = true;
				const NodeSchema *valueSchema = detail::FindValueNodeSchema(node.Type);
				size_t inputCapacity = node.Values.size() + node.DynamicInputs.size();
				for (const PortSchema &port : valueSchema->Ports)
					if (port.Direction == PortDirection::Input) inputCapacity++;
				if (inputCapacity > Limits::MaximumEvaluationBytes / sizeof(detail::ValueInputView)) {
					SetDiagnostic(
						diagnostic,
						Status::LimitExceeded,
						"typed value inputs exceed the live byte budget",
						node.Id,
						"inputs"
					);
					return diagnostic.Code;
				}
				auto inputCharge = budget.Reserve(inputCapacity * sizeof(detail::ValueInputView));
				if (!inputCharge) {
					SetDiagnostic(
						diagnostic,
						Status::LimitExceeded,
						"typed value inputs exceed the live byte budget",
						node.Id,
						"inputs"
					);
					return diagnostic.Code;
				}
				// The source node and resolved results own these values. Keep borrowed
				// views here instead of cloning every authored string and payload before
				// evaluation.
				std::vector<detail::ValueInputView> inputs;
				inputs.reserve(inputCapacity);
				for (const AuthoredValue &input : node.Values)
					inputs.push_back({input.Port, &input.Data});
				const auto resolveValueInput = [&](std::string_view port, const Value *fallback) {
					const auto link = std::find_if(
						plan.EffectiveLinks.begin(), plan.EffectiveLinks.end(), [&](const Link &candidate) {
							return candidate.ToNode == node.Id && candidate.ToPort == port;
						}
					);
					const Value *value = fallback;
					if (link != plan.EffectiveLinks.end()) {
						const size_t sourceIndex = nodeIndices.at(link->FromNode);
						const ValueOutputs *source =
							produced[sourceIndex] ? FindValueOutputs(results[sourceIndex]) : nullptr;
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
						std::find_if(inputs.begin(), inputs.end(), [&](const detail::ValueInputView &entry) {
							return entry.Port == port;
						});
					if (existing == inputs.end())
						inputs.push_back({port, value});
					else
						existing->Data = value;
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
					budget,
					currentCharge,
					failedPort,
					failureMessage
				);

				if (status != Status::Ok) {
					SetDiagnostic(diagnostic, status, failureMessage, node.Id, failedPort);
					return diagnostic.Code;
				}

			} else if (node.Type == "image.captured") {
				const AuthoredValue *name = FindValue(node, "source_id");
				const auto *id = name ? std::get_if<std::string>(&name->Data) : nullptr;
				if (!id || id->empty() || id->size() > 255 ||
					request.ImageSources.size() > Limits::MaximumNodes) {
					SetDiagnostic(
						diagnostic,
						Status::InvalidValue,
						"captured image needs a bounded durable source ID",
						node.Id,
						"source_id"
					);
					return diagnostic.Code;
				}
				const Image *source = nullptr;
				for (const auto &capture : request.ImageSources) {
					if (capture.SourceId != *id) continue;
					if (source) {
						SetDiagnostic(
							diagnostic,
							Status::DuplicateId,
							"captured image source ID is duplicated",
							node.Id,
							"source_id"
						);
						return diagnostic.Code;
					}
					source = &capture.Data;
				}
				if (!source ||
					!ValidSurfaceLayout(*source, request.MaximumImageDimension, Limits::MaximumOutputBytes) ||
					!FiniteSurfaceSamples(*source)) {
					SetDiagnostic(
						diagnostic,
						Status::InvalidValue,
						"captured image is missing or has invalid numeric storage",
						node.Id,
						"source_id"
					);
					return diagnostic.Code;
				}
				if (!admit(source->Pixels.size(), currentCharge, "image")) return diagnostic.Code;
				result = *source;
				result.Hash = SurfaceHash(result);
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
				if (width == 0 || height == 0 || width > Limits::MaximumDimension ||
					height > Limits::MaximumDimension) {
					SetDiagnostic(
						diagnostic,
						Status::LimitExceeded,
						"solid dimensions are outside the supported range",
						node.Id,
						width == 0 || width > Limits::MaximumDimension ? "width" : "height"
					);
					return diagnostic.Code;
				}
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
				if (!admit(static_cast<size_t>(byteCount), currentCharge, "image")) return diagnostic.Code;
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
				if (!admit(source->Pixels.size(), currentCharge, "image")) return diagnostic.Code;
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
				if (!admit(source->Pixels.size(), currentCharge, "image")) return diagnostic.Code;
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
				if (!admit(4 * source->Pixels.size(), currentCharge, "image")) return diagnostic.Code;
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
				if (!admit(dimensions->Pixels.size(), currentCharge, "image")) return diagnostic.Code;
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
				if (!admit(source->Pixels.size(), currentCharge, "image")) return diagnostic.Code;
				result = *source;
				if (enabled && node.Type == "image.gamma_map") {
					const AuthoredValue *invert = FindValue(node, "invert");
					if (!admit(result.Pixels.size(), scratchCharge, "image")) return diagnostic.Code;
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
					if (!admit(2 * source->Pixels.size(), currentCharge, "image")) return diagnostic.Code;
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
					if (!admit(source->Pixels.size(), currentCharge, "image")) return diagnostic.Code;
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
				if (!admit(source->Pixels.size(), currentCharge, "image")) return diagnostic.Code;
				result = *source;
				uint64_t curveBytes = source->Pixels.size();
				for (const char *port : {"brightness", "red", "green", "blue", "alpha"}) {
					const AuthoredValue *value = FindValue(node, port);
					curveBytes += value ? detail::PayloadOwnedBytes(std::get<Curve>(value->Data))
										: 2 * sizeof(std::array<double, 6>);
				}
				if (!admit(curveBytes, scratchCharge, "image")) return diagnostic.Code;
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
				if (!admit(source->Pixels.size(), currentCharge, "image")) return diagnostic.Code;
				result = *source;
				const AuthoredValue *gradientValue = FindValue(node, "gradient");
				if (!admit(
						gradientValue ? detail::PayloadOwnedBytes(std::get<Gradient>(gradientValue->Data))
									  : 2 * sizeof(GradientKey),
						scratchCharge,
						"gradient"
					))
					return diagnostic.Code;
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
						"displace mapped, mask, channel, iteration or oversample "
						"control is unavailable",
						node.Id
					);
					return diagnostic.Code;
				}
				if (source->Pixels.size() > Limits::MaximumEvaluationBytes - evaluationBytes) {
					SetDiagnostic(diagnostic, Status::LimitExceeded, "displace exceeds byte budget", node.Id);
					return diagnostic.Code;
				}
				if (!admit(source->Pixels.size(), currentCharge, "image")) return diagnostic.Code;
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
				if (!admit(source->Pixels.size(), currentCharge, "image")) return diagnostic.Code;
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
				if (!admit(source->Pixels.size(), currentCharge, "image")) return diagnostic.Code;
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
						"color adjust palette, mapped, mask modification or mix "
						"path is unavailable",
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
				if (!admit(source->Pixels.size(), currentCharge, "image")) return diagnostic.Code;
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
				if (!admit(source->Pixels.size(), currentCharge, "image")) return diagnostic.Code;
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
				if (!admit(source->Pixels.size() + 32 * sizeof(double), scratchCharge, "image"))
					return diagnostic.Code;
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
				const std::string_view name =
					kind ? std::string_view(std::get<std::string>(kind->Data)) : "Rectangle";
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
				if (!admit(uint64_t(width) * height * 16, currentCharge, "image")) return diagnostic.Code;
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
						"checker mapped, mask, UV or non-solid render control is "
						"unavailable",
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
				if (!admit(static_cast<size_t>(bytes), currentCharge, "image")) return diagnostic.Code;
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
						produced[sourceIndex] ? FindValueOutputs(results[sourceIndex]) : nullptr;
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
				if (!admit(static_cast<size_t>(bytes), currentCharge, "image")) return diagnostic.Code;
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
				if (!admit(static_cast<size_t>(bytes), currentCharge, "image")) return diagnostic.Code;
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
				if (!admit(static_cast<size_t>(bytes), currentCharge, "image")) return diagnostic.Code;
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
				if (!admit(static_cast<size_t>(bytes), currentCharge, "image")) return diagnostic.Code;
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
					if (!admit(3 * mask->Pixels.size() + radius * sizeof(double), scratchCharge, "mask"))
						return diagnostic.Code;
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
				std::array<const ImageArray *, 2> inputArrays{};
				std::array<const Image *, 2> directImages{};
				std::array<size_t, 2> lengths{};
				for (size_t input = 0; input < inputArrays.size(); input++) {
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
					inputArrays[input] =
						FindImageArrayOutput(results[nodeIndices.at(link->FromNode)], link->FromPort);
					directImages[input] =
						FindImageOutput(results[nodeIndices.at(link->FromNode)], link->FromPort);
					if (const auto *array = inputArrays[input]) {
						lengths[input] = array->Items.size();
						producedArray = true;
					} else
						lengths[input] = 1;
				}
				detail::ArrayScheduleFootprint scheduleFootprint;
				const Status measured = detail::MeasureArraySchedule(
					lengths,
					static_cast<detail::ArrayProcessMode>(process),
					Limits::MaximumArrayElements,
					scheduleFootprint
				);
				if (measured != Status::Ok) {
					SetDiagnostic(
						diagnostic,
						measured,
						"height blend schedule exceeds native bounds",
						node.Id,
						"array_process"
					);
					return diagnostic.Code;
				}
				auto scheduleCharge = budget.Reserve(scheduleFootprint.PeakBytes);
				if (!scheduleCharge) {
					SetDiagnostic(
						diagnostic,
						Status::LimitExceeded,
						"height blend schedule exceeds live byte budget",
						node.Id,
						"array_process"
					);
					return diagnostic.Code;
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
						"height blend array schedule exceeds native bounds or "
						"has an empty input",
						node.Id,
						"array_process"
					);
					return diagnostic.Code;
				}
				if (!scheduleCharge->Resize(scheduleFootprint.RetainedBytes)) std::terminate();
				if (producedArray) {
					if (!admit(
							schedule.size() * (sizeof(ImageArrayItem) + sizeof(Image)), currentCharge, "image"
						))
						return diagnostic.Code;
					arrayResult.Items.reserve(schedule.size());
					arrayResult.Images.reserve(schedule.size());
				}
				uint64_t outputBytes = 0;
				for (const auto &row : schedule) {
					std::array<const Image *, 2> selected{};
					for (size_t input = 0; input < selected.size(); input++) {
						if (directImages[input])
							selected[input] = directImages[input];
						else {
							const ImageArray &array = *inputArrays[input];
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
					if (!admit(bytes, currentCharge, "image")) return diagnostic.Code;
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
				if (!admit(sourceImage->Pixels.size(), currentCharge, "image")) return diagnostic.Code;
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
					if (!admit(2 * mask->Pixels.size() + radius * sizeof(double), scratchCharge, "mask"))
						return diagnostic.Code;
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
				if (node.DynamicInputs.size() > MaximumDynamicInputsForNode(node)) {
					SetDiagnostic(
						diagnostic,
						Status::LimitExceeded,
						"image array input count exceeds native bounds",
						node.Id,
						"array"
					);
					return diagnostic.Code;
				}
				std::array<detail::ImageArrayInput, Limits::MaximumDynamicInputsPerNode> inputs;
				size_t inputCount = 0;
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

					if (const Image *image = FindImageOutput(results[sourceIndex], link->FromPort)) {
						inputs[inputCount++] = image;
					} else {
						const ImageArray *source = FindImageArrayOutput(results[sourceIndex], link->FromPort);
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
						inputs[inputCount++] = source;
					}
				}
				const AuthoredValue *spreadValue = FindValue(node, "spread");
				const bool spread = spreadValue ? std::get<bool>(spreadValue->Data) : false;
				detail::ImageArrayFootprint footprint;
				const auto borrowed = std::span(inputs.data(), inputCount);
				const Status measured = detail::MeasureImageArray(borrowed, spread, footprint);
				if (measured != Status::Ok ||
					footprint.Pixels > Limits::MaximumEvaluationBytes - evaluationBytes) {
					SetDiagnostic(
						diagnostic,
						measured == Status::Ok ? Status::LimitExceeded : measured,
						"image array shape or pixels exceed native bounds",
						node.Id,
						"array"
					);
					return diagnostic.Code;
				}
				const Status collectStatus =
					detail::CollectImageArray(borrowed, spread, budget, arrayResult, currentCharge);
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
					produced[sourceIndex] ? FindImageArrayOutput(results[sourceIndex], link->FromPort)
										  : nullptr;
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
				if (!admit(sourceArray->Images[*imageIndex].Pixels.size(), currentCharge, "image"))
					return diagnostic.Code;
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
			if (node.Type == "image.noise_simplex" &&
				((output != document.Outputs.end() && output->NodeId == node.Id && output->Port == "field") ||
				 std::any_of(
					 selectedOutputs.begin(),
					 selectedOutputs.end(),
					 [&](const auto *selected) {
						 return selected->NodeId == node.Id && selected->Port == "field";
					 }
				 ) ||
				 std::any_of(plan.EffectiveLinks.begin(), plan.EffectiveLinks.end(), [&](const auto &link) {
					 return link.FromNode == node.Id && link.FromPort == "field";
				 }))) {
				if (!admit(
						sizeof(NoiseFieldData) + result.Pixels.size() + sizeof(AuthoredValue) +
							sizeof(std::pair<std::string, Image>) + 2 * std::string{}.capacity(),
						currentCharge,
						"field"
					))
					return diagnostic.Code;
				NoiseFieldValue field;
				auto &data = field.Data.emplace();
				data.Components = RasterNoiseComponents(node, &document);
				data.Raster = result;
				catalogueResult.Values.push_back({"field", std::move(field)});
				catalogueResult.Images.emplace_back("image", std::move(result));
				producedCatalogue = true;
			}
			const uint64_t resultBytes = producedValue		  ? ResultBytes(valueResult)
										 : producedArray	  ? ResultBytes(arrayResult)
										 : producedShape	  ? ResultBytes(shapeResult)
										 : producedConversion ? ResultBytes(conversionResult)
										 : producedMirror	  ? ResultBytes(mirrorResult)
										 : producedCatalogue  ? ResultBytes(catalogueResult)
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
			else if (producedCatalogue)
				results[index] = std::move(catalogueResult);
			else
				results[index] = std::move(result);
			resultCharges[index] = std::move(currentCharge);
			produced[index] = 1;
			if (groupProcess && groupProcess->Run[index]) {
				auto &snapshot = groupProcess->Outputs->Nodes[index];
				if (!CaptureCacheGroupOutputs(
						node,
						results[index],
						snapshot,
						budget,
						*groupProcess->Charge,
						captureComparisonWork,
						diagnostic
					))
					return diagnostic.Code;
				const bool direct = groupProcess->DirectTarget == index;
				if (!direct) (*groupProcess->Nodes)[index].Rendered = true;
				if (groupProcess->Common && !direct) {
					const auto *owner = detail::CommonOwner(document, node.Id);
					if (owner) {
						const auto profile = detail::SourceCommonOwnerDispatch(
							document, request, size_t(owner - document.SourceCommonOwners.data())
						);
						if (profile.Wrapper == SourceCommonWrapperKind::Full) {
							const auto socket = std::find_if(
								groupProcess->Common->Owners.begin(),
								groupProcess->Common->Owners.end(),
								[&](const auto &row) {
									return row.OwnerId == owner->SourceOwnerId &&
										   row.OwnerType == owner->SourceType;
								}
							);
							if (socket == groupProcess->Common->Owners.end()) {
								SetDiagnostic(
									diagnostic,
									Status::InvalidValue,
									"full update has no common socket owner",
									node.Id
								);
								return diagnostic.Code;
							}
							socket->Updated = true;
						}
					}
				}
			}
			if (!tracked.empty() && tracked[index] &&
				!CaptureCacheGroupOutputs(
					node,
					results[index],
					*tracked[index],
					budget,
					*dataCharge,
					captureComparisonWork,
					diagnostic
				))
				return diagnostic.Code;
			// A large chain needs only the current input and output in memory.
			for (const size_t source : upstream[index]) {
				if (!dynamicPcx && --remainingConsumers[source] == 0 && source != targetIndex &&
					(!batch || !retainedTargets[source])) {
					evaluationBytes -= ResultBytes(results[source]);
					results[source] = Image{};
					resultCharges[source].Reset();
				}
			}
		}
		if (currentData) {
			for (auto &node : currentData->CacheGroups.Nodes)
				for (auto &output : node.Outputs)
					if (output.Data && !std::holds_alternative<SurfaceValue>(*output.Data) &&
						!detail::SyncSourcePathSequentialValue(
							pathShiftMemo, *output.Data, budget, *dataCharge, diagnostic
						))
						return diagnostic.Code;
			for (auto &entry : currentData->Entries)
				for (auto &frame : entry.Values) {
					if (!detail::SyncSourcePathSequentialValue(
							pathShiftMemo, frame.Data, budget, *dataCharge, diagnostic
						))
						return diagnostic.Code;
					detail::StripSourcePathShiftIdentities(frame.Data);
				}
		}
		for (size_t i = 0; i < results.size(); ++i)
			if (produced[i])
				if (auto *values = FindValueOutputs(results[i]))
					for (auto &value : *values)
						if (!detail::SyncSourcePathSequentialValue(
								pathShiftMemo, value.Data, budget, resultCharges[i], diagnostic
							))
							return diagnostic.Code;
		if (groupProcess) {
			diagnostic = {};
			return Status::Ok;
		}
		if (batch && nodeInputs && !inputsCaptured) {
			SetDiagnostic(
				diagnostic,
				Status::UnsupportedExecution,
				"selected input snapshot has no capture kernel",
				std::string(targetNodeId)
			);
			return diagnostic.Code;
		}
		if (batch) {
			uint64_t bytes = selectedOutputs.size() * sizeof(StatefulNamedOutput);
			for (const auto *selected : selectedOutputs) {
				const size_t index = nodeIndices.at(selected->NodeId);
				if (!produced[index]) {
					SetDiagnostic(
						diagnostic,
						Status::UnsupportedExecution,
						"selected batch output was not evaluated",
						selected->NodeId,
						selected->Port
					);
					return diagnostic.Code;
				}
				if (!AddBytes(bytes, std::max(selected->Id.size(), std::string{}.capacity()))) {
					SetDiagnostic(diagnostic, Status::LimitExceeded, "batch output size overflows");
					return diagnostic.Code;
				}
				if (const auto *refusal = FindOutputDiagnostic(results[index], selected->Port)) {
					diagnostic = *refusal;
					return diagnostic.Code;
				}
				uint64_t payload = 0;
				if (const auto *array = FindImageArrayOutput(results[index], selected->Port))
					payload = RetainedImageArrayBytes(*array);
				else if (const auto *image = FindImageOutput(results[index], selected->Port);
						 image &&
						 FindPortType(document.Nodes[index], selected->Port, PortDirection::Output) !=
							 ValueType::Atlas)
					payload = image->Pixels.capacity();
				else {
					const auto *values = FindValueOutputs(results[index]);
					const auto value =
						values ? std::find_if(
									 values->begin(),
									 values->end(),
									 [&](const auto &entry) { return entry.Port == selected->Port; }
								 )
							   : ValueOutputs::const_iterator{};
					if (!values || value == values->end()) {
						SetDiagnostic(
							diagnostic,
							Status::InvalidOutput,
							"node did not produce the selected batch port",
							selected->NodeId,
							selected->Port
						);
						return diagnostic.Code;
					}
					payload = detail::RetainedPayloadBytes(value->Data);
					if (!AddBytes(payload, std::max(value->Port.size(), std::string{}.capacity())))
						payload = std::numeric_limits<uint64_t>::max();
				}
				if (!AddBytes(bytes, payload)) {
					SetDiagnostic(diagnostic, Status::LimitExceeded, "batch payload size overflows");
					return diagnostic.Code;
				}
			}
			auto charge = budget.Reserve(bytes);
			if (!charge) {
				SetDiagnostic(
					diagnostic, Status::LimitExceeded, "batch replacement exceeds the live byte budget"
				);
				return diagnostic.Code;
			}
			batch->Outputs->reserve(selectedOutputs.size());
			for (const auto *selected : selectedOutputs) {
				const size_t index = nodeIndices.at(selected->NodeId);
				StatefulNamedOutput named;
				named.Id = selected->Id;
				if (const auto *array = FindImageArrayOutput(results[index], selected->Port))
					named.Output = *array;
				else if (const auto *image = FindImageOutput(results[index], selected->Port);
						 image &&
						 FindPortType(document.Nodes[index], selected->Port, PortDirection::Output) !=
							 ValueType::Atlas)
					named.Output = *image;
				else {
					const auto *values = FindValueOutputs(results[index]);
					const auto value = std::find_if(values->begin(), values->end(), [&](const auto &entry) {
						return entry.Port == selected->Port;
					});
					named.Output = EvaluatedValue{
						value->Port,
						value->Data,
						FindOutputDomain(document.Nodes[index], results[index], selected->Port)
					};
				}
				if (auto *value = std::get_if<EvaluatedValue>(&named.Output))
					detail::StripSourcePathShiftIdentities(value->Data);
				batch->Outputs->push_back(std::move(named));
			}
			if (!batch->Charge->Merge(std::move(*charge))) std::terminate();
			diagnostic = {};
			return Status::Ok;
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
		if (const auto *refusal = FindOutputDiagnostic(results[targetIndex], output->Port)) {
			diagnostic = *refusal;
			return diagnostic.Code;
		}
		if (auto *catalogue = std::get_if<CatalogueOutputs>(&results[targetIndex])) {
			const auto imageArray = std::find_if(
				catalogue->ImageArrays.begin(), catalogue->ImageArrays.end(), [&](const auto &entry) {
					return entry.first == output->Port;
				}
			);
			const auto image =
				std::find_if(catalogue->Images.begin(), catalogue->Images.end(), [&](const auto &entry) {
					return entry.first == output->Port;
				});
			const auto value = std::find_if(
				catalogue->Values.begin(), catalogue->Values.end(), [&](const AuthoredValue &entry) {
					return entry.Port == output->Port;
				}
			);
			if (imageArray != catalogue->ImageArrays.end())
				outputValue = std::move(imageArray->second);
			else if (image != catalogue->Images.end())
				outputValue = std::move(image->second);
			else if (value != catalogue->Values.end()) {
				auto selectionCharge = budget.Reserve(sizeof(AuthoredValue));
				if (!selectionCharge) {
					SetDiagnostic(
						diagnostic,
						Status::LimitExceeded,
						"selected output storage exceeds the byte budget",
						output->NodeId,
						output->Port
					);
					return diagnostic.Code;
				}
				// initializer_list elements are const and would clone the moved typed
				// payload.
				CatalogueOutputs selected;
				selected.Values.reserve(1);
				selected.Values.push_back(std::move(*value));
				// Dynamic group declarations travel with the raw value through final
				// output selection.
				selected.Domains = std::move(catalogue->Domains);
				selected.FrozenDomains = std::move(catalogue->FrozenDomains);
				outputValue = std::move(selected);
				if (!resultCharges[targetIndex].Merge(std::move(*selectionCharge))) std::terminate();
			} else {
				SetDiagnostic(
					diagnostic,
					Status::InvalidOutput,
					"node did not produce the selected output",
					output->NodeId,
					output->Port
				);
				return diagnostic.Code;
			}
		} else if (std::holds_alternative<MirrorOutputs>(results[targetIndex])) {
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
		if (auto *values = FindValueOutputs(outputValue))
			for (auto &value : *values)
				detail::StripSourcePathShiftIdentities(value.Data);
		outputCharge = std::move(resultCharges[targetIndex]);
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

	Status ReplayGroupRefresh(
		const Document &document,
		const Plan &plan,
		std::span<const GroupRefreshEvent> events,
		const GroupReplayState &previous,
		uint64_t revision,
		GroupReplayState &result,
		Diagnostic &diagnostic,
		uint64_t maximumBytes
	) try {
		ENGINE_PROFILE("imagegraph.group_refresh");
		if (events.empty() && previous.RetainedBytes() && previous.AuthoringRevision() != revision) {
			SetDiagnostic(
				diagnostic, Status::InvalidValue, "group replay revision changed without an explicit event"
			);
			return diagnostic.Code;
		}
		const uint64_t destinationBytes = &previous == &result ? 0 : result.RetainedBytes();
		auto candidate = detail::CloneGroupReplay(
			previous, events.size(), revision, maximumBytes, destinationBytes, diagnostic
		);
		if (!candidate) return diagnostic.Code;
		GroupReplayState working;
		detail::GroupReplayAccess::Install(working, std::move(candidate));
		auto &owner = *detail::GroupReplayAccess::Get(working);
		detail::AllocationReservation planCharge;
		Plan checked;
		const Status compiled = CompileWithBudget(document, checked, diagnostic, owner.Budget, planCharge);
		if (compiled != Status::Ok) return compiled;
		if (checked != plan) {
			SetDiagnostic(
				diagnostic, Status::InvalidOutput, "group replay plan does not match the authored document"
			);
			return diagnostic.Code;
		}
		for (const auto &entry : previous.Entries()) {
			const auto node =
				std::find_if(document.Nodes.begin(), document.Nodes.end(), [&](const Node &node) {
					return node.Id == entry.NodeId && node.Type == "pc.group_input";
				});
			if (node == document.Nodes.end()) {
				SetDiagnostic(
					diagnostic, Status::InvalidGroup, "group replay has a stale input boundary", entry.NodeId
				);
				return diagnostic.Code;
			}
		}
		for (const auto &event : events) {
			if (event.Reason != GroupRefreshReason::Load && event.Reason != GroupRefreshReason::Edit &&
				event.Reason != GroupRefreshReason::Connect &&
				event.Reason != GroupRefreshReason::ParentEdit &&
				event.Reason != GroupRefreshReason::Restore) {
				SetDiagnostic(
					diagnostic, Status::InvalidValue, "group refresh event reason is invalid", event.NodeId
				);
				return diagnostic.Code;
			}
			const auto node =
				std::find_if(document.Nodes.begin(), document.Nodes.end(), [&](const Node &node) {
					return node.Id == event.NodeId && node.Type == "pc.group_input";
				});
			if (node == document.Nodes.end()) {
				SetDiagnostic(
					diagnostic,
					Status::InvalidGroup,
					"group refresh needs a native input boundary",
					event.NodeId
				);
				return diagnostic.Code;
			}
			EvaluationRequest request = event.At;
			request.GroupReplay = &working;
			request.GroupAuthoringRevision = revision;
			const Status validation = ValidateEvaluationRequest(request, diagnostic);
			if (validation != Status::Ok) return validation;
			detail::AllocationReservation ignoredCharge;
			NodeResult ignored;
			GroupRefreshCapture capture{&event, &owner};
			const Status evaluated = EvaluateGraph(
				document,
				plan,
				{},
				request,
				ignored,
				diagnostic,
				owner.Budget,
				ignoredCharge,
				nullptr,
				nullptr,
				nullptr,
				nullptr,
				event.NodeId,
				{},
				true,
				&capture
			);
			if (evaluated != Status::Ok) return evaluated;
			if (!capture.Captured) {
				SetDiagnostic(
					diagnostic,
					Status::UnsupportedExecution,
					"group refresh controls were not resolved",
					event.NodeId
				);
				return diagnostic.Code;
			}
		}
		// Release transient compile storage before transferring its ledger owner to
		// the caller.
		checked = {};
		planCharge.Reset();
		owner.PreviousShadow.Reset();
		result = std::move(working);
		diagnostic = {};
		return Status::Ok;
	} catch (const std::bad_alloc &) {
		SetDiagnostic(diagnostic, Status::LimitExceeded, "group refresh allocation was refused");
		return diagnostic.Code;
	}

	static Status EvaluateResult(
		const Document &document,
		const Plan &plan,
		const std::string &outputId,
		const EvaluationRequest &request,
		EvaluationResult &evaluation,
		Diagnostic &diagnostic,
		WavPreviewCapture *wavPreview = nullptr,
		Vector2PresentationCapture *vectorPreview = nullptr
	) {
		ENGINE_PROFILE("imagegraph.evaluate");
		auto &outputValue = evaluation.Data;
		auto &budget = evaluation.Budget;
		auto &outputCharge = evaluation.Charge;
		const Status validation = ValidateEvaluationRequest(request, diagnostic);
		if (validation != Status::Ok) return validation;
		return EvaluateGraph(
			document,
			plan,
			outputId,
			request,
			outputValue,
			diagnostic,
			budget,
			outputCharge,
			wavPreview,
			vectorPreview
		);
	}

	Status ResolveWavPreviewControls(
		const Document &document,
		const std::string &nodeId,
		const EvaluationRequest &request,
		WavPreviewControls &controls,
		Diagnostic &diagnostic
	) {
		ENGINE_PROFILE("imagegraph.evaluate");
		const Status requestStatus = ValidateEvaluationRequest(request, diagnostic);
		if (requestStatus != Status::Ok) return requestStatus;
		EvaluationResult evaluation;
		detail::AllocationReservation originalPlanCharge;
		Plan original;
		const Status checked =
			CompileWithBudget(document, original, diagnostic, evaluation.Budget, originalPlanCharge);
		if (checked != Status::Ok) return checked;
		const auto node = std::find_if(document.Nodes.begin(), document.Nodes.end(), [&](const Node &entry) {
			return entry.Id == nodeId;
		});
		if (node == document.Nodes.end() || node->Type != "pc.wav_file_read") {
			SetDiagnostic(diagnostic, Status::UnknownNode, "audio preview needs a WAV file node", nodeId);
			return diagnostic.Code;
		}
		WavPreviewCapture capture{nodeId, {}, false};
		NodeResult ignored;
		const Status evaluated = EvaluateGraph(
			document,
			original,
			{},
			request,
			ignored,
			diagnostic,
			evaluation.Budget,
			evaluation.Charge,
			&capture,
			nullptr,
			nullptr,
			nullptr,
			nodeId,
			{},
			true
		);
		if (evaluated != Status::Ok) return evaluated;
		if (!capture.Captured) {
			SetDiagnostic(
				diagnostic, Status::UnsupportedExecution, "WAV preview controls were not resolved", nodeId
			);
			return diagnostic.Code;
		}
		controls = std::move(capture.Controls);
		return Status::Ok;
	}

	Status ResolveVector2Presentation(
		const Document &document,
		const std::string &nodeId,
		const EvaluationRequest &request,
		Vector2Presentation &result,
		Diagnostic &diagnostic
	) {
		const Status requestStatus = ValidateEvaluationRequest(request, diagnostic);
		if (requestStatus != Status::Ok) return requestStatus;
		EvaluationResult evaluation;
		detail::AllocationReservation originalPlanCharge;
		Plan original;
		const Status checked =
			CompileWithBudget(document, original, diagnostic, evaluation.Budget, originalPlanCharge);
		if (checked != Status::Ok) return checked;
		const auto node = std::find_if(document.Nodes.begin(), document.Nodes.end(), [&](const Node &entry) {
			return entry.Id == nodeId;
		});
		if (node == document.Nodes.end() || node->Type != "pc.vector2") {
			SetDiagnostic(
				diagnostic, Status::UnknownNode, "Vector2 presentation needs a Vector2 node", nodeId
			);
			return diagnostic.Code;
		}
		Vector2PresentationCapture capture{nodeId, {}, nullptr};
		NodeResult ignored;
		const Status evaluated = EvaluateGraph(
			document,
			original,
			{},
			request,
			ignored,
			diagnostic,
			evaluation.Budget,
			evaluation.Charge,
			nullptr,
			&capture,
			nullptr,
			nullptr,
			nodeId,
			{},
			true
		);
		if (evaluated != Status::Ok) return evaluated;
		result = std::move(capture.Controls);
		return Status::Ok;
	}

#include "SourceCommonInputRead.inc"

	namespace detail {
		static Status EvaluateSourceInputImpl(
			const Document &document,
			const Plan &plan,
			std::string_view nodeId,
			std::string_view port,
			const EvaluationRequest &request,
			EvaluationBudget &budget,
			Value &result,
			AllocationReservation &resultCharge,
			Diagnostic &diagnostic,
			bool planAlreadyValidated,
			std::optional<std::span<const AuthoredValue>> observedInputs,
			std::string_view observedInputOwner
		) try {
			ENGINE_PROFILE("imagegraph.source_input_getter");
			const auto fail = [&](Status code, std::string_view message) {
				SetDiagnostic(
					diagnostic,
					code,
					std::string(message),
					nodeId.size() <= Limits::MaximumTextBytes ? nodeId : std::string_view{},
					port.size() <= Limits::MaximumTextBytes ? port : std::string_view{}
				);
				return code;
			};
			if (document.Nodes.size() > Limits::MaximumNodes || nodeId.size() > Limits::MaximumTextBytes ||
				port.size() > Limits::MaximumTextBytes)
				return fail(Status::LimitExceeded, "source input target exceeds text bounds");
			const auto node =
				std::find_if(document.Nodes.begin(), document.Nodes.end(), [&](const auto &candidate) {
					return candidate.Id == nodeId;
				});
			if (node == document.Nodes.end()) return fail(Status::UnknownNode, "source input node is absent");
			if (node->DynamicInputs.size() > MaximumDynamicInputsForNode(*node))
				return fail(Status::LimitExceeded, "source input declaration count exceeds bounds");
			if (!SourceSeparatedVec2Input(*node, port))
				return fail(Status::UnknownPort, "source input is not a declared two-axis getter");
			const auto requestStatus = ValidateEvaluationRequest(request, diagnostic);
			if (requestStatus != Status::Ok) return requestStatus;
			uint64_t observedBytes = 0, mapWork = 0;
			if (observedInputs) {
				if (observedInputs->size() > Limits::MaximumKeyframes)
					return fail(Status::LimitExceeded, "observed input map exceeds port bounds");
				for (size_t i = 0; i < observedInputs->size(); ++i) {
					const auto &input = (*observedInputs)[i];
					const auto bytes = ValueClonePayloadBytes(input.Data);
					if (input.Port.empty() || input.Port.size() > Limits::MaximumTextBytes || !bytes ||
						!AddBytes(
							observedBytes,
							detail::RetainedPayloadBytes(input.Data) + sizeof(AuthoredValue) +
								input.Port.capacity()
						))
						return fail(Status::LimitExceeded, "observed input map payload exceeds bounds");
					for (size_t j = 0; j < i; ++j) {
						const uint64_t comparison =
							1 + std::min((*observedInputs)[j].Port.size(), input.Port.size());
						if (comparison > 64'000'000 - mapWork)
							return fail(
								Status::LimitExceeded, "observed input map lookup exceeds work bounds"
							);
						mapWork += comparison;
						if ((*observedInputs)[j].Port == input.Port)
							return fail(Status::InvalidValue, "observed input map has duplicate ports");
					}
				}
			}
			auto observedShadow = budget.Reserve(observedBytes);
			if (!observedShadow)
				return fail(Status::LimitExceeded, "observed input map overlap exceeds budget");
			AllocationReservation charge;
			std::vector<EvaluationInputValue> values;
			std::vector<EvaluationInputImage> images;
			std::vector<SnapshotImageArray> arrays;
			std::optional<SurfaceFormat> surfacePolicy;
			NodeInputCapture capture{
				nodeId,
				&values,
				&images,
				&surfacePolicy,
				&charge,
				&arrays,
				nullptr,
				false,
				port,
				observedInputs,
				observedInputOwner.empty() ? nodeId : observedInputOwner
			};
			NodeResult ignored;
			const auto status = EvaluateGraph(
				document,
				plan,
				{},
				request,
				ignored,
				diagnostic,
				budget,
				charge,
				nullptr,
				nullptr,
				&capture,
				nullptr,
				nodeId,
				{},
				planAlreadyValidated
			);
			if (status != Status::Ok) return status;
			const auto found = std::find_if(values.begin(), values.end(), [&](const auto &input) {
				return input.Port == port;
			});
			if (found == values.end())
				return fail(Status::UnsupportedExecution, "source getter has no represented value");
			const auto retained = sizeof(Value) + detail::RetainedPayloadBytes(found->Data);
			if (retained > charge.Bytes())
				return fail(Status::LimitExceeded, "source getter result exceeds its admitted payload");
			static_assert(std::is_nothrow_move_assignable_v<Value>);
			result = std::move(found->Data);
			std::vector<EvaluationInputValue>{}.swap(values);
			std::vector<EvaluationInputImage>{}.swap(images);
			std::vector<SnapshotImageArray>{}.swap(arrays);
			if (!charge.Resize(retained)) std::terminate();
			resultCharge = std::move(charge);
			diagnostic = {};
			return Status::Ok;
		} catch (const std::bad_alloc &) {
			SetDiagnostic(
				diagnostic, Status::LimitExceeded, "source input allocation was refused", nodeId, port
			);
			return diagnostic.Code;
		}
	}

	Status detail::CompileSourceDocument(
		const Document &document,
		Plan &plan,
		EvaluationBudget &budget,
		AllocationReservation &planCharge,
		Diagnostic &diagnostic
	) {
		return CompileWithBudget(document, plan, diagnostic, budget, planCharge);
	}

	Status detail::EvaluateSourceInput(
		const Document &document,
		const Plan &plan,
		std::string_view nodeId,
		std::string_view port,
		const EvaluationRequest &request,
		EvaluationBudget &budget,
		Value &result,
		AllocationReservation &resultCharge,
		Diagnostic &diagnostic,
		std::optional<std::span<const AuthoredValue>> observedInputs,
		std::string_view observedInputOwner
	) {
		if (const auto selector = CommonSelector(port)) {
			if (*selector != SourceCommonSelector::Update) {
				diagnostic = {
					Status::UnknownPort,
					std::string(nodeId),
					std::string(port),
					"common source selector is not an input"
				};
				return diagnostic.Code;
			}
			const auto *owner = CommonOwner(document, nodeId);
			if (!owner) {
				SetDiagnostic(
					diagnostic,
					Status::UnknownNode,
					"common input has no registered owner",
					std::string(nodeId)
				);
				return diagnostic.Code;
			}
			return ReadSourceCommonGetter(
				document,
				plan,
				owner->SourceOwnerId,
				*selector,
				request,
				budget,
				result,
				resultCharge,
				diagnostic
			);
		}
		return EvaluateSourceInputImpl(
			document,
			plan,
			nodeId,
			port,
			request,
			budget,
			result,
			resultCharge,
			diagnostic,
			false,
			observedInputs,
			observedInputOwner
		);
	}

	Status detail::EvaluateSourceInput(
		const Document &document,
		std::string_view nodeId,
		std::string_view port,
		const EvaluationRequest &request,
		EvaluationBudget &budget,
		Value &result,
		AllocationReservation &resultCharge,
		Diagnostic &diagnostic,
		std::optional<std::span<const AuthoredValue>> observedInputs,
		std::string_view observedInputOwner
	) {
		AllocationReservation planCharge;
		Plan plan;
		const auto compiled = CompileWithBudget(document, plan, diagnostic, budget, planCharge);
		if (compiled != Status::Ok) return compiled;
		return EvaluateSourceInputImpl(
			document,
			plan,
			nodeId,
			port,
			request,
			budget,
			result,
			resultCharge,
			diagnostic,
			true,
			observedInputs,
			observedInputOwner
		);
	}

	Status EvaluateNodeInputs(
		const Document &document,
		const Plan &plan,
		std::string_view nodeId,
		const EvaluationRequest &request,
		EvaluationSnapshot &snapshot,
		Diagnostic &diagnostic,
		uint64_t maximumBytes
	) try {
		if (maximumBytes == 0 || maximumBytes > Limits::MaximumEvaluationBytes) {
			SetDiagnostic(
				diagnostic,
				maximumBytes == 0 ? Status::InvalidValue : Status::LimitExceeded,
				"node snapshot byte cap is outside native limits",
				std::string(nodeId)
			);
			return diagnostic.Code;
		}
		const Status requestStatus = ValidateEvaluationRequest(request, diagnostic);
		if (requestStatus != Status::Ok) return requestStatus;
		const auto node =
			std::find_if(document.Nodes.begin(), document.Nodes.end(), [&](const Node &candidate) {
				return candidate.Id == nodeId;
			});
		if (node == document.Nodes.end()) {
			SetDiagnostic(
				diagnostic, Status::InvalidValue, "selected node does not exist", std::string(nodeId)
			);
			return diagnostic.Code;
		}
		if (!FindCatalogueEntry(node->Type) && !FindSchema(node->Type)) {
			SetDiagnostic(
				diagnostic,
				Status::UnsupportedExecution,
				"node input snapshot needs a declared input schema",
				std::string(nodeId)
			);
			return diagnostic.Code;
		}
		const uint64_t oldBytes = snapshot.RetainedBytes();
		const uint64_t metadataBytes = sizeof(EvaluationSnapshot::Storage);
		if (oldBytes > maximumBytes || metadataBytes > maximumBytes - oldBytes) {
			SetDiagnostic(
				diagnostic,
				Status::LimitExceeded,
				"existing and replacement node snapshots exceed the byte cap",
				std::string(nodeId)
			);
			return diagnostic.Code;
		}
		auto candidate = std::make_unique<EvaluationSnapshot::Storage>(maximumBytes);
		if (oldBytes != 0) {
			auto shadow = candidate->Budget.Reserve(oldBytes);
			if (!shadow) {
				SetDiagnostic(
					diagnostic,
					Status::LimitExceeded,
					"existing and replacement node snapshots exceed the byte cap",
					std::string(nodeId)
				);
				return diagnostic.Code;
			}
			candidate->ReplacementShadow = std::move(*shadow);
		}
		auto metadata = candidate->Budget.Reserve(metadataBytes);
		if (!metadata || !candidate->Charge.Merge(std::move(*metadata))) {
			SetDiagnostic(
				diagnostic,
				Status::LimitExceeded,
				"replacement node snapshot metadata exceeds the byte cap",
				std::string(nodeId)
			);
			return diagnostic.Code;
		}
		NodeInputCapture capture{
			nodeId,
			&candidate->Values,
			&candidate->Images,
			&candidate->SurfacePolicy,
			&candidate->Charge,
			&candidate->ImageArrays,
			&candidate->InterpolationPolicy
		};
		NodeResult ignored;
		const Status status = EvaluateGraph(
			document,
			plan,
			{},
			request,
			ignored,
			diagnostic,
			candidate->Budget,
			candidate->Charge,
			nullptr,
			nullptr,
			&capture,
			nullptr,
			nodeId
		);
		if (status != Status::Ok) return status;
		snapshot.Data.reset();
		candidate->ReplacementShadow.Reset();
		snapshot.Data = std::move(candidate);
		diagnostic = {};
		return Status::Ok;
	} catch (const std::bad_alloc &) {
		SetDiagnostic(
			diagnostic,
			Status::LimitExceeded,
			"node input snapshot allocation was refused",
			std::string(nodeId)
		);
		return diagnostic.Code;
	}

	static bool RetainedAuthoredValuesBytes(const std::vector<AuthoredValue> &values, uint64_t &bytes) {
		bytes = 0;
		if (!AddArrayBytes(bytes, values.capacity(), sizeof(AuthoredValue))) return false;
		for (const AuthoredValue &value : values)
			if (!AddBytes(bytes, value.Port.capacity()) ||
				!AddBytes(bytes, detail::RetainedPayloadBytes(value.Data)))
				return false;
		return true;
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
		const Status requestStatus = ValidateEvaluationRequest(request, diagnostic);
		if (requestStatus != Status::Ok) return requestStatus;
		const auto node =
			std::find_if(document.Nodes.begin(), document.Nodes.end(), [&](const Node &candidate) {
				return candidate.Id == nodeId;
			});
		if (node == document.Nodes.end()) {
			SetDiagnostic(diagnostic, Status::InvalidValue, "selected node does not exist", nodeId);
			return diagnostic.Code;
		}
		uint64_t oldBytes = 0;
		if (!RetainedAuthoredValuesBytes(values, oldBytes)) {
			SetDiagnostic(
				diagnostic, Status::LimitExceeded, "existing node values exceed the live byte budget", nodeId
			);
			return diagnostic.Code;
		}
		detail::EvaluationBudget budget(Limits::MaximumEvaluationBytes);
		detail::AllocationReservation oldShadow;
		if (oldBytes != 0) {
			auto reservation = budget.Reserve(oldBytes);
			if (!reservation) {
				SetDiagnostic(
					diagnostic,
					Status::LimitExceeded,
					"existing and resolved node values exceed the live byte budget",
					nodeId
				);
				return diagnostic.Code;
			}
			oldShadow = std::move(*reservation);
		}
		detail::AllocationReservation newCharge;
		std::vector<AuthoredValue> resolvedValues;
		NodeValuesCapture capture{nodeId, &resolvedValues, &newCharge};
		NodeResult ignored;
		const Status status = EvaluateGraph(
			document,
			plan,
			{},
			request,
			ignored,
			diagnostic,
			budget,
			newCharge,
			nullptr,
			nullptr,
			nullptr,
			&capture,
			nodeId,
			outputId
		);
		if (status != Status::Ok) return status;
		values = std::move(resolvedValues);
		oldShadow.Reset();
		diagnostic = {};
		return Status::Ok;
	}

	Status Evaluate(
		const Document &document,
		const Plan &plan,
		const std::string &outputId,
		const EvaluationRequest &request,
		Image &image,
		Diagnostic &diagnostic,
		uint64_t maximumBytes
	) {
		if (!maximumBytes || maximumBytes > Limits::MaximumEvaluationBytes) {
			SetDiagnostic(
				diagnostic, Status::LimitExceeded, "image evaluation byte cap is outside native bounds"
			);
			return diagnostic.Code;
		}
		EvaluationResult result(maximumBytes);
		const Status status = EvaluateResult(document, plan, outputId, request, result, diagnostic);
		if (status != Status::Ok) return status;
		if (auto *single = std::get_if<Image>(&result.Data)) {
			image = std::move(*single);
			return Status::Ok;
		}
		if (auto *catalogue = std::get_if<CatalogueOutputs>(&result.Data);
			catalogue && catalogue->Values.size() == 1) {
			auto &value = catalogue->Values.front();
			if (auto *atlas = std::get_if<AtlasValue>(&value.Data);
				atlas && atlas->Data && atlas->Data->Kind == AtlasKind::SurfaceAtlas &&
				detail::ValidRuntimeValue(value.Data) &&
				ValidSurfaceLayout(atlas->Data->Surface.Data, Limits::MaximumDimension, maximumBytes)) {
				// The typed result retains Atlas identity; the image overload transfers its owned pixels.
				image = std::move(atlas->Data->Surface.Data);
				return Status::Ok;
			}
		}
		ImageArray *array = std::get_if<ImageArray>(&result.Data);
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

	template <class ReplayResult>
	static Status EvaluateReplayInternal(
		const Document &document,
		const Plan &plan,
		const std::string &outputId,
		const EvaluationRequest &request,
		ReplayResult &result,
		Diagnostic &diagnostic,
		uint64_t maximumBytes,
		std::span<const std::string> batchIds = {},
		std::string_view inputNodeId = {},
		GroupProcessCapture *groupProcess = nullptr
	) try {
		constexpr bool inputSupport = std::is_same_v<ReplayResult, StatefulInputEvaluationResult>;
		constexpr bool batchSupport = std::is_same_v<ReplayResult, StatefulOutputEvaluationResult>;
		constexpr bool surfaceSupport =
			std::is_same_v<ReplayResult, StatefulEvaluationResult> || batchSupport || inputSupport;
		auto &simulationResult = [&]() -> SimulationReplayState & {
			if constexpr (surfaceSupport)
				return result.Simulation;
			else
				return result.Replay;
		}();
		const auto fail = [&](Status status, const char *message) {
			SetDiagnostic(diagnostic, status, message);
			return status;
		};
		if (!maximumBytes || maximumBytes > Limits::MaximumEvaluationBytes ||
			document.Nodes.size() > Limits::MaximumNodes)
			return fail(Status::LimitExceeded, "simulation evaluation byte or node cap is outside bounds");
		const Status validated = ValidateEvaluationRequest(request, diagnostic);
		if (validated != Status::Ok) return validated;
		const auto cacheActions = ValidateSimulationCacheActions(document, request, diagnostic);
		if (cacheActions != Status::Ok) return cacheActions;
		std::array<std::string_view, Limits::MaximumNodes> processRoots{};
		size_t processRootCount = 0;
		if (groupProcess) {
			for (size_t index = 0; index < groupProcess->Run.size(); ++index)
				if (groupProcess->Run[index]) processRoots[processRootCount++] = document.Nodes[index].Id;
		}
		const auto cone = AnalyzeStatefulTemporalCone(
			document,
			plan,
			inputSupport ? batchIds : (batchSupport ? batchIds : std::span<const std::string>(&outputId, 1)),
			inputNodeId,
			groupProcess ? std::span<const std::string_view>(processRoots.data(), processRootCount)
						 : request.SimulationCacheCaptures
		);
		if (!cone.Valid) return fail(Status::InvalidOutput, "selected temporal closure is invalid");

		const Status destination = ValidateSimulationReplay(simulationResult, maximumBytes, diagnostic);
		if (destination != Status::Ok) return destination;
		if (request.SimulationReplay) {
			const Status prior =
				ValidateSimulationReplay(*request.SimulationReplay, maximumBytes, diagnostic);
			if (prior != Status::Ok) return prior;
		}
		if constexpr (surfaceSupport) {

			if (ValidateRigidReplay(result.Rigid, maximumBytes, diagnostic) != Status::Ok)
				return diagnostic.Code;
			if (request.RigidReplay &&
				ValidateRigidReplay(*request.RigidReplay, maximumBytes, diagnostic) != Status::Ok)
				return diagnostic.Code;
			if (ValidateRandomReplay(result.Random, maximumBytes, diagnostic) != Status::Ok)
				return diagnostic.Code;
			if (request.RandomReplay &&
				ValidateRandomReplay(*request.RandomReplay, maximumBytes, diagnostic) != Status::Ok)
				return diagnostic.Code;
			if (ValidateDataReplay(result.Data, maximumBytes, diagnostic) != Status::Ok)
				return diagnostic.Code;
			if (request.DataReplay &&
				ValidateDataReplay(*request.DataReplay, maximumBytes, diagnostic) != Status::Ok)
				return diagnostic.Code;
			if (ValidateSurfaceFrameReplay(result.Surfaces, maximumBytes, diagnostic) != Status::Ok)
				return diagnostic.Code;
			if (request.SurfaceReplay &&
				ValidateSurfaceFrameReplay(*request.SurfaceReplay, maximumBytes, diagnostic) != Status::Ok)
				return diagnostic.Code;
			const bool temporal = cone.SurfaceCaches != 0;
			if (temporal &&
				(request.NegativeFrame || request.Subframe != 0 ||
				 (request.SurfaceReplay && request.SurfaceReplay->Initialized && !request.ResetSurfaceReplay
					  ? request.Tick != request.SurfaceReplay->Tick &&
							(request.SurfaceReplay->Tick >= Limits::MaximumTick ||
							 request.Tick != request.SurfaceReplay->Tick + 1)
					  : request.Tick != 0)))
				return fail(
					Status::InvalidValue, "surface replay requires reset at zero or contiguous integer frames"
				);
		}
		EvaluationResult evaluation(maximumBytes);
		std::unique_ptr<StatefulSnapshotAccess::Storage> inputStorage;
		if constexpr (inputSupport)
			inputStorage = std::make_unique<StatefulSnapshotAccess::Storage>(maximumBytes);
		auto &budget = inputSupport ? inputStorage->Budget : evaluation.Budget;
		auto &outputCharge = inputSupport ? inputStorage->Charge : evaluation.Charge;
		if constexpr (inputSupport) {
			auto metadata = budget.Reserve(sizeof(StatefulSnapshotAccess::Storage));
			if (!metadata || !outputCharge.Merge(std::move(*metadata)))
				return fail(Status::LimitExceeded, "stateful input metadata exceeds the byte cap");
		}
		uint64_t retained = RetainedSimulationReplayBytes(simulationResult);
		if (request.SimulationReplay && request.SimulationReplay != &simulationResult &&
			!AddBytes(retained, RetainedSimulationReplayBytes(*request.SimulationReplay)))
			return fail(Status::LimitExceeded, "simulation prior state size overflows");
		if constexpr (surfaceSupport) {
			if (!AddBytes(retained, RetainedSurfaceFrameReplayBytes(result.Surfaces)) ||
				(request.SurfaceReplay && request.SurfaceReplay != &result.Surfaces &&
				 !AddBytes(retained, RetainedSurfaceFrameReplayBytes(*request.SurfaceReplay))))
				return fail(Status::LimitExceeded, "surface retained owner size overflows");
		}
		if constexpr (surfaceSupport) {
			if (!AddBytes(retained, RetainedRandomReplayBytes(result.Random)) ||
				(request.RandomReplay && request.RandomReplay != &result.Random &&
				 !AddBytes(retained, RetainedRandomReplayBytes(*request.RandomReplay))))
				return fail(Status::LimitExceeded, "random replay prior size overflows");
		}
		if constexpr (surfaceSupport) {
			if (!AddBytes(retained, RetainedDataReplayBytes(result.Data)) ||
				(request.DataReplay && request.DataReplay != &result.Data &&
				 !AddBytes(retained, RetainedDataReplayBytes(*request.DataReplay))))
				return fail(Status::LimitExceeded, "data replay prior size overflows");
		}

		if constexpr (surfaceSupport) {
			if (!AddBytes(retained, RetainedRigidReplayBytes(result.Rigid)) ||
				(request.RigidReplay && request.RigidReplay != &result.Rigid &&
				 !AddBytes(retained, RetainedRigidReplayBytes(*request.RigidReplay))))
				return fail(Status::LimitExceeded, "rigid replay prior size overflows");
		}
		const uint64_t oldOutput = [&]() -> uint64_t {
			if constexpr (inputSupport)
				return result.Inputs.RetainedBytes();
			else if constexpr (batchSupport)
				return RetainedStatefulOutputBytes(result);
			else
				return std::visit(
					[](const auto &output) -> uint64_t {
						using T = std::decay_t<decltype(output)>;
						if constexpr (std::is_same_v<T, Image>)
							return output.Pixels.capacity();
						else if constexpr (std::is_same_v<T, ImageArray>)
							return RetainedImageArrayBytes(output);
						else {
							uint64_t bytes = detail::RetainedPayloadBytes(output.Data);
							return AddBytes(bytes, output.Port.capacity())
									   ? bytes
									   : std::numeric_limits<uint64_t>::max();
						}
					},
					result.Output
				);
		}();
		if (!AddBytes(retained, oldOutput))
			return fail(Status::LimitExceeded, "simulation old output size overflows");
		auto oldShadow = budget.Reserve(retained);
		if (!oldShadow) return fail(Status::LimitExceeded, "simulation replacement overlap exceeds bounds");
		const size_t slots = std::max(
			document.Nodes.size(),
			request.SimulationReplay ? request.SimulationReplay->Entries.size() : size_t{0}
		);
		uint64_t copyBytes = slots * sizeof(SimulationReplayEntry);
		if (request.SimulationReplay &&
			!AddBytes(copyBytes, RetainedSimulationReplayBytes(*request.SimulationReplay)))
			return fail(Status::LimitExceeded, "simulation copy size overflows");
		if constexpr (surfaceSupport) {
			if (!AddBytes(
					copyBytes,
					std::min(
						Limits::MaximumArrayElements,
						cone.SurfaceCaches +
							(request.SurfaceReplay ? request.SurfaceReplay->Entries.size() : 0)
					) * sizeof(SurfaceFrameReplayEntry)
				) ||
				(request.SurfaceReplay &&
				 !AddBytes(copyBytes, RetainedSurfaceFrameReplayBytes(*request.SurfaceReplay))))
				return fail(Status::LimitExceeded, "surface candidate owner size overflows");
		}
		if constexpr (surfaceSupport) {
			const size_t randomSlots = std::min(
				Limits::MaximumArrayElements,
				cone.RandomGenerators + (request.RandomReplay ? request.RandomReplay->Entries.size() : 0)
			);
			if (!AddBytes(copyBytes, randomSlots * sizeof(RandomReplayEntry)) ||
				(request.RandomReplay &&
				 !AddBytes(copyBytes, RetainedRandomReplayBytes(*request.RandomReplay))))
				return fail(Status::LimitExceeded, "random replay candidate size overflows");
		}
		if constexpr (surfaceSupport) {
			const size_t dataSlots = std::min(
				Limits::MaximumArrayElements,
				cone.DataProcessors + (request.DataReplay ? request.DataReplay->Entries.size() : 0)
			);
			if (!AddBytes(copyBytes, dataSlots * sizeof(DataReplayEntry)) ||
				(request.DataReplay && !AddBytes(copyBytes, RetainedDataReplayBytes(*request.DataReplay))))
				return fail(Status::LimitExceeded, "data replay candidate size overflows");
		}

		if constexpr (surfaceSupport) {
			if (request.RigidReplay && !AddBytes(copyBytes, RetainedRigidReplayBytes(*request.RigidReplay)))
				return fail(Status::LimitExceeded, "rigid replay candidate size overflows");
		}
		auto stateCharge = budget.Reserve(copyBytes);
		if (!stateCharge) return fail(Status::LimitExceeded, "simulation candidate state exceeds bounds");
		ReplayResult candidate;
		auto &candidateSimulation = [&]() -> SimulationReplayState & {
			if constexpr (surfaceSupport)
				return candidate.Simulation;
			else
				return candidate.Replay;
		}();
		candidateSimulation.Entries.reserve(slots);
		if (request.SimulationReplay)
			for (const auto &entry : request.SimulationReplay->Entries)
				if (std::any_of(document.Nodes.begin(), document.Nodes.end(), [&](const auto &node) {
						return node.Id == entry.NodeId;
					}))
					candidateSimulation.Entries.push_back(entry);
		SimulationCapture capture{&candidateSimulation, &*stateCharge};
		if constexpr (surfaceSupport) {
			const bool temporal = cone.SurfaceCaches != 0;
			candidate.Surfaces.Entries.reserve(
				std::min(
					Limits::MaximumArrayElements,
					cone.SurfaceCaches + (request.SurfaceReplay ? request.SurfaceReplay->Entries.size() : 0)
				)
			);
			if (request.SurfaceReplay)
				for (const auto &entry : request.SurfaceReplay->Entries)
					if (std::any_of(document.Nodes.begin(), document.Nodes.end(), [&](const auto &node) {
							return node.Id == entry.NodeId;
						}))
						candidate.Surfaces.Entries.push_back(entry);
			candidate.Surfaces.Tick = request.Tick;
			candidate.Surfaces.Initialized =
				temporal || (request.SurfaceReplay && request.SurfaceReplay->Initialized);
			capture.Surfaces = &candidate.Surfaces;
		}
		if constexpr (surfaceSupport) {
			candidate.Random.Entries.reserve(
				std::min(
					Limits::MaximumArrayElements,
					cone.RandomGenerators + (request.RandomReplay ? request.RandomReplay->Entries.size() : 0)
				)
			);
			if (request.RandomReplay)
				for (const auto &entry : request.RandomReplay->Entries)
					if (std::any_of(document.Nodes.begin(), document.Nodes.end(), [&](const auto &node) {
							return node.Id == entry.NodeId;
						}))
						candidate.Random.Entries.push_back(entry);
			capture.Random = &candidate.Random;
		}
		if constexpr (surfaceSupport) {
			if (request.DataReplay && !request.DataReplay->CacheGroups.Nodes.empty() &&
				ReconcileCacheGroupReplay(
					document,
					request.DataReplay->CacheGroups,
					candidate.Data.CacheGroups,
					request.SourceCacheProject,
					maximumBytes,
					diagnostic
				) != Status::Ok)
				return diagnostic.Code;
			// Load authored membership once; observation ticks preserve interactive ownership and activity.
			const bool authoredGroups = HasAuthoredCacheGroups(document);
			if (authoredGroups && candidate.Data.CacheGroups.Owners.empty()) {
				CacheGroupReplayState initialized;
				if (InitializeAuthoredCacheGroupReplay(
						document, candidate.Data.CacheGroups, initialized, budget.Available(), diagnostic
					) != Status::Ok)
					return diagnostic.Code;
				auto initializedCharge = budget.Reserve(RetainedCacheGroupReplayBytes(initialized));
				if (!initializedCharge || !stateCharge->Merge(std::move(*initializedCharge)))
					return fail(Status::LimitExceeded, "initialized cache-group journal exceeds live bytes");
				candidate.Data.CacheGroups = std::move(initialized);
			}
			for (auto &node : candidate.Data.CacheGroups.Nodes)
				for (auto &output : node.Outputs)
					if (output.Data) detail::StripSourcePathShiftIdentities(*output.Data);
			candidate.Data.Entries.reserve(
				std::min(
					Limits::MaximumArrayElements,
					cone.DataProcessors + (request.DataReplay ? request.DataReplay->Entries.size() : 0)
				)
			);
			if (request.DataReplay)
				for (const auto &entry : request.DataReplay->Entries)
					if (std::any_of(document.Nodes.begin(), document.Nodes.end(), [&](const auto &node) {
							return node.Id == entry.NodeId;
						})) {
						candidate.Data.Entries.push_back(entry);
						for (auto &frame : candidate.Data.Entries.back().Values)
							detail::StripSourcePathShiftIdentities(frame.Data);
					}
			capture.Data = &candidate.Data;
		}

		if constexpr (surfaceSupport) {
			if (request.RigidReplay) {
				candidate.Rigid.Owners.reserve(request.RigidReplay->Owners.capacity());
				for (const auto &owner : request.RigidReplay->Owners)
					candidate.Rigid.Owners.push_back(owner);
			}
			capture.Rigid = &candidate.Rigid;
		}
		NodeInputCapture inputs;
		if constexpr (inputSupport)
			inputs = {
				inputNodeId,
				&inputStorage->Values,
				&inputStorage->Images,
				&inputStorage->SurfacePolicy,
				&outputCharge,
				&inputStorage->ImageArrays,
				&inputStorage->InterpolationPolicy
			};
		StatefulOutputCapture batch{batchIds, nullptr, &outputCharge};
		if constexpr (batchSupport || inputSupport) batch.Outputs = &candidate.Outputs;
		detail::AllocationReservation groupOutputsCharge;
		if (groupProcess) {
			auto held = budget.Reserve(RetainedCacheGroupReplayBytes(*groupProcess->Outputs));
			if (!held) return fail(Status::LimitExceeded, "group output history exceeds live bytes");
			groupOutputsCharge = std::move(*held);
			groupProcess->Charge = &groupOutputsCharge;
		}
		const Status evaluated = EvaluateGraph(
			document,
			plan,
			outputId,
			request,
			evaluation.Data,
			diagnostic,
			budget,
			outputCharge,
			nullptr,
			nullptr,
			inputSupport ? &inputs : nullptr,
			nullptr,
			inputNodeId,
			{},
			false,
			nullptr,
			&capture,
			!groupProcess && (batchSupport || inputSupport) ? &batch : nullptr,
			groupProcess
		);
		if (evaluated != Status::Ok) return evaluated;
		if constexpr (!batchSupport && !inputSupport) {
			const auto output =
				std::find_if(document.Outputs.begin(), document.Outputs.end(), [&](const auto &entry) {
					return entry.Id == outputId;
				});
			if (auto *image = std::get_if<Image>(&evaluation.Data))
				candidate.Output = std::move(*image);
			else if (auto *array = std::get_if<ImageArray>(&evaluation.Data)) {
				if constexpr (surfaceSupport)
					candidate.Output = std::move(*array);
				else {
					if (array->Items.size() != 1 ||
						!std::holds_alternative<size_t>(array->Items.front().Data))
						return fail(
							Status::InvalidOutput, "simulation single image requires one top-level image"
						);
					candidate.Output = std::move(array->Images[std::get<size_t>(array->Items.front().Data)]);
				}
			} else {
				auto *values = FindValueOutputs(evaluation.Data);
				if (!values || output == document.Outputs.end())
					return fail(Status::InvalidOutput, "simulation output is not an image or typed value");
				const auto selected = std::find_if(values->begin(), values->end(), [&](const auto &entry) {
					return entry.Port == output->Port;
				});
				if (selected == values->end())
					return fail(Status::InvalidOutput, "simulation selected port was not produced");
				const auto node =
					std::find_if(document.Nodes.begin(), document.Nodes.end(), [&](const auto &entry) {
						return entry.Id == output->NodeId;
					});
				const auto domain = node == document.Nodes.end()
										? std::optional<SourceSocketDomain>{}
										: FindOutputDomain(*node, evaluation.Data, selected->Port);
				candidate.Output =
					EvaluatedValue{std::move(selected->Port), std::move(selected->Data), domain};
			}
		}
		// Source mesh outputs share their constructor identity, including mutations performed later in
		// the union closure. Publish every selected view against the final candidate owner.
		const auto resolveAlias = [&](Value &value) {
			const DataReplayState *strandReplay = nullptr;
			if constexpr (surfaceSupport) strandReplay = &candidate.Data;
			const auto status = detail::ResolveSimulationValueAliases(
				value, candidateSimulation, budget, outputCharge, diagnostic, strandReplay
			);
			if (status == Status::Ok) detail::StripSourcePathShiftIdentities(value);
			return status;
		};
		if constexpr (batchSupport || inputSupport) {
			for (auto &named : candidate.Outputs)
				if (auto *value = std::get_if<EvaluatedValue>(&named.Output))
					if (resolveAlias(value->Data) != Status::Ok) return diagnostic.Code;
		} else if (auto *value = std::get_if<EvaluatedValue>(&candidate.Output)) {
			if (resolveAlias(value->Data) != Status::Ok) return diagnostic.Code;
		}
		if constexpr (inputSupport)
			for (auto &value : inputStorage->Values)
				if (resolveAlias(value.Data) != Status::Ok) return diagnostic.Code;
		if constexpr (surfaceSupport) {
			for (auto &node : candidate.Data.CacheGroups.Nodes)
				for (auto &output : node.Outputs)
					if (output.Data) {
						if (detail::ResolveSimulationValueAliases(
								*output.Data,
								candidateSimulation,
								budget,
								*stateCharge,
								diagnostic,
								&candidate.Data
							) != Status::Ok)
							return diagnostic.Code;
						detail::StripSourcePathShiftIdentities(*output.Data);
					}
			size_t workspaceRows = candidate.Data.Entries.size();
			for (const auto &node : candidate.Data.CacheGroups.Nodes)
				workspaceRows = std::max(workspaceRows, node.Outputs.size());
			auto workspace = budget.Reserve(workspaceRows * sizeof(size_t));
			if (!workspace)
				return fail(Status::LimitExceeded, "data replay validation workspace exceeds live budget");
			if (ValidateDataReplay(candidate.Data, maximumBytes, diagnostic) != Status::Ok)
				return diagnostic.Code;
		}
		if constexpr (surfaceSupport)
			for (auto &entry : candidate.Data.Entries)
				for (auto &frame : entry.Values)
					detail::StripSourcePathShiftIdentities(frame.Data);
		if constexpr (inputSupport) {
			inputStorage->ReplayCharge = std::move(*stateCharge);
			StatefulSnapshotAccess::Install(candidate.Inputs, std::move(inputStorage));
		}
		result = std::move(candidate);
		oldShadow->Reset();
		diagnostic = {};
		return Status::Ok;
	} catch (const std::bad_alloc &) {
		SetDiagnostic(diagnostic, Status::LimitExceeded, "simulation evaluation allocation was refused");
		return diagnostic.Code;
	}

	Status EvaluateSimulation(
		const Document &document,
		const Plan &plan,
		const std::string &outputId,
		const EvaluationRequest &request,
		SimulationEvaluationResult &result,
		Diagnostic &diagnostic,
		uint64_t maximumBytes
	) {
		return EvaluateReplayInternal(document, plan, outputId, request, result, diagnostic, maximumBytes);
	}
	static bool
	GroupHeldPortCompatible(const Document &document, const Node &node, const CacheGroupReplayOutput &held) {
		const auto declared = FindPortType(node, held.Port, PortDirection::Output, &document);
		if (!declared) return false;
		if (!held.Data) return !held.Domain || held.Domain->Type == *declared || *declared == ValueType::Any;
		ValueType actual =
			held.Domain && held.Domain->Type != ValueType::Any ? held.Domain->Type : TypeOf(*held.Data);
		std::span<const ValueType> alternatives{};
		if (const auto *schema = FindPort(node.Type, held.Port, PortDirection::Output))
			alternatives = schema->Alternatives;
		if (node.Type == "value.noise_field" || node.Type == "value.sample_noise" ||
			IsNoiseImageGenerator(node.Type)) {
			const auto port = NoiseNodePort(node, held.Port, PortDirection::Output, &document);
			if (port) alternatives = port->Alternatives;
		}
		if (!alternatives.empty())
			return std::find(alternatives.begin(), alternatives.end(), actual) != alternatives.end();
		return *declared == ValueType::Any || *declared == actual;
	}

	bool GroupRenderSession::Ready(std::string_view nodeId) const {
		const auto found =
			std::find_if(Nodes.begin(), Nodes.end(), [&](const auto &node) { return node.NodeId == nodeId; });
		return found != Nodes.end() && found->Rendered;
	}

	uint64_t RetainedGroupRenderSessionBytes(const GroupRenderSession &session) {
		uint64_t bytes = sizeof(session) + session.Nodes.capacity() * sizeof(GroupRenderReadiness);
		const uint64_t commonBytes = detail::RetainedSourceCommonMembershipBytes(session);
		const uint64_t embedded = sizeof(session.Common) + sizeof(session.CommonAnimators) +
								  sizeof(session.SourceCommonWrites) + sizeof(session.SourceCommonBindings) +
								  sizeof(session.SourceCommonInputs);
		if (commonBytes < embedded || !AddBytes(bytes, commonBytes - embedded)) return UINT64_MAX;
		if (!AddBytes(bytes, session.Purities.capacity() * sizeof(GroupRenderPurity))) return UINT64_MAX;
		if (!AddBytes(bytes, RetainedCacheGroupReplayBytes(session.Outputs)) ||
			!AddBytes(bytes, RetainedStatefulOutputBytes(session.Replay)) ||
			!AddBytes(bytes, RetainedSimulationReplayBytes(session.Replay.Simulation)) ||
			!AddBytes(bytes, RetainedSurfaceFrameReplayBytes(session.Replay.Surfaces)) ||
			!AddBytes(bytes, RetainedRandomReplayBytes(session.Replay.Random)) ||
			!AddBytes(bytes, RetainedDataReplayBytes(session.Replay.Data)) ||
			!AddBytes(bytes, RetainedRigidReplayBytes(session.Replay.Rigid)))
			return UINT64_MAX;
		for (const auto &node : session.Nodes)
			if (!AddBytes(bytes, node.NodeId.capacity()) || !AddBytes(bytes, node.GroupId.capacity()) ||
				!AddBytes(bytes, node.InstanceBase.capacity()) ||
				!AddBytes(bytes, node.SourceParentInputBase.capacity()))
				return UINT64_MAX;
		for (const auto &group : session.Purities)
			if (!AddBytes(bytes, group.GroupId.capacity()) || !AddBytes(bytes, group.ParentId.capacity()) ||
				!AddBytes(bytes, group.OwnerNodeId.capacity()) ||
				!AddBytes(bytes, group.InstanceBase.capacity()))
				return UINT64_MAX;
		return bytes;
	}

	static bool GroupScopeDisabled(const Document &document, std::string_view scope) {
		for (size_t hop = 0; !scope.empty() && hop < document.Groups.size(); ++hop) {
			const auto group =
				std::find_if(document.Groups.begin(), document.Groups.end(), [&](const Group &candidate) {
					return candidate.Id == scope;
				});
			if (group == document.Groups.end()) return true;
			if (!group->RenderActive) return true;
			scope = group->ParentId;
		}
		return false;
	}

#include "SourceFrameActivitySeeds.inc"
	static std::optional<bool> SourceNodeAnimated(
		const Document &document,
		const Node &local,
		const GroupReplayState *replay,
		std::span<const GroupRenderReadiness> observations
	) {
		const Node *node = &local;
		bool unresolvedActivity = false;
		for (size_t hop = 0; hop <= document.Nodes.size(); ++hop) {
			const auto observation =
				std::find_if(observations.begin(), observations.end(), [&](const auto &row) {
					return row.NodeId == node->Id;
				});
			if (observation == observations.end() ||
				observation->FrameActivity == SourceFrameActivity::Unknown)
				unresolvedActivity = true;
			else if (observation->FrameActivity == SourceFrameActivity::FrameDriven)
				return true;
			if (node->InstanceBase.empty()) break;
			const auto base =
				std::find_if(document.Nodes.begin(), document.Nodes.end(), [&](const Node &candidate) {
					return candidate.Id == node->InstanceBase;
				});
			if (base == document.Nodes.end() || hop == document.Nodes.size()) return std::nullopt;
			node = &*base;
		}
		const auto animated = [&](std::string_view port, bool legacyKeyed = false) {
			// The parent socket belongs to Collection.inputs, not the Group Input control's inputs.
			if (node->Type == "pc.group_input" && port == "parent_value") return false;
			const auto mode = detail::SourcePropertyGetterAnimated(document, *node, port, replay);
			return mode.value_or(legacyKeyed);
		};
		for (const auto &port : node->SourceAnimatedInputs)
			if (animated(port)) return true;
		for (const auto &port : node->SourceStaticInputs)
			if (animated(port)) return true;
		if (const auto *catalogue = FindCatalogueEntry(node->Type))
			for (const auto &port : catalogue->Inputs)
				if (animated(port.Id)) return true;
		for (const auto &port : node->DynamicInputs)
			if (animated(port.Id)) return true;
		if (replay && replay->InstancesBound())
			for (const auto &binding : replay->Bindings())
				if (binding.NodeId == node->Id && animated(binding.Port)) return true;
		// Retained source keys do not change is_anim. Legacy native graphs have no source mode.
		for (const auto &key : document.Keyframes)
			if (key.NodeId == node->Id && animated(key.Port, true)) return true;
		for (const auto &track : document.Tracks)
			if (track.NodeId == node->Id && animated(track.Port, true)) return true;
		return unresolvedActivity ? std::nullopt : std::optional<bool>{false};
	}

	static std::optional<bool> PureSourceGroup(
		const Document &document,
		std::string_view groupId,
		const GroupReplayState *replay,
		std::span<const GroupRenderReadiness> observations
	) {
		for (const auto &group : document.Groups) {
			if (group.Id == groupId && !group.PureFunction) return false;
			if (group.ParentId == groupId) return false;
		}
		bool unresolved = false;
		for (const auto &node : document.Nodes) {
			if (node.GroupId != groupId) continue;
			const auto animated = SourceNodeAnimated(document, node, replay, observations);
			if (!animated)
				unresolved = true;
			else if (*animated)
				return false;
			if (std::any_of(document.Groups.begin(), document.Groups.end(), [&](const auto &group) {
					return group.OwnerNodeId == node.Id;
				}))
				return false;
		}
		return unresolved ? std::nullopt : std::optional<bool>{true};
	}

#include "SourceCommonAdmission.inc"
#include "SourceCommonInvocation.inc"

	Status ProcessGroupRender(
		const Document &document,
		const Plan &plan,
		const EvaluationRequest &request,
		const GroupRenderOperation &operation,
		GroupRenderSession &session,
		Diagnostic &diagnostic,
		uint64_t maximumBytes
	) try {
		ENGINE_PROFILE("imagegraph.group_process");
		const auto fail = [&](Status code, const char *message) {
			SetDiagnostic(diagnostic, code, message);
			return code;
		};
		if (!maximumBytes || maximumBytes > Limits::MaximumEvaluationBytes ||
			document.Nodes.size() > Limits::MaximumNodes || session.Nodes.size() > Limits::MaximumNodes ||
			operation.AffectedNodes.size() > Limits::MaximumNodes ||
			session.Purities.size() > Limits::MaximumGroups ||
			(operation.PurityRefresh && operation.PurityRefresh->Groups.size() > Limits::MaximumGroups))
			return fail(Status::LimitExceeded, "group process records or byte cap exceed bounds");
		if (ValidateEvaluationRequest(request, diagnostic) != Status::Ok) return diagnostic.Code;
		const uint64_t comparisons =
			document.Nodes.size() *
			uint64_t(
				document.Nodes.size() + document.Links.size() + document.Groups.size() +
				session.Nodes.size() + session.Outputs.Nodes.size() + session.Purities.size() +
				document.Groups.size()
			);
		const uint64_t purityWork =
			operation.Mode == GroupRenderMode::RefreshPurity
				? document.Nodes.size() *
					  uint64_t(
						  document.Keyframes.size() + document.Tracks.size() +
						  (request.GroupReplay ? request.GroupReplay->Bindings().size() : 0)
					  )
				: 0;
		if (comparisons > 16'000'000 || purityWork > 16'000'000 - comparisons)
			return fail(Status::LimitExceeded, "group processing exceeds its comparison work cap");
		if (operation.Mode != GroupRenderMode::AutomaticFull &&
			operation.Mode != GroupRenderMode::AutomaticPartial &&
			operation.Mode != GroupRenderMode::ForceGroup && operation.Mode != GroupRenderMode::RefreshPurity)
			return fail(Status::InvalidValue, "group processing mode is invalid");
		if (operation.InitialState != SourceNodeInitialState::Loaded &&
			operation.InitialState != SourceNodeInitialState::Constructed)
			return fail(Status::InvalidValue, "source initialization policy is invalid");
		if ((operation.Mode == GroupRenderMode::RefreshPurity) != operation.PurityRefresh.has_value())
			return fail(Status::InvalidValue, "purity refresh requires its explicit lifecycle operation");
		if (operation.PurityRefresh) {
			const auto &refresh = *operation.PurityRefresh;
			if (refresh.Event != SourcePurityRefreshEvent::LoadTopology &&
				refresh.Event != SourcePurityRefreshEvent::Membership &&
				refresh.Event != SourcePurityRefreshEvent::AnimationMode &&
				refresh.Event != SourcePurityRefreshEvent::PureFunction &&
				refresh.Event != SourcePurityRefreshEvent::InputOutput)
				return fail(Status::InvalidValue, "source purity refresh event is invalid");
			for (size_t index = 0; index < refresh.Groups.size(); ++index) {
				const auto id = refresh.Groups[index];
				if (id.size() > Limits::MaximumTextBytes ||
					std::find(refresh.Groups.begin(), refresh.Groups.begin() + index, id) !=
						refresh.Groups.begin() + index ||
					std::none_of(document.Groups.begin(), document.Groups.end(), [&](const auto &group) {
						return group.Id == id;
					}))
					return fail(Status::InvalidGroup, "source purity refresh group is absent or duplicated");
			}
		}
		if (operation.Mode == GroupRenderMode::ForceGroup) {
			if (std::none_of(document.Groups.begin(), document.Groups.end(), [&](const Group &group) {
					return group.Id == operation.GroupId;
				}))
				return fail(Status::InvalidGroup, "forced group does not exist");
		} else if (!operation.GroupId.empty())
			return fail(Status::InvalidGroup, "automatic process has no selected group");
		if (operation.Mode != GroupRenderMode::AutomaticPartial && !operation.AffectedNodes.empty())
			return fail(Status::InvalidValue, "only partial processing accepts explicit seed nodes");
		for (const auto id : operation.AffectedNodes)
			if (id.size() > Limits::MaximumTextBytes ||
				std::none_of(document.Nodes.begin(), document.Nodes.end(), [&](const Node &node) {
					return node.Id == id;
				}))
				return fail(Status::UnknownNode, "partial processing seed node is absent");
		detail::EvaluationBudget budget(maximumBytes);
		auto previous = budget.Reserve(RetainedGroupRenderSessionBytes(session));
		const auto documentBytes = DocumentRetainedPayloadBytes(document);
		auto authored = documentBytes ? budget.Reserve(*documentBytes) : std::nullopt;
		if (!previous || !authored)
			return fail(Status::LimitExceeded, "group process prior state exceeds live bytes");
		detail::AllocationReservation sourceReplayCharge;
		if (operation.Mode == GroupRenderMode::RefreshPurity && request.GroupReplay) {
			if (request.GroupReplay->AuthoringRevision() != request.GroupAuthoringRevision ||
				(!request.GroupReplay->InstancesBound() &&
				 std::any_of(document.Nodes.begin(), document.Nodes.end(), [](const auto &node) {
					 return !node.InstanceBase.empty() || !node.SourceParentInputBase.empty();
				 })))
				return fail(Status::InvalidValue, "source purity refresh animator owner is stale or unbound");
			auto charge = budget.Reserve(request.GroupReplay->RetainedBytes());
			if (!charge)
				return fail(Status::LimitExceeded, "source purity refresh animator owner exceeds live bytes");
			sourceReplayCharge = std::move(*charge);
		}
		detail::AllocationReservation planCharge;
		Plan checked;
		if (CompileWithBudget(
				document, checked, diagnostic, budget, planCharge, plan.SourceCommonRuntimeOnly
			) != Status::Ok)
			return diagnostic.Code;
		if (checked != plan) return fail(Status::InvalidOutput, "group process plan does not match document");
		if (ValidateCacheGroupReplay(session.Outputs, maximumBytes, diagnostic) != Status::Ok)
			return diagnostic.Code;
		for (size_t index = 0; index < session.Nodes.size(); ++index) {
			const auto &id = session.Nodes[index].NodeId;
			if (id.empty() || id.size() > Limits::MaximumTextBytes ||
				std::any_of(session.Nodes.begin(), session.Nodes.begin() + index, [&](const auto &node) {
					return node.NodeId == id;
				}))
				return fail(Status::InvalidValue, "group readiness identity is invalid or duplicated");
			if (session.Nodes[index].GroupId.size() > Limits::MaximumTextBytes ||
				session.Nodes[index].InstanceBase.size() > Limits::MaximumTextBytes ||
				session.Nodes[index].SourceParentInputBase.size() > Limits::MaximumTextBytes ||
				(session.Nodes[index].FrameActivity != SourceFrameActivity::Unknown &&
				 session.Nodes[index].FrameActivity != SourceFrameActivity::Static &&
				 session.Nodes[index].FrameActivity != SourceFrameActivity::FrameDriven))
				return fail(Status::InvalidValue, "source activity record is invalid");
		}
		for (size_t index = 0; index < session.Purities.size(); ++index) {
			const auto &row = session.Purities[index];
			if (row.GroupId.empty() || row.GroupId.size() > Limits::MaximumTextBytes ||
				row.ParentId.size() > Limits::MaximumTextBytes ||
				row.OwnerNodeId.size() > Limits::MaximumTextBytes ||
				row.InstanceBase.size() > Limits::MaximumTextBytes ||
				std::any_of(
					session.Purities.begin(),
					session.Purities.begin() + index,
					[&](const auto &prior) { return prior.GroupId == row.GroupId; }
				) ||
				(row.State != SourceGroupPurity::Unknown && row.State != SourceGroupPurity::Nonpure &&
				 row.State != SourceGroupPurity::Pure))
				return fail(Status::InvalidValue, "cached source purity record is invalid");
		}
		auto survivorCharge = budget.Reserve(detail::CacheGroupReplayCloneBytes(session.Outputs));
		if (!survivorCharge)
			return fail(Status::LimitExceeded, "group process retained output clone exceeds live bytes");
		CacheGroupReplayState surviving;
		surviving.Nodes.reserve(session.Outputs.Nodes.size());
		for (const auto &held : session.Outputs.Nodes)
			if (std::any_of(document.Nodes.begin(), document.Nodes.end(), [&](const Node &node) {
					return node.Id == held.NodeId && node.Type == held.NodeType &&
						   std::all_of(held.Outputs.begin(), held.Outputs.end(), [&](const auto &port) {
							   return GroupHeldPortCompatible(document, node, port);
						   });
				}))
				surviving.Nodes.push_back(held);
		GroupRenderSession candidate;
		auto commonHistoryCharge = budget.Reserve(detail::RetainedSourceCommonMembershipBytes(session));
		if (!commonHistoryCharge)
			return fail(Status::LimitExceeded, "common process history clone exceeds live bytes");
		if (!session.SourceCommonBindings.empty()) {
			if (session.SourceCommonBindings.size() != document.SourceCommonOwners.size() ||
				session.Common.Owners.size() != document.SourceCommonOwners.size())
				return fail(
					Status::InvalidValue,
					"common process history requires explicit initialization or reconciliation"
				);
			for (size_t index = 0; index < document.SourceCommonOwners.size(); ++index) {
				const auto &owner = document.SourceCommonOwners[index];
				const auto &binding = session.SourceCommonBindings[index];
				if (binding.SourceOwnerId != owner.SourceOwnerId || binding.SourceType != owner.SourceType ||
					binding.NativeOwnerKind != owner.NativeOwnerKind ||
					binding.NativeOwnerId != owner.NativeOwnerId)
					return fail(
						Status::InvalidValue, "common process owner bindings require explicit reconciliation"
					);
			}
			candidate.Common = session.Common;
			candidate.CommonAnimators = session.CommonAnimators;
			candidate.SourceCommonWrites = session.SourceCommonWrites;
			candidate.SourceCommonBindings = session.SourceCommonBindings;
			candidate.SourceCommonInputs = session.SourceCommonInputs;
		}
		if (detail::InitializeGroupRenderOutputs(
				document, surviving, candidate.Outputs, budget.Available(), diagnostic
			) != Status::Ok)
			return diagnostic.Code;
		std::array<bool, Limits::MaximumNodes> retainedSchema{};
		for (size_t index = 0; index < document.Nodes.size(); ++index)
			retainedSchema[index] =
				std::any_of(surviving.Nodes.begin(), surviving.Nodes.end(), [&](const auto &row) {
					return row.NodeId == document.Nodes[index].Id;
				});
		surviving = {};
		survivorCharge->Reset();
		auto snapshotCharge = budget.Reserve(RetainedCacheGroupReplayBytes(candidate.Outputs));
		uint64_t metadataBytes = document.Nodes.size() *
								 (sizeof(GroupRenderReadiness) + sizeof(uint8_t) +
								  sizeof(CacheGroupReplayNode) + sizeof(std::pair<std::string_view, size_t>));
		for (const auto &node : document.Nodes)
			if (!AddBytes(metadataBytes, std::max(node.Id.size(), std::string{}.capacity())))
				return fail(Status::LimitExceeded, "group readiness size overflows");
		for (const auto &node : document.Nodes)
			if (!AddBytes(metadataBytes, std::max(node.GroupId.size(), std::string{}.capacity())) ||
				!AddBytes(metadataBytes, std::max(node.InstanceBase.size(), std::string{}.capacity())) ||
				!AddBytes(
					metadataBytes, std::max(node.SourceParentInputBase.size(), std::string{}.capacity())
				))
				return fail(Status::LimitExceeded, "source activity membership size overflows");
		if (!AddBytes(metadataBytes, document.Groups.size() * sizeof(GroupRenderPurity)))
			return fail(Status::LimitExceeded, "cached source purity size overflows");
		for (const auto &group : document.Groups)
			if (!AddBytes(metadataBytes, std::max(group.Id.size(), std::string{}.capacity())) ||
				!AddBytes(metadataBytes, std::max(group.ParentId.size(), std::string{}.capacity())) ||
				!AddBytes(metadataBytes, std::max(group.OwnerNodeId.size(), std::string{}.capacity())) ||
				!AddBytes(metadataBytes, std::max(group.InstanceBase.size(), std::string{}.capacity())))
				return fail(Status::LimitExceeded, "cached source purity identity size overflows");
		auto metadataCharge = budget.Reserve(metadataBytes);
		if (!snapshotCharge || !metadataCharge)
			return fail(Status::LimitExceeded, "group socket and readiness metadata exceed live bytes");
		// Align the journal once with the checked document; evaluator capture then has constant indexing.
		CacheGroupReplayState ordered;
		ordered.Nodes.reserve(document.Nodes.size());
		candidate.Nodes.reserve(document.Nodes.size());
		for (const auto &node : document.Nodes) {
			const auto held = std::find_if(
				candidate.Outputs.Nodes.begin(), candidate.Outputs.Nodes.end(), [&](const auto &record) {
					return record.NodeId == node.Id;
				}
			);
			const bool retained = std::any_of(
				session.Outputs.Nodes.begin(), session.Outputs.Nodes.end(), [&](const auto &record) {
					return record.NodeId == node.Id && record.NodeType == node.Type &&
						   std::all_of(record.Outputs.begin(), record.Outputs.end(), [&](const auto &port) {
							   return GroupHeldPortCompatible(document, node, port);
						   });
				}
			);
			for (auto &port : held->Outputs) {
				if (port.Domain) continue;
				if (const auto type = FindPortType(node, port.Port, PortDirection::Output, &document))
					port.Domain = SourceSocketDomain{*type, std::nullopt, std::nullopt};
			}
			ordered.Nodes.push_back(std::move(*held));
			const auto prior = std::find_if(session.Nodes.begin(), session.Nodes.end(), [&](const auto &row) {
				return row.NodeId == node.Id;
			});
			const auto activity = retained && prior != session.Nodes.end()
									  ? prior->FrameActivity
									  : SourceInitialFrameActivity(node.Type, operation.InitialState);
			candidate.Nodes.push_back(
				{node.Id,
				 operation.Mode != GroupRenderMode::AutomaticFull && retained && session.Ready(node.Id),
				 activity,
				 node.GroupId,
				 node.InstanceBase,
				 node.SourceParentInputBase}
			);
		}
		candidate.Outputs = std::move(ordered);
		std::array<bool, Limits::MaximumGroups> structureChanged{};
		const auto changed = [&](std::string_view id) {
			for (size_t index = 0; index < document.Groups.size(); ++index)
				if (document.Groups[index].Id == id) structureChanged[index] = true;
		};
		const EvaluationNodeIndices structureIndices(document);
		std::array<bool, Limits::MaximumNodes> sameNodeContract{};
		for (size_t index = 0; index < document.Nodes.size(); ++index) {
			const auto &node = document.Nodes[index];
			const auto old = std::find_if(session.Nodes.begin(), session.Nodes.end(), [&](const auto &row) {
				return row.NodeId == node.Id;
			});
			sameNodeContract[index] = retainedSchema[index] && old != session.Nodes.end() &&
									  old->GroupId == node.GroupId &&
									  old->InstanceBase == node.InstanceBase &&
									  old->SourceParentInputBase == node.SourceParentInputBase;
			if (!sameNodeContract[index]) {
				changed(node.GroupId);
				if (old != session.Nodes.end()) changed(old->GroupId);
			}
		}
		// Invalidate the owning classification when a referenced base contract is replaced as well.
		for (const auto &node : document.Nodes) {
			const Node *base = &node;
			for (size_t hop = 0; hop < document.Nodes.size(); ++hop) {
				if (!sameNodeContract[structureIndices.at(base->Id)]) {
					changed(node.GroupId);
					break;
				}
				const auto alias = !base->InstanceBase.empty()
									   ? std::string_view(base->InstanceBase)
									   : std::string_view(base->SourceParentInputBase);
				if (alias.empty()) break;
				const auto next = structureIndices.find(alias);
				if (!next) {
					changed(node.GroupId);
					break;
				}
				base = &document.Nodes[*next];
			}
		}
		for (const auto &old : session.Nodes)
			if (!structureIndices.find(old.NodeId)) changed(old.GroupId);
		for (const auto &group : document.Groups) {
			const auto old =
				std::find_if(session.Purities.begin(), session.Purities.end(), [&](const auto &row) {
					return row.GroupId == group.Id;
				});
			if (old == session.Purities.end() || old->ParentId != group.ParentId) {
				changed(group.ParentId);
				if (old != session.Purities.end()) changed(old->ParentId);
			}
			if (!group.OwnerNodeId.empty()) {
				const auto owner = structureIndices.find(group.OwnerNodeId);
				if (!owner || !sameNodeContract[*owner]) changed(group.Id);
			}
		}
		for (const auto &old : session.Purities)
			if (std::none_of(document.Groups.begin(), document.Groups.end(), [&](const auto &group) {
					return group.Id == old.GroupId;
				}))
				changed(old.ParentId);
		candidate.Purities.reserve(document.Groups.size());
		for (size_t index = 0; index < document.Groups.size(); ++index) {
			const auto &group = document.Groups[index];
			const auto prior =
				std::find_if(session.Purities.begin(), session.Purities.end(), [&](const auto &row) {
					return row.GroupId == group.Id;
				});
			const bool sameStructure = prior != session.Purities.end() && !structureChanged[index] &&
									   prior->ParentId == group.ParentId &&
									   prior->OwnerNodeId == group.OwnerNodeId &&
									   prior->InstanceBase == group.InstanceBase &&
									   prior->AuthoredPureFunction == group.PureFunction;
			const auto state = prior == session.Purities.end()
								   ? SourceGroupPurity::Nonpure
								   : (sameStructure ? prior->State : SourceGroupPurity::Unknown);
			candidate.Purities.push_back(
				{group.Id, group.ParentId, group.OwnerNodeId, group.InstanceBase, group.PureFunction, state}
			);
		}
		if (operation.PurityRefresh) {
			for (auto &row : candidate.Purities) {
				const auto targets = operation.PurityRefresh->Groups;
				if (!targets.empty() &&
					std::find(targets.begin(), targets.end(), row.GroupId) == targets.end())
					continue;
				const auto pure =
					PureSourceGroup(document, row.GroupId, request.GroupReplay, candidate.Nodes);
				row.State = !pure ? SourceGroupPurity::Unknown
								  : (*pure ? SourceGroupPurity::Pure : SourceGroupPurity::Nonpure);
			}
		}
		if (operation.Mode == GroupRenderMode::RefreshPurity) {
			if (ValidateSimulationReplay(session.Replay.Simulation, maximumBytes, diagnostic) != Status::Ok ||
				ValidateSurfaceFrameReplay(session.Replay.Surfaces, maximumBytes, diagnostic) != Status::Ok ||
				ValidateRandomReplay(session.Replay.Random, maximumBytes, diagnostic) != Status::Ok ||
				ValidateDataReplay(session.Replay.Data, maximumBytes, diagnostic) != Status::Ok ||
				ValidateRigidReplay(session.Replay.Rigid, maximumBytes, diagnostic) != Status::Ok)
				return diagnostic.Code;
			// The complete session-size gate includes every replay ledger before its bounded copy.
			auto replayCharge = budget.Reserve(RetainedGroupRenderSessionBytes(session));
			if (!replayCharge)
				return fail(Status::LimitExceeded, "purity refresh replay clone exceeds live bytes");
			candidate.Replay = session.Replay;
			if (RetainedGroupRenderSessionBytes(candidate) >
				maximumBytes - previous->Bytes() - authored->Bytes() - sourceReplayCharge.Bytes())
				return fail(Status::LimitExceeded, "purity refresh publication exceeds live bytes");
			core::Metrics::Count(
				"imagegraph.group_process.purity_refreshes",
				operation.PurityRefresh->Groups.empty() ? document.Groups.size()
														: operation.PurityRefresh->Groups.size()
			);
			core::Metrics::SetGauge(
				"imagegraph.group_process.retained_bytes", double(RetainedGroupRenderSessionBytes(candidate))
			);
			session = std::move(candidate);
			diagnostic = {};
			return Status::Ok;
		}
		if (document.Nodes.empty()) {
			session = std::move(candidate);
			diagnostic = {};
			return Status::Ok;
		}
		EvaluationRequest tunnelRequest = request;
		tunnelRequest.SourceTunnelPreviousOutputs = &session.Outputs;
		if (!tunnelRequest.DataReplay) tunnelRequest.DataReplay = &session.Replay.Data;
		if (ResolveSourceTunnelRegistry(
				document, tunnelRequest, checked, budget, planCharge, diagnostic, true
			) != Status::Ok)
			return diagnostic.Code;
		const Plan &processPlan = checked;
		std::vector<uint8_t> run(document.Nodes.size(), 0);
		const EvaluationNodeIndices indices(document);
		const bool forced = operation.Mode == GroupRenderMode::ForceGroup;
		const auto cached =
			std::find_if(candidate.Purities.begin(), candidate.Purities.end(), [&](const auto &row) {
				return row.GroupId == operation.GroupId;
			});
		const auto purity =
			!forced ? std::optional<bool>{false}
					: (cached == candidate.Purities.end() || cached->State == SourceGroupPurity::Unknown
						   ? std::nullopt
						   : std::optional<bool>{cached->State == SourceGroupPurity::Pure});
		if (!purity)
			return fail(
				Status::UnsupportedExecution, "forced group requires an observed source purity refresh"
			);
		const bool pure = *purity;
		for (const size_t index : processPlan.NodeOrder) {
			const auto &node = document.Nodes[index];
			if (!candidate.SourceCommonBindings.empty()) {
				if (const auto *owner = detail::CommonOwner(document, node.Id)) {
					const auto dispatch = detail::SourceCommonOwnerDispatch(
						document, request, size_t(owner - document.SourceCommonOwners.data())
					);
					if (dispatch.Wrapper != SourceCommonWrapperKind::Unsupported) {
						if (!request.SourceSafeMode)
							return fail(
								Status::UnsupportedExecution, "source wrapper needs an observed safe mode"
							);
						if (*request.SourceSafeMode) {
							const auto prior =
								std::find_if(session.Nodes.begin(), session.Nodes.end(), [&](const auto &n) {
									return n.NodeId == node.Id;
								});
							if (prior != session.Nodes.end())
								candidate.Nodes[index].Rendered = prior->Rendered;
							continue;
						}
					}
				}
			}
			if (forced) {
				if (pure && node.GroupId == operation.GroupId) run[index] = 1;
				continue;
			}
			if (GroupScopeDisabled(document, node.GroupId)) continue;
			bool ready = true;
			bool affected =
				!candidate.Nodes[index].Rendered ||
				candidate.Nodes[index].FrameActivity == SourceFrameActivity::FrameDriven ||
				!node.InstanceBase.empty() ||
				std::find(operation.AffectedNodes.begin(), operation.AffectedNodes.end(), node.Id) !=
					operation.AffectedNodes.end();
			for (const auto &link : processPlan.EffectiveLinks) {
				if (link.ToNode != node.Id ||
					FindPortType(node, link.ToPort, PortDirection::Input) == ValueType::NodeRef)
					continue;
				const size_t source = indices.at(link.FromNode);
				affected = affected || run[source];
				bool inactive = request.DataReplay &&
								!CacheGroupReplayShouldRun(request.DataReplay->CacheGroups, link.FromNode);
				if (!run[source] && !candidate.Nodes[source].Rendered && !inactive) {
					ready = false;
					break;
				}
			}

			for (const auto &route : processPlan.PcxNamedDependencies) {
				if (route.Consumer != index || route.Name != detail::SourceTunnelRouteName) continue;
				affected = affected || run[route.Producer];
				// Tunnel receivers read the sender getter even when its callback cannot run.
			}
			if (ready && (operation.Mode != GroupRenderMode::AutomaticPartial || affected)) run[index] = 1;
		}
		EvaluationRequest processing = request;
		processing.GroupRender = nullptr;
		processing.SourceTunnelPreviousOutputs = &session.Outputs;
		processing.ForceGroupRender = true;
		if (!processing.SimulationReplay) processing.SimulationReplay = &session.Replay.Simulation;
		if (!processing.SurfaceReplay) processing.SurfaceReplay = &session.Replay.Surfaces;
		if (!processing.RandomReplay) processing.RandomReplay = &session.Replay.Random;
		if (!processing.DataReplay) processing.DataReplay = &session.Replay.Data;
		if (!processing.RigidReplay) processing.RigidReplay = &session.Replay.Rigid;
		GroupProcessCapture capture{run, &candidate.Outputs, &candidate.Nodes};
		capture.Common = candidate.SourceCommonBindings.empty() ? nullptr : &candidate.Common;
		capture.Session = candidate.SourceCommonBindings.empty() ? nullptr : &candidate;
		processing.SourceCommon = capture.Common;
		processing.SourceCommonAnimators = &candidate.CommonAnimators;
		// Output storage is charged by the shared stateful evaluator while it grows the candidate.
		snapshotCharge->Reset();
		checked = {};
		planCharge.Reset();
		if (EvaluateReplayInternal(
				document,
				plan,
				{},
				processing,
				candidate.Replay,
				diagnostic,
				budget.Available(),
				{},
				{},
				&capture
			) != Status::Ok)
			return diagnostic.Code;
		if (RetainedGroupRenderSessionBytes(candidate) > maximumBytes - previous->Bytes() - authored->Bytes())
			return fail(Status::LimitExceeded, "group process publication exceeds live bytes");
		core::Metrics::Count(
			"imagegraph.group_process.nodes", std::count(run.begin(), run.end(), uint8_t{1})
		);
		core::Metrics::SetGauge(
			"imagegraph.group_process.retained_bytes", double(RetainedGroupRenderSessionBytes(candidate))
		);
		session = std::move(candidate);
		diagnostic = {};
		return Status::Ok;
	} catch (const std::bad_alloc &) {
		SetDiagnostic(diagnostic, Status::LimitExceeded, "group processing allocation refused");
		return diagnostic.Code;
	}

	Status RefreshSourceGroupPurity(
		const Document &document,
		const Plan &plan,
		const EvaluationRequest &request,
		const SourcePurityRefresh &refresh,
		GroupRenderSession &session,
		Diagnostic &diagnostic,
		uint64_t maximumBytes
	) {
		GroupRenderOperation operation;
		operation.Mode = GroupRenderMode::RefreshPurity;
		operation.PurityRefresh = refresh;
		return ProcessGroupRender(document, plan, request, operation, session, diagnostic, maximumBytes);
	}

	Status ReadGroupRenderOutput(
		const Document &document,
		std::string_view outputId,
		const GroupRenderSession &session,
		CacheGroupReplayOutput &output,
		Diagnostic &diagnostic,
		uint64_t maximumBytes
	) try {
		if (!maximumBytes || maximumBytes > Limits::MaximumEvaluationBytes ||
			document.Outputs.size() > Limits::MaximumOutputs) {
			SetDiagnostic(diagnostic, Status::LimitExceeded, "group socket observation exceeds bounds");
			return diagnostic.Code;
		}
		if (ValidateCacheGroupReplay(session.Outputs, maximumBytes, diagnostic) != Status::Ok)
			return diagnostic.Code;
		const auto selected =
			std::find_if(document.Outputs.begin(), document.Outputs.end(), [&](const Output &record) {
				return record.Id == outputId;
			});
		if (selected == document.Outputs.end()) {
			SetDiagnostic(diagnostic, Status::InvalidOutput, "held group output selector is absent");
			return diagnostic.Code;
		}
		const auto producer =
			std::find_if(session.Outputs.Nodes.begin(), session.Outputs.Nodes.end(), [&](const auto &node) {
				return node.NodeId == selected->NodeId;
			});
		const auto current =
			std::find_if(document.Nodes.begin(), document.Nodes.end(), [&](const Node &node) {
				return node.Id == selected->NodeId;
			});
		if (producer == session.Outputs.Nodes.end() || current == document.Nodes.end() ||
			producer->NodeType != current->Type ||
			!FindPortType(*current, selected->Port, PortDirection::Output)) {
			SetDiagnostic(
				diagnostic, Status::InvalidOutput, "held group producer is absent", selected->NodeId
			);
			return diagnostic.Code;
		}
		const auto held =
			std::find_if(producer->Outputs.begin(), producer->Outputs.end(), [&](const auto &port) {
				return port.Port == selected->Port;
			});
		if (held == producer->Outputs.end() || !GroupHeldPortCompatible(document, *current, *held)) {
			SetDiagnostic(
				diagnostic,
				Status::InvalidOutput,
				"held group socket is absent",
				selected->NodeId,
				selected->Port
			);
			return diagnostic.Code;
		}
		if (held->Refusal) {
			diagnostic = *held->Refusal;
			return diagnostic.Code;
		}
		uint64_t bytes = RetainedGroupRenderSessionBytes(session);
		if (!AddBytes(bytes, sizeof(output) + held->Port.capacity() + output.Port.capacity()) ||
			(held->Data && !AddBytes(bytes, CacheGroupValueCloneBytes(*held->Data).value_or(UINT64_MAX))) ||
			(output.Data && !AddBytes(bytes, detail::RetainedPayloadBytes(*output.Data))) ||
			bytes > maximumBytes) {
			SetDiagnostic(diagnostic, Status::LimitExceeded, "held group socket clone exceeds live bytes");
			return diagnostic.Code;
		}
		CacheGroupReplayOutput observed = *held;
		output = std::move(observed);
		diagnostic = {};
		return Status::Ok;
	} catch (const std::bad_alloc &) {
		SetDiagnostic(diagnostic, Status::LimitExceeded, "group socket observation allocation refused");
		return diagnostic.Code;
	}

	Status EvaluateStateful(
		const Document &document,
		const Plan &plan,
		const std::string &outputId,
		const EvaluationRequest &request,
		StatefulEvaluationResult &result,
		Diagnostic &diagnostic,
		uint64_t maximumBytes
	) {
		return EvaluateReplayInternal(document, plan, outputId, request, result, diagnostic, maximumBytes);
	}

	static bool ValidateStatefulOutputIds(std::span<const std::string> ids, Diagnostic &diagnostic) {
		if (ids.size() > Limits::MaximumOutputs) {
			SetDiagnostic(diagnostic, Status::LimitExceeded, "selected output count exceeds bounds");
			return false;
		}
		for (size_t index = 0; index < ids.size(); ++index)
			for (size_t prior = 0; prior < index; ++prior)
				if (ids[index] == ids[prior]) {
					SetDiagnostic(diagnostic, Status::DuplicateId, "selected output IDs must be unique");
					return false;
				}
		return true;
	}

	Status EvaluateStatefulOutputs(
		const Document &document,
		const Plan &plan,
		std::span<const std::string> outputIds,
		const EvaluationRequest &request,
		StatefulOutputEvaluationResult &result,
		Diagnostic &diagnostic,
		uint64_t maximumBytes
	) {
		if (outputIds.empty() || outputIds.size() > Limits::MaximumOutputs) {
			SetDiagnostic(diagnostic, Status::InvalidOutput, "batch needs bounded selected output IDs");
			return diagnostic.Code;
		}
		if (!ValidateStatefulOutputIds(outputIds, diagnostic)) return diagnostic.Code;
		return EvaluateReplayInternal(
			document, plan, outputIds.front(), request, result, diagnostic, maximumBytes, outputIds
		);
	}

	Status EvaluateStatefulNodeInputs(
		const Document &document,
		const Plan &plan,
		std::string_view nodeId,
		const EvaluationRequest &request,
		StatefulInputEvaluationResult &result,
		Diagnostic &diagnostic,
		uint64_t maximumBytes,
		std::span<const std::string> outputIds
	) {
		if (nodeId.empty()) {
			SetDiagnostic(diagnostic, Status::InvalidValue, "stateful input snapshot needs a node ID");
			return diagnostic.Code;
		}
		if (!ValidateStatefulOutputIds(outputIds, diagnostic)) return diagnostic.Code;
		const std::string noOutput;
		return EvaluateReplayInternal(
			document, plan, noOutput, request, result, diagnostic, maximumBytes, outputIds, nodeId
		);
	}

	Status EvaluateArray(
		const Document &document,
		const Plan &plan,
		const std::string &outputId,
		const EvaluationRequest &request,
		ImageArray &images,
		Diagnostic &diagnostic
	) {
		EvaluationResult result;
		const Status status = EvaluateResult(document, plan, outputId, request, result, diagnostic);
		if (status != Status::Ok) return status;
		if (auto *array = std::get_if<ImageArray>(&result.Data)) {
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
		EvaluationResult result;
		const Status status = EvaluateResult(document, plan, outputId, request, result, diagnostic);
		if (status != Status::Ok) return status;
		const auto output =
			std::find_if(document.Outputs.begin(), document.Outputs.end(), [&](const Output &candidate) {
				return candidate.Id == outputId;
			});
		ValueOutputs *ports = FindValueOutputs(result.Data);
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
		const auto selectedOutput =
			std::find_if(document.Outputs.begin(), document.Outputs.end(), [&](const auto &output) {
				return output.Id == outputId;
			});
		const auto selectedNode =
			std::find_if(document.Nodes.begin(), document.Nodes.end(), [&](const auto &node) {
				return selectedOutput != document.Outputs.end() && node.Id == selectedOutput->NodeId;
			});
		const auto domain = selectedNode == document.Nodes.end()
								? std::optional<SourceSocketDomain>{}
								: FindOutputDomain(*selectedNode, result.Data, selected->Port);
		value = {std::move(selected->Port), std::move(selected->Data), domain};
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
} // namespace engine::imagegraph
