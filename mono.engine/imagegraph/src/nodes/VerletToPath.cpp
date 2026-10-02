#include "../SourceVerletPath.hpp"
#include "VerletNodes.hpp"

namespace engine::imagegraph::detail {
	bool VerletMeshToPath(NodeContext &context) {
		const auto *input = context.Find("mesh");
		const auto *mesh = input ? std::get_if<MeshValue2D>(input) : nullptr;
		if (!mesh || !mesh->Data) return true;
		if (!mesh->Data->Verlet)
			return context.Fail(
				Status::UnsupportedExecution,
				"source Mesh To Path requires defined Verlet edge references",
				"mesh"
			);
		if (!ValidMesh2DPayload(*mesh))
			return context.Fail(Status::InvalidValue, "source Mesh To Path input is invalid", "mesh");
		const auto &edges = mesh->Data->Simulation.Edges;
		size_t lengthCount = 0;
		for (size_t index = 0; index < edges.size(); ++index)
			if (edges[index].Active) lengthCount = index + 1;
		if (!context.ReserveOutput(
				sizeof(SourcePathData2D) + Mesh2DStorageBytes<true>(*mesh) +
					lengthCount * sizeof(std::optional<double>),
				"path"
			))
			return false;
		Path2D output;
		auto &data = output.SourceOperation.emplace();
		data.Kind = SourcePathOperationKind::VerletMesh;
		data.Mesh = mesh->Data;
		data.CachedLengths.resize(lengthCount);
		for (size_t index = 0; index < lengthCount; ++index) {
			const auto &edge = edges[index];
			if (!edge.Active) continue;
			const auto a = mesh->Data->Simulation.Points[edge.First].Position,
					   b = mesh->Data->Simulation.Points[edge.Second].Position;
			const double length = std::hypot(b.X - a.X, b.Y - a.Y);
			data.CachedLengths[index] = length;
			data.CachedTotalLength += length;
		}
		if (!ValidSourceVerletPath(data))
			return context.Fail(
				Status::InvalidValue, "source Mesh To Path produced invalid cached lengths", "path"
			);
		context.SetValue("path", std::move(output));
		return context.FailureCode == Status::Ok;
	}
}
