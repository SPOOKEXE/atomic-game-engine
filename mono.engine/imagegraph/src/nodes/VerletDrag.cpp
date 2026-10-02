#include "VerletNodes.hpp"

#include <cmath>
#include <numbers>

namespace engine::imagegraph::detail {
	bool VerletDragMesh(NodeContext &context) {
		const Value *input = context.Find("mesh");
		const auto *mesh = input ? std::get_if<MeshValue2D>(input) : nullptr;
		if (!mesh || !mesh->Data) return PublishVerletMesh(context, {});
		if (const auto preserved = PreserveVerletForCacheAction(context, *mesh)) return *preserved;
		if (!ValidMesh2DPayload(*mesh))
			return context.Fail(Status::InvalidValue, "drag input mesh is invalid", "mesh");
		if (!context.Boolean("active", true) || !mesh->Data->Verlet) return PublishVerletMesh(context, *mesh);
		VerletDragReplay state;
		const SimulationReplayEntry *previous = nullptr;
		if (context.CurrentSimulation)
			for (const auto &entry : context.CurrentSimulation->Entries)
				if (entry.NodeId == context.Authored.Id && entry.ProcessorRow == context.ProcessorRow) {
					previous = &entry;
					break;
				}
		if (previous) {
			if (!previous->Drag ||
				previous->State.AuthoringRevision != context.Request.SimulationAuthoringRevision)
				return context.Fail(
					Status::InvalidValue, "drag replay requires matching node kind and revision", "drag"
				);
			state = *previous->Drag;
		} else if (context.Request.Tick != 0) {
			return context.Fail(Status::InvalidValue, "drag seek requires reset and replay", "drag");
		}
		Vector2 drag = context.Vec2("drag");
		if (!context.IsLinked("drag") && context.Integer("drag_unit", 1) == 1) {
			const Vector2 dimension = VerletScopeDimension(context);
			drag.X *= dimension.X;
			drag.Y *= dimension.Y;
		}
		const double rotation = context.Scalar("rotation");
		const bool autoAnchor = context.Boolean("auto_anchor", true);
		const Vector2 authoredAnchor = context.Vec2("anchor");
		if (context.FailureCode != Status::Ok) return false;
		if (!context.ReserveOutput(Mesh2DStorageBytes<false>(*mesh), "mesh")) return false;
		MeshValue2D output = *mesh;
		auto &points = output.Data->Simulation.Points;
		Vector2 delta = drag;
		double angle = rotation;
		if (context.Request.Tick == 0) {
			Vector2 center{};
			size_t count = 0;
			for (auto &point : points) {
				if (point.Pin) {
					center.X += point.Position.X;
					center.Y += point.Position.Y;
					++count;
				}
				point.Position.X += delta.X;
				point.Position.Y += delta.Y;
			}
			state.Move = {};
			state.Anchor.reset();
			if (count) state.Anchor = Vector2{center.X / count, center.Y / count};
		} else {
			delta = {drag.X - state.Previous.X, drag.Y - state.Previous.Y};
			angle = rotation - state.PreviousAngle;
			state.Move.X += delta.X;
			state.Move.Y += delta.Y;
			for (auto &point : points)
				if (point.Pin) {
					point.Position.X += delta.X;
					point.Position.Y += delta.Y;
				}
		}
		if (angle != 0) {
			if (autoAnchor && !state.Anchor)
				return context.Fail(
					Status::InvalidValue, "source automatic drag rotation has no pinned anchor", "anchor"
				);
			Vector2 anchor = autoAnchor ? *state.Anchor : authoredAnchor;
			anchor.X += state.Move.X;
			anchor.Y += state.Move.Y;
			for (auto &point : points) {
				if (context.Request.Tick != 0 && !point.Pin) continue;
				const double dx = point.Position.X - anchor.X, dy = point.Position.Y - anchor.Y;
				const double distance = std::hypot(dx, dy);
				const double direction = std::atan2(-dy, dx) + angle * std::numbers::pi / 180;
				point.Position = {
					anchor.X + distance * std::cos(direction), anchor.Y - distance * std::sin(direction)
				};
			}
		}
		if (context.Request.Tick == 0)
			for (auto &point : points)
				point.Previous = point.Position;
		state.Previous = drag;
		state.PreviousAngle = rotation;
		if (!MeshFinite(state.Move) || !MeshFinite(state.Previous) ||
			(state.Anchor && !MeshFinite(*state.Anchor)))
			return context.Fail(
				Status::InvalidValue, "drag controls produced nonfinite replay state", "drag"
			);
		if (!PublishVerletMesh(context, std::move(output))) return false;
		const uint64_t bytes =
			sizeof(SimulationReplayEntry) + std::max(context.Authored.Id.size(), std::string{}.capacity());
		if (!context.ReserveOutput(bytes, "drag")) return false;
		auto overlap = context.ReserveWorkspace(
			context.SimulationUpdates.capacity() * sizeof(SimulationReplayEntry), "drag"
		);
		if (!overlap) return false;
		context.SimulationUpdates.reserve(context.SimulationUpdates.size() + 1);
		SimulationReplayEntry update;
		update.NodeId = context.Authored.Id;
		update.ProcessorRow = context.ProcessorRow;
		update.State.Tick = context.Request.Tick;
		update.State.AuthoringRevision = context.Request.SimulationAuthoringRevision;
		update.State.Initialized = true;
		update.Drag = state;
		context.SimulationUpdates.push_back(std::move(update));
		return true;
	}
}
