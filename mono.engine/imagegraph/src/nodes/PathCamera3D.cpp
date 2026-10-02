#include "Path3D.hpp"
#include "Processor.hpp"

#include <engine/imagegraph/SourceCamera3D.hpp>
namespace engine::imagegraph::detail {
	bool ProjectSourcePath3D(NodeContext &context) {
		ENGINE_PROFILE("imagegraph.path3d.camera");
		const Value *source = context.Find("path");
		const auto *spatial = source ? std::get_if<PathValue3D>(source) : nullptr;
		const auto *planar = source ? std::get_if<Path2D>(source) : nullptr;
		if (source && !spatial && !planar)
			return context.Fail(Status::TypeMismatch, "path camera requires a path", "path");
		if (source && !ValidRuntimeValue(*source))
			return context.Fail(Status::InvalidValue, "path camera source is invalid", "path");
		const size_t depth = spatial && spatial->Data ? spatial->Data->Transforms.size() : 0;
		if (depth >= Limits::MaximumArrayDepth)
			return context.Fail(Status::LimitExceeded, "path camera exceeds wrapper depth", "path");
		uint32_t width = 1, height = 1;
		if (!ResolveDimension(context, "dimension", width, height)) return false;
		const std::string_view controls[]{
			"postioning_mode",
			"projection",
			"horizontal_angle",
			"vertical_angle",
			"distance",
			"orthographic_scale",
			"fov",
			"position",
			"rotation",
			"lookat_position"
		};
		auto scratch = context.ReserveWorkspace(
			(std::size(controls) + 1) * (sizeof(EvaluationInputValue) + 32), "camera"
		);
		if (!scratch) return false;
		std::vector<EvaluationInputValue> inputs;
		inputs.reserve(std::size(controls) + 1);
		for (auto id : controls)
			if (const Value *value = context.Find(id))
				inputs.push_back({std::string(id), *value, context.IsLinked(id), {}});
		Vector2 clipping{.1, 100};
		if (const Value *value = context.Find("depth_range")) {
			const auto *range = std::get_if<Vector2>(value);
			if (!range)
				return context.Fail(
					Status::TypeMismatch, "path camera depth requires Vector2", "depth_range"
				);
			clipping = *range;
		}
		inputs.push_back({"clipping_distance", clipping, false, {}});
		SourceCameraPose pose;
		Diagnostic diagnostic;
		if (ResolveSourceCameraPose(inputs, width, height, pose, diagnostic) != Status::Ok)
			return context.Fail(diagnostic.Code, diagnostic.Message, diagnostic.Port);
		PathTransform3D projection;
		projection.Projective = true;
		projection.DepthWeight = context.Boolean("apply_depth_to_weight");
		if (pose.Projection == SourceCameraProjection::Orthographic) {
			const double scale = context.Scalar("orthographic_scale", .5);
			pose.OrthographicViewSize = {1 / scale, double(width) / height / scale};
			projection.ProjectionScale = {
				pose.OrthographicViewSize.X * width / 4, pose.OrthographicViewSize.Y * height / 4
			};
		} else if (pose.Projection == SourceCameraProjection::Perspective)
			projection.ProjectionScale = {double(width) * width / 4, double(height) * height / 4};
		else
			return context.Fail(
				Status::UnsupportedExecution, "path camera projection has no source case", "projection"
			);
		if (ResolveSourceCameraMatrices(
				pose, width, height, projection.CameraView, projection.CameraProjection, diagnostic
			) != Status::Ok)
			return context.Fail(diagnostic.Code, diagnostic.Message, diagnostic.Port);
		const uint64_t bytes =
			spatial ? RetainedPayloadBytes(*spatial) + (spatial->Data ? 0 : sizeof(PathData3D))
					: (planar ? RetainedPayloadBytes(*planar) + sizeof(PathData3D) : sizeof(PathData3D));
		if (!context.ReserveOutput(
				MeshAddBytes(bytes, (depth + 1) * sizeof(PathTransform3D)) + 64, "rendered"
			))
			return false;
		PathValue3D output;
		if (spatial && spatial->Data)
			output = *spatial;
		else {
			auto &data = output.Data.emplace();
			if (planar) data.Source2D = *planar;
			data.SourcePresent = planar && (context.IsLinked("path") || !planar->Anchors.empty() ||
											bool(planar->SourceOperation));
		}
		output.Data->Transforms.push_back(projection);
		if (!ValidRuntimeValue(output))
			return context.Fail(Status::InvalidValue, "path camera payload is invalid", "rendered");
		context.SetValue("rendered", std::move(output));
		return context.FailureCode == Status::Ok;
	}
}
