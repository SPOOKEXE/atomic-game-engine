#include "../SourceMeshTransform.hpp"
#include "Families.hpp"
#include "Path3D.hpp"

#include <algorithm>
namespace engine::imagegraph::detail {
	bool EvaluateSourcePath3D(NodeContext &context) {
		ENGINE_PROFILE("imagegraph.path3d.build");
		using Ordered = std::pair<size_t, PathAnchor3D>;
		size_t count = 0;
		for (const auto &input : context.Authored.DynamicInputs) {
			size_t group = 0;
			const auto *slot = FindDynamicTemplate(context.Entry, input.Id, group);
			if (slot && slot->Id == "anchor") ++count;
		}
		if (count > Limits::MaximumPathAnchors || count > Limits::MaximumArrayElements / 10)
			return context.Fail(Status::LimitExceeded, "3D path exceeds anchor caps", "path_data");
		const uint64_t bytes =
			sizeof(PathData3D) +
			count * (sizeof(PathAnchor3D) + 10 * sizeof(ElementValue) + sizeof(std::vector<ElementValue>)) +
			128;
		if (!context.ReserveOutput(bytes, "path_data")) return false;
		auto scratch = context.ReserveWorkspace(count * sizeof(Ordered), "anchors");
		if (!scratch) return false;
		std::vector<Ordered> ordered;
		ordered.reserve(count);
		for (const auto &input : context.Authored.DynamicInputs) {
			size_t group = 0;
			const auto *slot = FindDynamicTemplate(context.Entry, input.Id, group);
			if (!slot || slot->Id != "anchor") continue;
			const Value *value = context.Find(input.Id);
			const auto *array = value ? std::get_if<ArrayValue>(value) : nullptr;
			if (!array || !array->Nested.empty() || !array->Items.empty())
				return context.Fail(Status::TypeMismatch, "3D anchor requires ten source fields", input.Id);
			PathAnchor3D anchor;
			for (size_t i = 0; i < std::min<size_t>(10, array->Elements.size()); ++i) {
				std::optional<double> number;
				if (const auto *scalar = std::get_if<double>(&array->Elements[i]))
					number = *scalar;
				else if (const auto *integer = std::get_if<int64_t>(&array->Elements[i]))
					number = double(*integer);
				if (!number || !std::isfinite(*number))
					return context.Fail(
						Status::InvalidValue, "3D anchor requires finite numeric fields", input.Id
					);
				if (i < 9)
					anchor.Controls[i] = *number;
				else
					anchor.Index = *number;
			}

			ordered.emplace_back(group, anchor);
		}
		std::sort(ordered.begin(), ordered.end(), [](const auto &a, const auto &b) {
			return a.first < b.first;
		});
		PathValue3D path;
		auto &data = path.Data.emplace();
		data.Loop = context.Boolean("loop");
		data.Anchors.reserve(count);
		const bool round = context.Boolean("round_anchor");
		ArrayValue anchors{ValueType::Scalar, {}};
		anchors.Nested.reserve(count);
		for (auto &[index, anchor] : ordered) {
			if (round)
				for (size_t axis = 0; axis < 3; ++axis) {
					const double lower = std::floor(anchor.Controls[axis]),
								 fraction = anchor.Controls[axis] - lower;
					anchor.Controls[axis] =
						lower + (fraction > .5 || (fraction == .5 && std::fmod(lower, 2) != 0));
				}
			data.Anchors.push_back(anchor);
			std::vector<ElementValue> row;
			row.reserve(10);
			for (double number : anchor.Controls)
				row.emplace_back(number);
			row.emplace_back(double(anchor.Index));
			anchors.Nested.push_back(std::move(row));
		}
		const double ratio = context.Scalar("sample_path"), mode = context.SourceChoice("sample_mode");
		if (context.FailureCode != Status::Ok) return false;
		if (!std::isfinite(ratio))
			return context.Fail(Status::InvalidValue, "3D path sample must be finite", "sample_path");
		PathRuntime3D runtime(data);
		if (!runtime.Valid())
			return context.Fail(Status::InvalidValue, "3D path length overflowed", "path_data");
		PathPoint3D point;
		if (mode == 0)
			point = runtime.Ratio(ratio);
		else if (mode == 1)
			point = runtime.BySegment(ratio);
		else
			return context.Fail(
				Status::UnsupportedExecution, "3D path sample mode has no source case", "sample_mode"
			);
		if (!MeshFinite(point.Position))
			return context.Fail(Status::InvalidValue, "3D path sample is degenerate", "position_out");
		context.SetValue("position_out", point.Position);
		context.SetValue("path_data", std::move(path));
		context.SetValue("anchors", std::move(anchors));
		return context.FailureCode == Status::Ok;
	}
	bool TransformSourcePath3D(NodeContext &context) {
		ENGINE_PROFILE("imagegraph.path3d.transform");
		const Value *input = context.Find("path");
		const auto *spatial = input ? std::get_if<PathValue3D>(input) : nullptr;
		const auto *planar = input ? std::get_if<Path2D>(input) : nullptr;
		if (input && !spatial && !planar)
			return context.Fail(Status::TypeMismatch, "3D path transform requires a path", "path");
		if (input && !ValidRuntimeValue(*input))
			return context.Fail(Status::InvalidValue, "path transform source is invalid", "path");
		PathTransform3D transform;
		const auto vector = [&](std::string_view id, Vector3 &target) {
			const Value *value = context.Find(id);
			if (!value) return true;
			if (const auto *typed = std::get_if<Vector3>(value))
				target = *typed;
			else if (const auto *number = std::get_if<double>(value))
				target = {*number, *number, *number};
			else if (const auto *number = std::get_if<int64_t>(value))
				target = {double(*number), double(*number), double(*number)};
			else
				return context.Fail(Status::TypeMismatch, "path transform control requires Vector3", id);
			return MeshFinite(target) ||
				   context.Fail(Status::InvalidValue, "path transform control must be finite", id);
		};
		if (!vector("position", transform.Position) || !vector("anchor", transform.Anchor) ||
			!vector("scale", transform.Scale))
			return false;
		if (const Value *value = context.Find("rotation")) {
			const auto *rotation = std::get_if<Quaternion>(value);
			if (!rotation)
				return context.Fail(
					Status::TypeMismatch, "path transform rotation requires Quaternion", "rotation"
				);
			transform.Rotation = *rotation;
		}
		if (!MeshFinite(SourceRotate(transform.Rotation, {1, 0, 0})))
			return context.Fail(
				Status::InvalidValue, "path transform quaternion must be nonzero finite", "rotation"
			);
		const size_t transforms = spatial && spatial->Data ? spatial->Data->Transforms.size() : 0;
		if (transforms >= Limits::MaximumArrayDepth)
			return context.Fail(Status::LimitExceeded, "path transform wrapper depth exceeds caps", "path");
		const uint64_t bytes =
			spatial ? RetainedPayloadBytes(*spatial) + (spatial->Data ? 0 : sizeof(PathData3D))
					: (planar ? RetainedPayloadBytes(*planar) + sizeof(PathData3D) : sizeof(PathData3D));
		if (!context.ReserveOutput(
				MeshAddBytes(bytes, (transforms + 1) * sizeof(PathTransform3D)) + 64, "path"
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
		output.Data->Transforms.push_back(transform);
		if (!ValidPayload(output, true))
			return context.Fail(Status::LimitExceeded, "transformed path exceeds payload caps", "path");
		context.SetValue("path", std::move(output));
		return context.FailureCode == Status::Ok;
	}

}
