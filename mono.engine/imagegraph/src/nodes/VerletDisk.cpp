#include "VerletNodes.hpp"

#include <array>
#include <cmath>
#include <numbers>

namespace engine::imagegraph::detail {
	bool VerletDiskMesh(NodeContext &context) {
		if (const auto restored = RestoreVerletConstructor(context)) return *restored;
		const Vector2 subdivision = context.Vec2("subdivision", {12, 4});
		if (!MeshFinite(subdivision))
			return context.Fail(Status::InvalidValue, "disk subdivision must be finite", "subdivision");
		if (subdivision.X != std::trunc(subdivision.X) || subdivision.Y != std::trunc(subdivision.Y))
			return context.Fail(
				Status::UnsupportedExecution,
				"fractional source disk allocation requires a reference capture",
				"subdivision"
			);
		const double columnValue = std::max(1.0, subdivision.X), rowValue = std::max(1.0, subdivision.Y);
		if (columnValue > Limits::MaximumArrayElements || rowValue > Limits::MaximumArrayElements ||
			1 + columnValue * (rowValue + 1) > Limits::MaximumArrayElements)
			return context.Fail(Status::LimitExceeded, "disk point count exceeds budget", "subdivision");
		const uint32_t columns = uint32_t(columnValue), rows = uint32_t(rowValue);
		const uint64_t pointCount = 1 + uint64_t(columns) * (rows + 1),
					   edgeCount = uint64_t(rows + 1) * (2 * columns + 1),
					   triangleCount = uint64_t(columns) * (2 * rows + 1);
		if (edgeCount > Limits::MaximumLinks || triangleCount > Limits::MaximumLinks)
			return context.Fail(Status::LimitExceeded, "disk topology exceeds budget", "subdivision");
		const bool quad = context.Boolean("quad"), cartesian = context.Boolean("cartesian");
		if (quad && triangleCount % 2)
			return context.Fail(
				Status::InvalidValue, "source disk final sparse quad references an absent triangle", "quad"
			);
		const uint64_t bytes =
			sizeof(MeshData2D) + pointCount * sizeof(VerletPoint) + edgeCount * sizeof(VerletEdge) +
			triangleCount * sizeof(std::array<uint32_t, 3>) +
			(quad ? (triangleCount - 1) * sizeof(std::optional<std::array<uint32_t, 2>>) : 0) +
			std::max(context.Authored.Id.size(), std::string{}.capacity());
		if (!context.ReserveOutput(bytes, "mesh")) return false;
		const Area area = VerletMeshArea(context);
		const double flexibility = 1 - context.Scalar("tension", .5), drag = context.Scalar("drag"),
					 angularDrag = context.Scalar("stiffness");
		if (context.FailureCode != Status::Ok) return false;
		MeshValue2D output;
		auto &mesh = output.Data.emplace();
		mesh.Verlet = true;
		mesh.OriginNodeId = context.Authored.Id;
		mesh.OriginProcessorRow = context.ProcessorRow;
		auto &points = mesh.Simulation.Points;
		auto &edges = mesh.Simulation.Edges;
		points.reserve(pointCount);
		edges.reserve(edgeCount);
		mesh.Triangles.reserve(triangleCount);
		const auto addPoint = [&](Vector2 position, Vector2 uv) {
			VerletPoint point;
			point.SourceIndex = uint32_t(mesh.Simulation.Points.size());
			point.Position = point.Previous = point.BeforePrevious = point.Original =
				point.VelocityReference = position;
			point.Drag = drag;
			point.UV = uv;
			points.push_back(point);
		};
		addPoint({area.CenterX, area.CenterY}, cartesian ? Vector2{.5, .5} : Vector2{});
		for (uint32_t row = 0; row <= rows; ++row)
			for (uint32_t column = 0; column < columns; ++column) {
				const double radius = double(row + 1) / (rows + 1),
							 angle = double(column) / columns * 360 * std::numbers::pi / 180;
				const Vector2 offset{radius * std::cos(angle), -radius * std::sin(angle)};
				addPoint(
					{area.CenterX + offset.X * area.HalfWidth, area.CenterY + offset.Y * area.HalfHeight},
					cartesian ? Vector2{.5 + offset.X * .5, .5 + offset.Y * .5}
							  : Vector2{double(column) / columns, double(row) / rows}
				);
			}
		const auto addEdge = [&](uint32_t first, uint32_t second, int64_t previous) {
			const auto a = points[first].Position, b = points[second].Position;
			VerletEdge edge{
				first,
				second,
				std::hypot(b.X - a.X, b.Y - a.Y),
				flexibility,
				std::atan2(a.Y - b.Y, b.X - a.X) * 180 / std::numbers::pi,
				angularDrag
			};
			if (edge.DirectionDegrees < 0) edge.DirectionDegrees += 360;
			edge.PreviousEdge = previous;
			if (previous >= 0) edges[size_t(previous)].NextEdge = int64_t(edges.size());
			edges.push_back(edge);
		};
		for (uint32_t column = 0; column <= columns; ++column) {
			int64_t previous = -1;
			for (int64_t row = -1; row < int64_t(rows); ++row) {
				const uint32_t first = row < 0 ? 0 : 1 + uint32_t(row) * columns + column % columns;
				const uint32_t second = 1 + uint32_t(row + 1) * columns + column % columns;
				addEdge(first, second, previous);
				previous = int64_t(edges.size()) - 1;
			}
		}
		for (uint32_t row = 0; row <= rows; ++row) {
			int64_t previous = -1;
			for (uint32_t column = 0; column < columns; ++column) {
				addEdge(1 + row * columns + column, 1 + row * columns + (column + 1) % columns, previous);
				previous = int64_t(edges.size()) - 1;
			}
		}
		for (uint32_t row = 0; row < rows; ++row)
			for (uint32_t column = 0; column < columns; ++column) {
				const uint32_t a = 1 + row * columns + column, b = 1 + row * columns + (column + 1) % columns,
							   c = 1 + (row + 1) * columns + column,
							   d = 1 + (row + 1) * columns + (column + 1) % columns;
				mesh.Triangles.push_back({a, b, c});
				mesh.Triangles.push_back({c, b, d});
			}
		for (uint32_t column = 0; column < columns; ++column)
			mesh.Triangles.push_back({0, 1 + column, 1 + (column + 1) % columns});
		if (quad) {
			mesh.SparseQuads.resize(triangleCount - 1);
			for (size_t index = 0; index < triangleCount; index += 2)
				mesh.SparseQuads[index] = std::array{uint32_t(index), uint32_t(index + 1)};
		}
		RecomputeVerletMeshBounds(mesh);
		return PublishVerletMesh(context, std::move(output));
	}
}
