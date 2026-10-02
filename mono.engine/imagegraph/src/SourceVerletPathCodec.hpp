#pragma once
#include "SourceVerletPath.hpp"

#include <iomanip>
#include <istream>
#include <ostream>

namespace engine::imagegraph::detail {
	inline void WriteSourceVerletPath(std::ostream &stream, const SourcePathData2D &data) {
		const auto &mesh = *data.Mesh;
		stream << mesh.Verlet << ' ' << mesh.Warp << ' ' << mesh.VerletQuads << ' ' << mesh.OriginProcessorRow
			   << ' ' << mesh.OriginNodeId.size() << ' ' << std::quoted(mesh.OriginNodeId) << ' '
			   << mesh.Simulation.Points.size() << ' ' << mesh.Simulation.Edges.size() << ' '
			   << mesh.Triangles.size() << ' ' << mesh.Quads.size() << ' ' << mesh.SparseQuads.size() << ' '
			   << data.CachedLengths.size() << ' ' << data.CachedTotalLength << ' ' << mesh.Center.X << ' '
			   << mesh.Center.Y << ' ';
		for (const auto component : mesh.Bounds)
			stream << component << ' ';
		for (const auto &point : mesh.Simulation.Points) {
			stream << point.Position.X << ' ' << point.Position.Y << ' ' << point.Previous.X << ' '
				   << point.Previous.Y << ' ' << point.BeforePrevious.X << ' ' << point.BeforePrevious.Y
				   << ' ' << point.Drag << ' ' << point.Pin << ' ' << point.Rest << ' ' << point.Active << ' '
				   << point.UV.X << ' ' << point.UV.Y << ' ' << point.Original.X << ' ' << point.Original.Y
				   << ' ' << point.VelocityReference.X << ' ' << point.VelocityReference.Y << ' '
				   << bool(point.DrawPosition) << ' ' << point.DrawPosition.value_or(Vector2{}).X << ' '
				   << point.DrawPosition.value_or(Vector2{}).Y << ' ' << point.Blend << ' '
				   << point.SourceIndex << ' ';
		}
		for (const auto &edge : mesh.Simulation.Edges)
			stream << edge.First << ' ' << edge.Second << ' ' << edge.Distance << ' ' << edge.Flexibility
				   << ' ' << edge.DirectionDegrees << ' ' << edge.AngularDrag << ' ' << edge.Active << ' '
				   << edge.PreviousEdge << ' ' << edge.NextEdge << ' ';
		for (const auto &triangle : mesh.Triangles)
			for (const auto index : triangle)
				stream << index << ' ';
		for (const auto &quad : mesh.Quads)
			for (const auto index : quad)
				stream << index << ' ';
		for (const auto &quad : mesh.SparseQuads)
			stream << bool(quad) << ' ' << (quad ? (*quad)[0] : 0) << ' ' << (quad ? (*quad)[1] : 0) << ' ';
		for (const auto length : data.CachedLengths)
			stream << bool(length) << ' ' << length.value_or(0) << ' ';
	}
	inline bool ReadVerletBoolean(std::istream &stream, bool &output) {
		unsigned value;
		if (!(stream >> value) || value > 1) return false;
		output = value != 0;
		return true;
	}
	template <class Admit>
	bool ReadSourceVerletPath(std::istream &stream, SourcePathData2D &data, Admit &&admit) {
		if (!admit(sizeof(MeshData2D))) return false;
		auto &mesh = data.Mesh.emplace();
		size_t originLength = 0;
		if (!ReadVerletBoolean(stream, mesh.Verlet) || !ReadVerletBoolean(stream, mesh.Warp) ||
			!ReadVerletBoolean(stream, mesh.VerletQuads) ||
			!(stream >> mesh.OriginProcessorRow >> originLength) || originLength > Limits::MaximumTextBytes ||
			!admit(std::max(originLength, std::string{}.capacity())))
			return false;
		stream >> std::ws;
		if (stream.get() != '"') return false;
		mesh.OriginNodeId.reserve(originLength);
		for (;;) {
			int character = stream.get();
			if (character == std::char_traits<char>::eof()) return false;
			if (character == '"') break;
			if (character == '\\') {
				character = stream.get();
				if (character == std::char_traits<char>::eof()) return false;
			}
			if (mesh.OriginNodeId.size() >= originLength) return false;
			mesh.OriginNodeId.push_back(char(character));
		}
		if (mesh.OriginNodeId.size() != originLength) return false;
		size_t pointCount, edgeCount, triangleCount, quadCount, sparseCount, lengthCount;
		if (!(stream >> pointCount >> edgeCount >> triangleCount >> quadCount >> sparseCount >> lengthCount >>
			  data.CachedTotalLength >> mesh.Center.X >> mesh.Center.Y) ||
			pointCount > Limits::MaximumArrayElements || edgeCount > Limits::MaximumLinks ||
			triangleCount > Limits::MaximumLinks || quadCount > Limits::MaximumArrayElements ||
			sparseCount > Limits::MaximumLinks || lengthCount > Limits::MaximumLinks)
			return false;
		const uint64_t bytes = pointCount * sizeof(VerletPoint) + edgeCount * sizeof(VerletEdge) +
							   triangleCount * sizeof(std::array<uint32_t, 3>) +
							   quadCount * sizeof(std::array<uint32_t, 2>) +
							   sparseCount * sizeof(std::optional<std::array<uint32_t, 2>>) +
							   lengthCount * sizeof(std::optional<double>);
		if (!admit(bytes)) return false;
		for (auto &component : mesh.Bounds)
			if (!(stream >> component)) return false;
		mesh.Simulation.Points.resize(pointCount);
		mesh.Simulation.Edges.resize(edgeCount);
		mesh.Triangles.resize(triangleCount);
		mesh.Quads.resize(quadCount);
		mesh.SparseQuads.resize(sparseCount);
		data.CachedLengths.resize(lengthCount);
		for (auto &point : mesh.Simulation.Points) {
			bool hasDraw;
			Vector2 draw;
			if (!(stream >> point.Position.X >> point.Position.Y >> point.Previous.X >> point.Previous.Y >>
				  point.BeforePrevious.X >> point.BeforePrevious.Y >> point.Drag) ||
				!ReadVerletBoolean(stream, point.Pin) || !ReadVerletBoolean(stream, point.Rest) ||
				!ReadVerletBoolean(stream, point.Active) ||
				!(stream >> point.UV.X >> point.UV.Y >> point.Original.X >> point.Original.Y >>
				  point.VelocityReference.X >> point.VelocityReference.Y) ||
				!ReadVerletBoolean(stream, hasDraw) ||
				!(stream >> draw.X >> draw.Y >> point.Blend >> point.SourceIndex))
				return false;
			if (hasDraw) point.DrawPosition = draw;
		}
		for (auto &edge : mesh.Simulation.Edges)
			if (!(stream >> edge.First >> edge.Second >> edge.Distance >> edge.Flexibility >>
				  edge.DirectionDegrees >> edge.AngularDrag) ||
				!ReadVerletBoolean(stream, edge.Active) || !(stream >> edge.PreviousEdge >> edge.NextEdge))
				return false;
		for (auto &triangle : mesh.Triangles)
			for (auto &index : triangle)
				if (!(stream >> index)) return false;
		for (auto &quad : mesh.Quads)
			for (auto &index : quad)
				if (!(stream >> index)) return false;
		for (auto &quad : mesh.SparseQuads) {
			bool present;
			std::array<uint32_t, 2> indices;
			if (!ReadVerletBoolean(stream, present) || !(stream >> indices[0] >> indices[1])) return false;
			if (present) quad = indices;
		}
		for (auto &length : data.CachedLengths) {
			bool present;
			double value;
			if (!ReadVerletBoolean(stream, present) || !(stream >> value)) return false;
			if (present) length = value;
		}
		return ValidSourceVerletPath(data);
	}
}
