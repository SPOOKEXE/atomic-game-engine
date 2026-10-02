#pragma once

#include "MeshPayload.hpp"

namespace engine::imagegraph::detail {
	template <bool retained> uint64_t Mesh2DDataStorageBytes(const MeshData2D &data) {
		uint64_t bytes = detail::MeshAddBytes(
			sizeof(MeshData2D), retained ? data.OriginNodeId.capacity() : data.OriginNodeId.size()
		);
		bytes = MeshAddBytes(bytes, MeshVectorBytes<retained>(data.Simulation.Points));
		bytes = MeshAddBytes(bytes, MeshVectorBytes<retained>(data.Simulation.Edges));
		bytes = MeshAddBytes(bytes, MeshVectorBytes<retained>(data.Triangles));
		return MeshAddBytes(
			bytes,
			MeshAddBytes(MeshVectorBytes<retained>(data.Quads), MeshVectorBytes<retained>(data.SparseQuads))
		);
	}
	inline bool ValidMesh2DData(const MeshData2D &data) {
		const auto &points = data.Simulation.Points;
		const auto &edges = data.Simulation.Edges;
		if (points.size() > Limits::MaximumArrayElements || edges.size() > Limits::MaximumLinks ||
			data.Triangles.size() > Limits::MaximumLinks ||
			data.Quads.size() > Limits::MaximumArrayElements ||
			data.SparseQuads.size() > Limits::MaximumLinks ||
			(!data.Quads.empty() && !data.SparseQuads.empty()) || !MeshFinite(data.Center) ||
			data.OriginNodeId.size() > Limits::MaximumTextBytes || (data.Verlet && data.Warp))
			return false;
		for (double component : data.Bounds)
			if (!std::isfinite(component)) return false;
		for (const auto &point : points)
			if (!MeshFinite(point.Position) || !MeshFinite(point.Previous) ||
				!MeshFinite(point.BeforePrevious) || !MeshFinite(point.UV) || !MeshFinite(point.Original) ||
				!MeshFinite(point.VelocityReference) ||
				(point.DrawPosition && !MeshFinite(*point.DrawPosition)) ||
				point.SourceIndex >= Limits::MaximumArrayElements || !std::isfinite(point.Drag))
				return false;
		for (const auto &edge : edges)
			if (edge.First >= points.size() || edge.Second >= points.size() || edge.PreviousEdge < -1 ||
				edge.NextEdge < -1 ||
				(edge.PreviousEdge >= 0 && uint64_t(edge.PreviousEdge) >= edges.size()) ||
				(edge.NextEdge >= 0 && uint64_t(edge.NextEdge) >= edges.size()) ||
				!std::isfinite(edge.Distance) || !std::isfinite(edge.Flexibility) ||
				!std::isfinite(edge.DirectionDegrees) || !std::isfinite(edge.AngularDrag))
				return false;
		for (const auto &triangle : data.Triangles)
			for (uint32_t point : triangle)
				if (point >= points.size()) return false;
		for (const auto &quad : data.Quads)
			for (uint32_t triangle : quad)
				if (triangle >= data.Triangles.size()) return false;
		for (const auto &quad : data.SparseQuads)
			if (quad)
				for (uint32_t triangle : *quad)
					if (triangle >= data.Triangles.size()) return false;
		return Mesh2DDataStorageBytes<true>(data) <= Limits::MaximumEvaluationBytes;
	}
	template <bool retained> uint64_t Mesh2DStorageBytes(const MeshValue2D &mesh) {
		return mesh.Data ? Mesh2DDataStorageBytes<retained>(*mesh.Data) : 0;
	}
	inline bool ValidMesh2DPayload(const MeshValue2D &mesh) {
		return !mesh.Data || ValidMesh2DData(*mesh.Data);
	}
}
