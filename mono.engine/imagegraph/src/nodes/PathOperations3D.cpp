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
}
