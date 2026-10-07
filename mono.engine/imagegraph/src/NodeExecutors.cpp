#include "NodeExecutors.hpp"

#include "GroupBoundary.hpp"
#include "SourceFontTransport.hpp"
#include "nodes/Families.hpp"

#include <unordered_map>

namespace engine::imagegraph::detail {
	namespace {
		const std::unordered_map<std::string_view, ExecutorEntry> &Executors() {
			static const std::unordered_map<std::string_view, ExecutorEntry> executors = [] {
				std::unordered_map<std::string_view, ExecutorEntry> merged;
				for (const auto family :
					 {AudioExecutors(),
					  AudioFileExecutors(),
					  ArrayExecutors(),
					  ArrayStructureExecutors(),
					  ArrayEditExecutors(),
					  ArrayNumericExecutors(),
					  RandomExecutors(),
					  FilterExecutors(),
					  GenerateExecutors(),
					  GradientExecutors(),
					  MatrixExecutors(),
					  CurveExecutors(),
					  ValueExecutors(),
					  VectorExecutors(),
					  OutlineExecutors(),
					  BlurExecutors(),
					  TransformExecutors(),
					  SourceAreaWarpExecutors(),
					  SourceBlendDepthExecutors(),
					  SourceBendExecutors(),
					  SourcePixelMathExecutors(),
					  SourceGlowExecutors(),
					  SourcePolarExecutors(),
					  SourceDisplaceExecutors(),
					  SourceJpegExecutors(),
					  SourcePixelSortExecutors(),
					  SourceNoiseExecutors(),
					  SourceCausticExecutors(),
					  SourceCellularExecutors(),
					  SourcePerlinExecutors(),
					  SourceVoronoiExtraExecutors(),
					  SourceShardNoiseExecutors(),
					  SourceStrandNoiseExecutors(),
					  SourceWeaveExecutors(),
					  SourcePytagoreanTileExecutors(),
					  SourceTileTransformExecutors(),
					  SourceErodeExecutors(),
					  PathExecutors(),
					  PointExecutors(),
					  MeshExecutors(),
					  MeshModifyExecutors(),
					  SourceMesh2DExecutors(),
					  Source2DExecutors(),
					  SourceHilbertExecutors(),
					  SourceAmbientOcclusionExecutors(),
					  SourceSimpleShapeExecutors(),
					  SourceTextExecutors(),
					  SourceFontDataExecutors(),
					  SourceFontTextExecutors(),
					  SourceBitmapFontExecutors(),
					  SourcePcxExecutors(),
					  SceneExecutors(),
					  SourceSdfExecutors(),
					  SimulationExecutors(),
					  SourceRigidExecutors(),
					  SourceValueExecutors(),
					  SourceDataExecutors(),
					  SourceArgumentExecutors(),
					  SourceMatrixExecutors(),
					  SourcePathExecutors(),
					  SourcePathComposeExecutors(),
					  SourcePathModifierExecutors(),
					  SourcePathGeometryExecutors(),
					  SourcePathShiftExecutors(),
					  SourcePathSequentialExecutors(),
					  SourcePathWeightExecutors(),
					  SourceQuaternionLookAtExecutors(),
					  SourcePointsExecutors(),
					  SourcePointDataExecutors(),
					  SourceSpatialPointsExecutors(),
					  SourceSpatialShapeExecutors(),
					  SourceTileExecutors(),
					  SourceSpriteStackExecutors(),
					  SourceNormalMapExecutors(),
					  SourceBevelExecutors(),
					  SourcePixelBevelExecutors(),
					  SourceVolumeProjectionExecutors(),
					  SourceCylinderProjectionExecutors(),
					  SourceHeightmapProjectionExecutors(),
					  SourceSurfaceProjectionExecutors(),
					  SourceAtlasExecutors(),
					  SourceAtlasPixelExecutors(),
					  SourcePaletteExecutors(),
					  SourceSurfaceDataExecutors(),
					  SourceSurfaceBufferExecutors(),
					  HostExecutors(),
					  TriggerExecutors(),
					  TemporalExecutors(),
					  SourceAnimationExecutors(),
					  SourceRoutingExecutors(),
					  SourceSwitchExecutors(),
					  SourceMiscExecutors(),
					  SourceSequenceAnimationExecutors(),
					  SourceCacheValueExecutors(),
					  SourceCacheResultsExecutors(),
					  SourceFrameCacheExecutors(),
					  SourceConversionExecutors(),
					  SourceColourFilterExecutors()})
					for (const ExecutorEntry &entry : family)
						merged.emplace(entry.Type, entry);
				merged.emplace("pc.group_input", ExecutorEntry{"pc.group_input", ExecuteGroupBoundary, true});
				merged.emplace(
					"pc.group_output", ExecutorEntry{"pc.group_output", ExecuteGroupBoundary, true}
				);
				return merged;
			}();
			return executors;
		}
		bool ExecuteChecked(NodeContext &context) {
			const auto found = Executors().find(context.Authored.Type);
			if (found == Executors().end())
				return context.Fail(Status::UnsupportedExecution, "executor is unavailable");
			const auto acceptsGeneral = [](std::string_view type, std::string_view port) {
				if (type == "pc.argument" && port == "default_value") return true;
				if ((type == "pc.path_sample" || type == "pc.path_smoothen") && port == "path") return true;
				if (SourceFontInput(type, port)) return true;
				if ((type == "pc.surface_to_buffer" && port == "surface") ||
					(type == "pc.surface_from_buffer" && port == "input_0"))
					return true;
				if (type == "pc.pin" && port == "in") return true;
				if (type == "pc.surface_project_cylinder_3_d" &&
					(port == "dimension" || port == "view_angle" || port == "position" ||
					 port == "angle_range" || port == "depth_range"))
					return true;
				if (type == "pc.surface_project_3_d" &&
					(port == "dimension" || port == "view_angle" || port == "position" ||
					 port == "depth_range" || port == "face_blending"))
					return true;
				if (type == "pc.heightmap_project_3_d" &&
					(port == "dimension" || port == "view_angle" || port == "position" ||
					 port == "height_range" || port == "depth_range"))
					return true;
				if (type == "pc.path_shape_3_d" && (port == "position" || port == "half_size")) return true;
				if (type == "pc.cache_results" && port == "surface_in") return true;
				if (type == "pc.switch" || type == "pc.threshold_switch") {
					if (port == "default_value" || port == "index") return true;
					const auto *entry = FindCatalogueEntry(type);
					size_t group = 0;
					const auto *slot = entry ? FindDynamicTemplate(*entry, port, group) : nullptr;
					return slot && (slot->SourceIndex == 0 || slot->SourceIndex == 1);
				}
				if (type == "pc.sequence_anim" && (port == "surface_in" || port == "sequence")) return true;
				if (type == "pc.quarternion_lookat" &&
					(port == "origin" || port == "target" || port == "up" || port == "unit"))
					return true;
				if ((type == "pc.strand_gravity" || type == "pc.strand_update") && port == "input_0")
					return true;
				if (type == "pc.rigid_object" && (port == "attribute_mesh" || port == "texture")) return true;
				if (type == "pc.rigid_override" &&
					(port == "surfaces" || port == "positions" || port == "rotations" || port == "scales" ||
					 port == "blends" || port == "alpha" || port == "mass" || port == "friction" ||
					 port == "bounciness" || port == "gravity_scale"))
					return true;
				if (type.starts_with("pc.rigid_") &&
					(port == "object" || port == "objects" || port.starts_with("object_") ||
					 port == "filter_object" || port == "detect_objects"))
					return true;
				if (type == "pc.gradient" && (port == "gradient" || port == "progress_remap" ||
											  port == "inverse_curve" || port == "curve"))
					return true;
				if (type == "pc.vfx_renderer" && port.starts_with("input_1_")) return true;
				if (type == "pc.edge_detect" && port == "attribute_filter") return true;
				if (type == "pc.segment_filter" && port == "segment") return true;
				if (type == "pc.path_join") {
					const auto *entry = FindCatalogueEntry(type);
					size_t group = 0;
					const auto *slot = entry ? FindDynamicTemplate(*entry, port, group) : nullptr;
					return slot && slot->Id == "path";
				}
				if ((type == "pc.palette" && port == "palette") ||
					((type == "pc.palette_sort" || type == "pc.palette_shrink") && port == "palette_in") ||
					(type == "pc.palette_replace" &&
					 (port == "palette_in" || port == "palette_from" || port == "palette_to")))
					return true;
				if (((type == "pc.atlas_get" || type == "pc.atlas_set" || type == "pc.atlas_struct") &&
					 port == "input_0") ||
					(type == "pc.atlas_draw" && port == "input_1") ||
					(type == "pc.atlas_affector" && (port == "atlas_in" || port == "target_atlas")))
					return true;
				return type == "pc.array_add" || type == "pc.array_get" || type == "pc.array_set" ||
					   type == "pc.array_insert" || type == "pc.array_remove" || type == "pc.array_find" ||
					   type == "pc.array_zip" || type == "pc.array_unique" || type == "pc.array_rearrange" ||
					   type == "pc.array_uniform" || type == "pc.array" || type == "pc.array_reverse" ||
					   type == "pc.array_copy" || type == "pc.array_trim" || type == "pc.array_shift" ||
					   type == "pc.array_partition" || type == "pc.array_flattern" ||
					   type == "pc.array_cumulative" || type == "pc.array_sort" ||
					   type == "pc.array_composite" || type == "pc.array_convolute" ||
					   type == "pc.array_sample" || type == "pc.array_shuffle" ||
					   type == "pc.array_randomizer" || type == "pc.array_boolean_opr" ||
					   type == "pc.array_pin" || type == "pc.array_split" || type == "pc.struct" ||
					   type == "pc.struct_set" || type == "pc.struct_get" || type == "pc.string_join" ||
					   type == "pc.array_transpose" || type == "pc.array_length" || type == "pc.logic" ||
					   type == "pc.path_array" || type == "pc.statistic" || type == "pc.global_scope" ||
					   type == "pc.globalvar" || type == "pc.equation" || type == "pc.pcx_equation" ||
					   type == "pc.pcx_var" || type == "pc.pcx_fn_var" || type == "pc.pcx_array_get" ||
					   type == "pc.pcx_array_set" || type == "pc.pcx_condition" ||
					   type == "pc.points_remap" || type == "pc.point_3_d_camera" ||
					   type == "pc.scatter_points_3_d" || type == "pc.delay_value" ||
					   type == "pc.plot_linear" || type == "pc.condition" || type == "pc.cache_value_array" ||
					   type == "pc.interpret_number" || type == "pc.color_adjust" ||
					   type == "pc.color_replace" || type == "pc.colors_replace" ||
					   type == "pc.color_separate";
			};
			const auto validateValue = [&](const Value &value, std::string_view port) {
				const auto *array = std::get_if<ArrayValue>(&value);
				const bool groupTransport =
					(context.Authored.Type == "pc.group_input" && port == "parent_value") ||
					(context.Authored.Type == "pc.group_output" && port == "value");
				if (array && !array->Items.empty() && !groupTransport &&
					!acceptsGeneral(context.Authored.Type, port))
					return context.Fail(
						Status::UnsupportedExecution,
						"executor requires homogeneous source array normalization",
						port
					);
				return true;
			};
			for (const auto &[port, value] : context.Values)
				if (context.Find(port) == &value && !validateValue(value, port)) return false;
			for (const auto &[port, value] : context.ValueViews)
				if (value && context.Find(port) == value && !validateValue(*value, port)) return false;
			const auto validate = [&](const Image &image, std::string_view port) {
				if (!ValidSurfaceLayout(image, Limits::MaximumDimension, Limits::MaximumOutputBytes))
					return context.Fail(Status::InvalidValue, "input surface layout is invalid", port);
				if (!found->second.TypedSurfaces && image.Format != SurfaceFormat::RGBA8Unorm)
					return context.Fail(
						Status::UnsupportedExecution,
						"executor requires an RGBA8 surface pending format migration",
						port
					);
				return true;
			};
			for (const auto &[port, image] : context.Images)
				if (image && !validate(*image, port)) return false;
			for (const auto &input : context.Entry.Inputs)
				if (input.Type == ValueType::Image &&
					std::none_of(context.Images.begin(), context.Images.end(), [&](const auto &entry) {
						return entry.first == input.Id;
					}))
					if (const auto *image = context.Input(input.Id); image && !validate(*image, input.Id))
						return false;
			for (const auto &[port, images] : context.ImageArrays)
				if (images)
					for (const Image &image : images->Images)
						if (!validate(image, port)) return false;
			if (!found->second.Run(context)) return false;
			if (context.NoiseFieldRequested && IsNoiseImageGenerator(context.Authored.Type)) {
				if (context.OutputImages.empty())
					return context.Fail(
						Status::InvalidOutput, "noise generator produced no surface for its field", "field"
					);
				const auto &image = context.OutputImages.front().second;
				if (!context.ReserveOutput(
						sizeof(NoiseFieldData) + image.Pixels.size() + std::string{}.capacity(), "field"
					))
					return false;
				NoiseFieldValue field;
				field.Data.emplace().Raster = image;
				context.SetValue("field", std::move(field));
			}
			return context.FailureCode == Status::Ok;
		}

	}

	Executor FindExecutor(std::string_view type) {
		const auto found = Executors().find(type);
		return found == Executors().end() ? nullptr : ExecuteChecked;
	}

	std::vector<std::string_view> ExecutorTypes() {
		std::vector<std::string_view> types;
		for (const auto &[type, run] : Executors())
			types.push_back(type);
		return types;
	}
}
