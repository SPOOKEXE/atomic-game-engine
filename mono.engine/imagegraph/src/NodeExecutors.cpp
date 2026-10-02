#include "NodeExecutors.hpp"

#include "GroupBoundary.hpp"
#include "nodes/Families.hpp"

#include <unordered_map>

namespace engine::imagegraph::detail {
	namespace {
		const std::unordered_map<std::string_view, ExecutorEntry> &Executors() {
			static const std::unordered_map<std::string_view, ExecutorEntry> executors = [] {
				std::unordered_map<std::string_view, ExecutorEntry> merged;
				for (const auto family :
					 {AudioExecutors(),			 AudioFileExecutors(),	 ArrayExecutors(),
					  ArrayStructureExecutors(), ArrayEditExecutors(),	 ArrayNumericExecutors(),
					  RandomExecutors(),		 FilterExecutors(),		 GenerateExecutors(),
					  GradientExecutors(),		 MatrixExecutors(),		 CurveExecutors(),
					  ValueExecutors(),			 VectorExecutors(),		 OutlineExecutors(),
					  BlurExecutors(),			 TransformExecutors(),	 PathExecutors(),
					  PointExecutors(),			 MeshExecutors(),		 MeshModifyExecutors(),
					  SourceMesh2DExecutors(),	 Source2DExecutors(),	 SourceTextExecutors(),
					  SourcePcxExecutors(),		 SceneExecutors(),		 SourceSdfExecutors(),
					  SimulationExecutors(),	 SourceValueExecutors(), SourceDataExecutors(),
					  SourceMatrixExecutors(),	 SourcePathExecutors(),	 HostExecutors(),
					  TriggerExecutors(),		 TemporalExecutors()})
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
			const auto acceptsGeneral = [](std::string_view type) {
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
					   type == "pc.pcx_array_set" || type == "pc.pcx_condition";
			};
			const auto validateValue = [&](const Value &value, std::string_view port) {
				const auto *array = std::get_if<ArrayValue>(&value);
				const bool groupTransport =
					(context.Authored.Type == "pc.group_input" && port == "parent_value") ||
					(context.Authored.Type == "pc.group_output" && port == "value");
				if (array && !array->Items.empty() && !groupTransport &&
					!acceptsGeneral(context.Authored.Type))
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
			for (const auto &[port, images] : context.ImageArrays)
				if (images)
					for (const Image &image : images->Images)
						if (!validate(image, port)) return false;
			return found->second.Run(context);
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
