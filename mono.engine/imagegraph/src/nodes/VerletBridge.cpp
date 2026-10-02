#include "Path.hpp"
#include "VerletNodes.hpp"

#include <numbers>

namespace engine::imagegraph::detail {
	bool VerletBridgeMesh(NodeContext &context) {
		if (const auto restored = RestoreVerletConstructor(context)) return *restored;
		const Vector2 subdivision = context.Vec2("subdivision", {4, 4});
		if (!MeshFinite(subdivision) || subdivision.X < 1 || subdivision.Y < 1 ||
			subdivision.X != std::trunc(subdivision.X) || subdivision.Y != std::trunc(subdivision.Y))
			return context.Fail(
				Status::InvalidValue, "source bridge requires positive integer subdivision", "subdivision"
			);
		size_t pathCount = 0;
		for (const auto &port : context.Authored.DynamicInputs) {
			const auto *value = context.Find(port.Id);
			if (value && std::holds_alternative<Path2D>(*value)) ++pathCount;
		}
		if (pathCount <= 1) return true;
		const double rows = (pathCount - 1) * subdivision.Y + 1;
		if ((subdivision.X + 1) * rows > Limits::MaximumArrayElements)
			return context.Fail(Status::LimitExceeded, "bridge points exceed bounds", "subdivision");
		const uint32_t gw = uint32_t(subdivision.X), gh = uint32_t(subdivision.Y);
		const size_t pointCount = (gw + 1) * size_t(rows), triangleCount = (pathCount - 1) * gw * gh * 2;
		const size_t edgeCount =
			(gw + 1) * gh + (gh + 1) * gw + (pathCount - 2) * ((gw + 1) * (gh - 1) + gh * gw);
		if (triangleCount > Limits::MaximumLinks || edgeCount > Limits::MaximumLinks)
			return context.Fail(Status::LimitExceeded, "bridge topology exceeds bounds", "subdivision");
		const bool loop = context.Boolean("loop"), pinFirst = context.Boolean("pin_first"),
				   quad = context.Boolean("quad");
		const double shift = context.Scalar("shift"), flexibility = 1 - context.Scalar("tension", .5),
					 drag = context.Scalar("drag"), angularDrag = context.Scalar("stiffness");
		if (context.FailureCode != Status::Ok) return false;
		const size_t sparseCount = quad && triangleCount ? triangleCount - 1 : 0;
		if (sparseCount > Limits::MaximumLinks)
			return context.Fail(Status::LimitExceeded, "bridge sparse quads exceed bounds", "quad");
		auto sampleCharge = context.ReserveWorkspace(pathCount * (gw + 1) * sizeof(Vector2), "path");
		if (!sampleCharge) return false;
		std::vector<Vector2> samples;
		samples.reserve(pathCount * (gw + 1));
		for (const auto &port : context.Authored.DynamicInputs) {
			const auto *value = context.Find(port.Id);
			const auto *path = value ? std::get_if<Path2D>(value) : nullptr;
			if (!path) continue;
			PathRuntime runtime;
			if (!runtime.Init(context, *path)) return false;
			for (uint32_t index = 0; index <= gw; ++index) {
				double ratio = double(index) / gw + shift;
				ratio = loop ? ratio - std::floor(ratio) : std::clamp(ratio, 0.0, .999);
				const auto point = runtime.PointRatio(ratio);
				samples.push_back({point.X, point.Y});
			}
		}
		const uint64_t bytes = sizeof(MeshData2D) +
							   std::max(context.Authored.Id.size(), std::string{}.capacity()) +
							   pointCount * sizeof(VerletPoint) + edgeCount * sizeof(VerletEdge) +
							   triangleCount * sizeof(std::array<uint32_t, 3>) +
							   sparseCount * sizeof(std::optional<std::array<uint32_t, 2>>);
		if (!context.ReserveOutput(bytes, "mesh")) return false;
		MeshValue2D output;
		auto &mesh = output.Data.emplace();
		mesh.Verlet = true;
		mesh.OriginNodeId = context.Authored.Id;
		mesh.OriginProcessorRow = context.ProcessorRow;
		auto &points = mesh.Simulation.Points;
		auto &edges = mesh.Simulation.Edges;
		points.resize(pointCount);
		edges.reserve(edgeCount);
		mesh.Triangles.reserve(triangleCount);
		const auto addEdge = [&](uint32_t first, uint32_t second, int64_t previous) {
			const auto &a = points[first].Position, &b = points[second].Position;
			VerletEdge edge;
			edge.First = first;
			edge.Second = second;
			edge.Distance = std::hypot(b.X - a.X, b.Y - a.Y);
			edge.Flexibility = flexibility;
			edge.AngularDrag = angularDrag;
			edge.DirectionDegrees = std::atan2(a.Y - b.Y, b.X - a.X) * 180 / std::numbers::pi;
			if (edge.DirectionDegrees < 0) edge.DirectionDegrees += 360;
			edge.PreviousEdge = previous;
			if (previous >= 0) edges[size_t(previous)].NextEdge = int64_t(edges.size());
			edges.push_back(edge);
		};
		const uint32_t columns = gw + 1, wrap = gw + !loop;
		for (size_t strip = 0; strip < pathCount - 1; ++strip) {
			const uint32_t start = uint32_t(strip * columns * gh), firstRow = strip != 0;
			for (uint32_t column = 0; column <= gw; ++column)
				for (uint32_t row = firstRow; row <= gh; ++row) {
					const auto a = samples[strip * columns + column],
							   b = samples[(strip + 1) * columns + column];
					const Vector2 position{a.X + (b.X - a.X) * row / gh, a.Y + (b.Y - a.Y) * row / gh};
					VerletPoint point;
					point.Position = point.Previous = point.BeforePrevious = point.Original =
						point.VelocityReference = position;
					point.UV = {
						double(column) / gw,
						double(strip) / (pathCount - 1) + double(row) / gh / (pathCount - 1)
					};
					point.Drag = drag;
					point.SourceIndex = start + row * columns + column;
					point.Pin = pinFirst && row == 0;
					points[start + row * columns + column] = point;
				}
			for (uint32_t column = 0; column <= gw; ++column) {
				int64_t previous = -1;
				// Source's cross-strip map lookup names an edge not yet built, so each strip starts a fresh
				// chain.
				for (uint32_t row = firstRow; row < gh; ++row) {
					addEdge(
						start + row * columns + column % wrap,
						start + (row + 1) * columns + column % wrap,
						previous
					);
					previous = int64_t(edges.size()) - 1;
				}
			}
			for (uint32_t row = firstRow; row <= gh; ++row) {
				int64_t previous = -1;
				for (uint32_t column = 0; column < gw; ++column) {
					addEdge(
						start + row * columns + column % wrap,
						start + row * columns + (column + 1) % wrap,
						previous
					);
					previous = int64_t(edges.size()) - 1;
				}
			}
			for (uint32_t row = 0; row < gh; ++row)
				for (uint32_t column = 0; column < gw; ++column) {
					const uint32_t a = start + row * columns + column % wrap,
								   b = start + row * columns + (column + 1) % wrap,
								   c = start + (row + 1) * columns + column % wrap,
								   d = start + (row + 1) * columns + (column + 1) % wrap;
					mesh.Triangles.push_back({a, b, c});
					mesh.Triangles.push_back({c, b, d});
				}
		}
		if (quad) {
			mesh.SparseQuads.resize(sparseCount);
			for (uint32_t index = 0; index < triangleCount; index += 2)
				mesh.SparseQuads[index] = std::array<uint32_t, 2>{index, index + 1};
		}
		RecomputeVerletMeshBounds(mesh);
		return PublishVerletMesh(context, std::move(output));
	}
}
