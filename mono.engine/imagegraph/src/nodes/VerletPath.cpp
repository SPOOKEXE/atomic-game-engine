#include "Path.hpp"
#include "VerletNodes.hpp"

#include <numbers>

namespace engine::imagegraph::detail {
	bool VerletMeshFromPath(NodeContext &context) {
		if (const auto restored = RestoreVerletConstructor(context)) return *restored;
		const Value *input = context.Find("path");
		const auto *path = input ? std::get_if<Path2D>(input) : nullptr;
		if (!path) return true;
		const int64_t samples = std::max<int64_t>(1, context.Integer("samples", 8));
		if (uint64_t(samples) >= Limits::MaximumArrayElements)
			return context.Fail(Status::LimitExceeded, "verlet path samples exceed point bounds", "samples");
		const double flexibility = 1 - context.Scalar("tension", .5);
		if (context.FailureCode != Status::Ok) return false;
		PathRuntime runtime;
		if (!runtime.Init(context, *path)) return false;
		const uint64_t bytes = sizeof(MeshData2D) +
							   std::max(context.Authored.Id.size(), std::string{}.capacity()) +
							   (samples + 1) * sizeof(VerletPoint) + samples * sizeof(VerletEdge);
		if (!context.ReserveOutput(bytes, "mesh")) return false;
		MeshValue2D output;
		auto &mesh = output.Data.emplace();
		mesh.Verlet = true;
		mesh.OriginNodeId = context.Authored.Id;
		mesh.OriginProcessorRow = context.ProcessorRow;
		mesh.Simulation.Points.reserve(samples + 1);
		mesh.Simulation.Edges.reserve(samples);
		for (int64_t index = 0; index <= samples; ++index) {
			const auto sampled = runtime.PointRatio(std::clamp(double(index) / samples, 0.0, .999));
			VerletPoint point;
			point.Position = point.Previous = point.Original = {sampled.X, sampled.Y};
			// Source assigns drag to its temporary path point, leaving the new Verlet point at zero.
			mesh.Simulation.Points.push_back(point);
			if (index) {
				const auto &a = mesh.Simulation.Points[size_t(index - 1)].Position, &b = point.Position;
				VerletEdge edge;
				edge.First = uint32_t(index - 1);
				edge.Second = uint32_t(index);
				edge.Distance = std::hypot(b.X - a.X, b.Y - a.Y);
				edge.Flexibility = flexibility;
				edge.DirectionDegrees = std::atan2(a.Y - b.Y, b.X - a.X) * 180 / std::numbers::pi;
				if (edge.DirectionDegrees < 0) edge.DirectionDegrees += 360;
				mesh.Simulation.Edges.push_back(edge);
			}
		}
		return PublishVerletMesh(context, std::move(output));
	}
}
