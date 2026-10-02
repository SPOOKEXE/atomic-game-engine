#include "Path3D.hpp"

#include <numbers>
namespace engine::imagegraph::detail {
	bool SampleSourcePath3D(NodeContext &context) {
		const Value *input = context.Find("path");
		const auto *path = input ? std::get_if<PathValue3D>(input) : nullptr;
		if (!path || !path->Data || !path->Data->SourcePresent) {
			context.SetValue("position", Vector2{});
			context.SetValue("direction", 0.0);
			context.SetValue("weight", 0.0);
			return context.FailureCode == Status::Ok;
		}
		if (!ValidRuntimeValue(*input))
			return context.Fail(Status::InvalidValue, "spatial sample path is invalid", "path");
		PathRuntime3D runtime(*path->Data, &context);
		if (!runtime.Valid())
			return context.Fail(Status::InvalidValue, "spatial sample path length is invalid", "path");
		const auto range = context.Vec2("range", {0, 1});
		const double shift = context.Scalar("shift"), raw = context.Scalar("ratio"),
					 mode = context.SourceChoice("type");
		double ratio = range.X + (range.Y - range.X) * (raw + shift - std::trunc(shift));
		bool inverse = false;
		if (mode == 0)
			ratio -= std::trunc(ratio);
		else if (mode == 1) {
			const double whole = std::floor(ratio);
			double fraction = ratio - std::trunc(ratio);
			if (std::fmod(whole, 2) == 1 && fraction != 0) {
				fraction = 1 - fraction;
				inverse = true;
			}
			ratio = fraction;
		} else if (mode == 2)
			ratio = std::clamp(ratio, 0.0, .999);
		if (!std::isfinite(ratio))
			return context.Fail(Status::InvalidValue, "spatial sample ratio is nonfinite", "ratio");
		const int64_t index = context.Integer("path_index");
		if (context.FailureCode != Status::Ok) return false;
		if (index < 0)
			return context.Fail(Status::InvalidValue, "spatial sample line index is undefined", "path_index");
		const auto point = runtime.Ratio(ratio, size_t(index)),
				   before = runtime.Ratio(std::clamp(ratio - .0001, 0.0, 1.0), size_t(index)),
				   after = runtime.Ratio(std::clamp(ratio + .0001, 0.0, 1.0), size_t(index));
		if (!MeshFinite(point.Position) || !MeshFinite(before.Position) || !MeshFinite(after.Position) ||
			!std::isfinite(point.Weight))
			return context.Fail(Status::InvalidValue, "spatial sample geometry is undefined", "path");
		if (runtime.SpatialLine(size_t(index))) {
			const double sign = inverse ? -1 : 1;
			const Vector3 direction{
				(before.Position.X - after.Position.X) * sign,
				(before.Position.Y - after.Position.Y) * sign,
				(before.Position.Z - after.Position.Z) * sign
			};
			context.SetValue("position", point.Position);
			context.SetValue("direction", direction);
		} else {
			const double sign = inverse ? -1 : 1, dx = (after.Position.X - before.Position.X) * sign,
						 dy = (after.Position.Y - before.Position.Y) * sign;
			double direction = std::atan2(-dy, dx) * 180 / std::numbers::pi;
			if (direction < 0) direction += 360;
			context.SetValue("position", Vector2{point.Position.X, point.Position.Y});
			context.SetValue("direction", direction);
		}
		context.SetValue("weight", point.Weight);
		return context.FailureCode == Status::Ok;
	}
	bool SourceSpatialPathOperation(NodeContext &context) {
		const bool combine = context.Authored.Type == "pc.path_array";
		using Borrowed = std::variant<const Path2D *, const PathData3D *>;
		std::vector<Borrowed> inputs;
		bool collect = false;
		size_t inputCount = 0, nodes = 1;
		uint64_t bytes = sizeof(PathData3D) + sizeof(SourcePathData3D);
		const auto append = [&](const auto &value) {
			Borrowed input;
			uint64_t size = 0;
			if (const auto *path = std::get_if<Path2D>(&value)) {
				if (!path->SourceOperation && path->Anchors.empty()) return true;
				if (++nodes > Limits::MaximumArrayElements || !ValidSourcePath2D(*path, 2, &nodes))
					return context.Fail(Status::LimitExceeded, "spatial path tree exceeds bounds", "path");
				input = path;
				size = MeshAddBytes(sizeof(PathData3D), SourcePath2DBytes<false>(*path));
			} else if (const auto *path = std::get_if<PathValue3D>(&value)) {
				if (!path->Data || !path->Data->SourcePresent) return true;
				if (!ValidSourcePath3D(*path->Data, 1, &nodes))
					return context.Fail(Status::LimitExceeded, "spatial path tree exceeds bounds", "path");
				input = &*path->Data;
				size = SourcePath3DBytes<false>(*path->Data);
			} else
				return true;
			if (inputCount >= Limits::MaximumArrayElements)
				return context.Fail(Status::LimitExceeded, "spatial path line count exceeds bounds", "path");
			size = MeshAddBytes(size, sizeof(PathValue3D));
			if (size > Limits::MaximumEvaluationBytes - bytes)
				return context.Fail(Status::LimitExceeded, "spatial path payload exceeds bounds", "path");
			bytes += size;
			++inputCount;
			if (collect) inputs.push_back(input);
			return true;
		};
		const auto items = [&](auto &&self, const std::vector<SourceArrayItem> &entries) -> bool {
			for (const auto &item : entries) {
				if (const auto *leaf = std::get_if<ElementValue>(&item.Data)) {
					if (!append(*leaf)) return false;
				} else if (!self(self, std::get<std::vector<SourceArrayItem>>(item.Data)))
					return false;
			}
			return true;
		};
		const auto visit = [&](const Value *value) {
			if (!value) return true;
			if (!ValidRuntimeValue(*value))
				return context.Fail(Status::InvalidValue, "spatial path input is invalid", "path");
			if (const auto *array = std::get_if<ArrayValue>(value)) {
				if (!array->Items.empty()) return items(items, array->Items);
				for (const auto &leaf : array->Elements)
					if (!append(leaf)) return false;
				for (const auto &row : array->Nested)
					for (const auto &leaf : row)
						if (!append(leaf)) return false;
				return true;
			}
			return append(*value);
		};
		const auto scan = [&] {
			if (!combine) return visit(context.Find("path"));
			for (const auto &input : context.Authored.DynamicInputs)
				if (!visit(context.Find(input.Id))) return false;
			return true;
		};
		if (!scan()) return false;
		if (!combine && inputCount > 1)
			return context.Fail(
				Status::InvalidValue, "spatial path wrapper requires one resolved path", "path"
			);
		auto charge = context.ReserveWorkspace(inputCount * sizeof(Borrowed), "path");
		if (!charge) return false;
		inputs.reserve(inputCount);
		collect = true;
		inputCount = 0;
		nodes = 1;
		bytes = sizeof(PathData3D) + sizeof(SourcePathData3D);
		if (!scan() || !context.ReserveOutput(bytes + 32, "path")) return false;
		PathValue3D output;
		auto &data = output.Data.emplace();
		auto &op = data.SourceOperation.emplace();
		op.Kind = combine ? SourcePathOperationKind::Combine : SourcePathOperationKind::Reverse;
		if (context.Authored.Type == "pc.path_trim") {
			op.Kind = SourcePathOperationKind::Trim;
			op.TrimRange = context.Vec2("range", {0, 1});
			const double shift = context.Scalar("shift");
			op.TrimRange.X += shift;
			op.TrimRange.Y += shift;
			if (context.Boolean("clamp")) {
				const auto bounds = context.Vec2("range_2", {0, 1});
				op.TrimRange.X = std::max(bounds.X, std::min(bounds.Y, op.TrimRange.X));
				op.TrimRange.Y = std::max(bounds.X, std::min(bounds.Y, op.TrimRange.Y));
			}
		}
		op.Inputs.reserve(inputs.size());
		for (const auto &input : inputs) {
			PathValue3D child;
			if (const auto *planar = std::get_if<const Path2D *>(&input))
				child.Data.emplace().Source2D = **planar;
			else
				child.Data.emplace() = *std::get<const PathData3D *>(input);
			op.Inputs.push_back(std::move(child));
		}
		context.SetValue(combine ? "combined_path" : "path", std::move(output));
		return context.FailureCode == Status::Ok;
	}

}
