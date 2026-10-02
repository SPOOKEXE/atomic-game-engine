#include "FluidPayload.hpp"
#include "MeshPayload.hpp"

#include <engine/imagegraph/SimulationReplay.hpp>

#include <cmath>

namespace engine::imagegraph {
	uint64_t RetainedSimulationEntryBytes(const SimulationReplayEntry &entry) {
		uint64_t bytes = detail::MeshAddBytes(sizeof(SimulationReplayEntry), entry.NodeId.capacity());
		bytes = detail::MeshAddBytes(bytes, detail::MeshVectorBytes<true>(entry.State.Mesh.Points));
		bytes = detail::MeshAddBytes(bytes, detail::MeshVectorBytes<true>(entry.State.Mesh.Edges));
		bytes = detail::MeshAddBytes(bytes, detail::MeshVectorBytes<true>(entry.Topology.Triangles));
		bytes = detail::MeshAddBytes(bytes, detail::MeshVectorBytes<true>(entry.Topology.Quads));
		bytes = detail::MeshAddBytes(bytes, detail::MeshVectorBytes<true>(entry.Topology.SparseQuads));
		if (entry.Cache) bytes = detail::MeshAddBytes(bytes, detail::MeshVectorBytes<true>(*entry.Cache));
		return detail::MeshAddBytes(bytes, detail::FluidStorageBytes<true>(entry.Fluid));
	}
	uint64_t RetainedSimulationReplayBytes(const SimulationReplayState &state) {
		uint64_t bytes =
			detail::MeshAddBytes(sizeof(SimulationReplayState), detail::MeshVectorBytes<true>(state.Entries));
		for (const auto &entry : state.Entries)
			bytes = detail::MeshAddBytes(
				bytes, RetainedSimulationEntryBytes(entry) - sizeof(SimulationReplayEntry)
			);
		return bytes;
	}
	Status ValidateSimulationReplay(
		const SimulationReplayState &state, uint64_t maximumBytes, Diagnostic &diagnostic
	) {
		diagnostic = {};
		const auto refuse = [&](Status status, std::string message, std::string node = {}) {
			diagnostic.Code = status;
			diagnostic.Message = std::move(message);
			diagnostic.NodeId = std::move(node);
			return status;
		};
		if (state.Entries.size() > Limits::MaximumNodes ||
			RetainedSimulationReplayBytes(state) > maximumBytes ||
			RetainedSimulationReplayBytes(state) > Limits::MaximumEvaluationBytes)
			return refuse(Status::LimitExceeded, "simulation replay exceeds node or byte budget");
		for (size_t index = 0; index < state.Entries.size(); ++index) {
			const auto &entry = state.Entries[index];
			if (entry.NodeId.empty() || entry.NodeId.size() > Limits::MaximumTextBytes ||
				!entry.State.Initialized || entry.State.Tick > Limits::MaximumTick)
				return refuse(
					Status::InvalidValue, "simulation replay identity or tick is invalid", entry.NodeId
				);
			if (entry.Drag &&
				(!detail::MeshFinite(entry.Drag->Move) || !detail::MeshFinite(entry.Drag->Previous) ||
				 !std::isfinite(entry.Drag->PreviousAngle) ||
				 (entry.Drag->Anchor && !detail::MeshFinite(*entry.Drag->Anchor)) ||
				 !entry.State.Mesh.Points.empty() || !entry.State.Mesh.Edges.empty()))
				return refuse(Status::InvalidValue, "simulation drag replay is invalid", entry.NodeId);
			if (entry.Fluid.Data &&
				(entry.Cache || entry.Drag || !entry.State.Mesh.Points.empty() ||
				 !entry.State.Mesh.Edges.empty() || !entry.Topology.Triangles.empty() ||
				 !entry.Topology.Quads.empty() || !entry.Topology.SparseQuads.empty() ||
				 !detail::ValidFluidPayload(entry.Fluid) || entry.Fluid.Data->OriginNodeId != entry.NodeId ||
				 entry.Fluid.Data->OriginProcessorRow != entry.ProcessorRow ||
				 entry.Fluid.Data->Tick != entry.State.Tick ||
				 entry.Fluid.Data->AuthoringRevision != entry.State.AuthoringRevision))
				return refuse(Status::InvalidValue, "simulation fluid replay is invalid", entry.NodeId);
			if (entry.Cache) {
				if (entry.Drag || !entry.State.Mesh.Points.empty() || !entry.State.Mesh.Edges.empty() ||
					entry.Cache->size() > Limits::MaximumArrayElements)
					return refuse(
						Status::InvalidValue, "simulation cached controls are invalid", entry.NodeId
					);
				for (const auto point : *entry.Cache)
					if (!detail::MeshFinite(point))
						return refuse(
							Status::InvalidValue, "simulation cached point must be finite", entry.NodeId
						);
			}
			for (size_t earlier = 0; earlier < index; ++earlier)
				if (state.Entries[earlier].NodeId == entry.NodeId &&
					state.Entries[earlier].ProcessorRow == entry.ProcessorRow)
					return refuse(
						Status::DuplicateId, "simulation replay repeats node identity", entry.NodeId
					);
			if (entry.Topology.Triangles.size() > Limits::MaximumLinks ||
				entry.Topology.Quads.size() > Limits::MaximumArrayElements ||
				entry.Topology.SparseQuads.size() > Limits::MaximumLinks ||
				(!entry.Topology.Quads.empty() && !entry.Topology.SparseQuads.empty()) ||
				!detail::MeshFinite(entry.Topology.Center))
				return refuse(
					Status::LimitExceeded, "simulation replay topology exceeds budget", entry.NodeId
				);
			for (double component : entry.Topology.Bounds)
				if (!std::isfinite(component))
					return refuse(
						Status::InvalidValue, "simulation replay bounds must be finite", entry.NodeId
					);
			const auto &points = entry.State.Mesh.Points;
			const auto &edges = entry.State.Mesh.Edges;
			if (points.size() > Limits::MaximumArrayElements || edges.size() > Limits::MaximumLinks)
				return refuse(
					Status::LimitExceeded, "simulation replay mesh exceeds topology budget", entry.NodeId
				);
			for (const auto &triangle : entry.Topology.Triangles)
				for (uint32_t point : triangle)
					if (point >= points.size())
						return refuse(
							Status::InvalidValue, "simulation triangle endpoint is invalid", entry.NodeId
						);
			for (const auto &quad : entry.Topology.Quads)
				for (uint32_t triangle : quad)
					if (triangle >= entry.Topology.Triangles.size())
						return refuse(
							Status::InvalidValue, "simulation quad endpoint is invalid", entry.NodeId
						);
			for (const auto &quad : entry.Topology.SparseQuads)
				if (quad)
					for (uint32_t triangle : *quad)
						if (triangle >= entry.Topology.Triangles.size())
							return refuse(
								Status::InvalidValue,
								"simulation sparse quad endpoint is invalid",
								entry.NodeId
							);
			for (const auto &point : points)
				if (!detail::MeshFinite(point.Position) || !detail::MeshFinite(point.Previous) ||
					!detail::MeshFinite(point.BeforePrevious) || !detail::MeshFinite(point.UV) ||
					point.SourceIndex >= Limits::MaximumArrayElements || !std::isfinite(point.Drag) ||
					!detail::MeshFinite(point.Original) || !detail::MeshFinite(point.VelocityReference) ||
					(point.DrawPosition && !detail::MeshFinite(*point.DrawPosition)))
					return refuse(
						Status::InvalidValue, "simulation replay point must be finite", entry.NodeId
					);
			for (const auto &edge : edges)
				if (edge.First >= points.size() || edge.Second >= points.size() || edge.PreviousEdge < -1 ||
					edge.NextEdge < -1 ||
					(edge.PreviousEdge >= 0 && uint64_t(edge.PreviousEdge) >= edges.size()) ||
					(edge.NextEdge >= 0 && uint64_t(edge.NextEdge) >= edges.size()) ||
					!std::isfinite(edge.Distance) || !std::isfinite(edge.Flexibility) ||
					!std::isfinite(edge.DirectionDegrees) || !std::isfinite(edge.AngularDrag))
					return refuse(Status::InvalidValue, "simulation replay edge is invalid", entry.NodeId);
		}
		return Status::Ok;
	}
}
