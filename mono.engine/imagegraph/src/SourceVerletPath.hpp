#pragma once
#include "Mesh2DPayload.hpp"

#include <array>
#include <optional>

namespace engine::imagegraph::detail {
	template <bool Retained> uint64_t SourceVerletPathBytes(const SourcePathData2D &data) {
		uint64_t bytes = MeshVectorBytes<Retained>(data.CachedLengths);
		if (data.Mesh) {
			bytes = MeshAddBytes(bytes, sizeof(MeshData2D));
			bytes = MeshAddBytes(bytes, MeshVectorBytes<Retained>(data.Mesh->Simulation.Points));
			bytes = MeshAddBytes(bytes, MeshVectorBytes<Retained>(data.Mesh->Simulation.Edges));
			bytes = MeshAddBytes(bytes, MeshVectorBytes<Retained>(data.Mesh->Triangles));
			bytes = MeshAddBytes(bytes, MeshVectorBytes<Retained>(data.Mesh->Quads));
			bytes = MeshAddBytes(bytes, MeshVectorBytes<Retained>(data.Mesh->SparseQuads));
			bytes = MeshAddBytes(
				bytes, Retained ? data.Mesh->OriginNodeId.capacity() : data.Mesh->OriginNodeId.size()
			);
		}
		return bytes;
	}
	inline bool ValidSourceVerletPath(const SourcePathData2D &data) {
		if (!data.Mesh || !data.Inputs.empty() || data.CachedLengths.size() > Limits::MaximumLinks ||
			!std::isfinite(data.CachedTotalLength) || data.CachedTotalLength < 0)
			return false;
		if (!data.Mesh->Verlet || !ValidMesh2DData(*data.Mesh)) return false;
		double total = 0;
		for (const auto length : data.CachedLengths) {
			if (length && (!std::isfinite(*length) || *length < 0)) return false;
			if (length) total += *length;
		}
		return total == data.CachedTotalLength;
	}
	inline size_t LineCountSourceVerletPath(const SourcePathData2D &) {
		return 1;
	}
	inline double LengthSourceVerletPath(const SourcePathData2D &data, size_t = 0) {
		return data.CachedTotalLength;
	}
	inline std::optional<std::array<double, 3>>
	DistanceSourceVerletPath(const SourcePathData2D &data, double distance, size_t = 0) {
		if (!data.Mesh || !std::isfinite(distance)) return std::nullopt;
		const auto &mesh = *data.Mesh;
		for (size_t index = 0; index < mesh.Simulation.Edges.size(); ++index) {
			const auto &edge = mesh.Simulation.Edges[index];
			if (!edge.Active) continue;
			if (index >= data.CachedLengths.size() || !data.CachedLengths[index]) return std::nullopt;
			const double length = *data.CachedLengths[index];
			if (distance > length) {
				distance -= length;
				continue;
			}
			if (length == 0) return std::nullopt;
			const auto a = mesh.Simulation.Points[edge.First].Position,
					   b = mesh.Simulation.Points[edge.Second].Position;
			const double t = distance / length;
			const std::array<double, 3> point{a.X + (b.X - a.X) * t, a.Y + (b.Y - a.Y) * t, 1};
			if (!std::isfinite(point[0]) || !std::isfinite(point[1])) return std::nullopt;
			return point;
		}
		return std::array<double, 3>{0, 0, 1};
	}
	inline std::optional<std::array<double, 3>>
	SampleSourceVerletPath(const SourcePathData2D &data, double ratio, size_t line = 0) {
		return DistanceSourceVerletPath(data, std::clamp(ratio, 0.0, .99) * data.CachedTotalLength, line);
	}
}
