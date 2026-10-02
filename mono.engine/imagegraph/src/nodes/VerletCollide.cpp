#include "../SourceVerletCollider.hpp"
#include "VerletNodes.hpp"
namespace engine::imagegraph::detail {
	bool VerletCollide(NodeContext &context) {
		if (context.InlineOwnerType != "pc.verlet_sim_inline" || context.InlineOwnerId.empty()) return true;
		const bool active = context.Boolean("active", true);
		VerletCollider geometry;
		geometry.Shape = context.Integer("shape", 0);
		geometry.Region =
			context.Get<Area>("area", {.CenterX = .5, .CenterY = .5, .HalfWidth = .2, .HalfHeight = .2});
		if (!context.IsLinked("area") && context.Integer("area_unit", 1) == 1) {
			const auto dimension = VerletScopeDimension(context);
			geometry.Region.CenterX *= dimension.X;
			geometry.Region.HalfWidth *= dimension.X;
			geometry.Region.CenterY *= dimension.Y;
			geometry.Region.HalfHeight *= dimension.Y;
		}
		if (active && !ValidSourceVerletCollider(geometry))
			return context.Fail(
				Status::InvalidValue, "source collider shape or finite ellipse denominator is invalid", "area"
			);
		const Value *input = context.Find("mesh");
		const auto *mesh = input ? std::get_if<MeshValue2D>(input) : nullptr;
		const uint64_t bytes = sizeof(SimulationReplayEntry) +
							   std::max(context.Authored.Id.size(), std::string{}.capacity()) +
							   std::max(context.InlineOwnerId.size(), std::string{}.capacity()) +
							   (mesh ? Mesh2DStorageBytes<true>(*mesh) : 0);
		if (!context.ReserveOutput(bytes, "mesh")) return false;
		SimulationReplayEntry update;
		update.NodeId = context.Authored.Id;
		update.ProcessorRow = context.ProcessorRow;
		update.State.Initialized = true;
		update.State.Tick = context.Request.Tick;
		update.State.AuthoringRevision = context.Request.SimulationAuthoringRevision;
		update.Collider = VerletColliderReplay{std::string(context.InlineOwnerId), geometry, active};
		context.SimulationUpdates.reserve(1);
		context.SimulationUpdates.push_back(std::move(update));
		if (mesh)
			context.SetValue("mesh", *mesh);
		else
			context.SetValue("mesh", UndefinedValue{});
		return context.FailureCode == Status::Ok;
	}
	bool ResolveVerletColliderControls(
		NodeContext &context, std::vector<VerletCollider> &colliders, AllocationReservation &charge
	) {
		if (context.SimulationColliderIds.empty()) return true;
		const auto *replay = context.CurrentSimulation;
		if (!replay)
			return context.Fail(
				Status::InvalidValue, "source collider controls require current evaluation state", "mesh"
			);
		if (context.SimulationColliderIds.size() > Limits::MaximumNodes ||
			replay->Entries.size() > Limits::MaximumNodes)
			return context.Fail(
				Status::LimitExceeded, "source collider control list exceeds bounded nodes", "mesh"
			);
		struct Ordered {
			size_t Order, Row;
			VerletCollider Geometry;
		};
		const uint64_t bytes = context.SimulationColliderIds.size() *
								   (sizeof(std::pair<std::string_view, size_t>) + sizeof(uint8_t)) +
							   replay->Entries.size() * (sizeof(Ordered) + sizeof(VerletCollider));
		auto admitted = context.ReserveWorkspace(bytes, "mesh");
		if (!admitted) return false;
		charge = std::move(*admitted);
		std::vector<std::pair<std::string_view, size_t>> ids;
		ids.reserve(context.SimulationColliderIds.size());
		for (size_t index = 0; index < context.SimulationColliderIds.size(); ++index)
			ids.emplace_back(context.SimulationColliderIds[index], index);
		std::sort(ids.begin(), ids.end());
		for (size_t index = 1; index < ids.size(); ++index)
			if (ids[index].first == ids[index - 1].first)
				return context.Fail(
					Status::DuplicateId, "source collider dependency repeats identity", "mesh"
				);
		std::vector<Ordered> ordered;
		ordered.reserve(replay->Entries.size());
		std::vector<uint8_t> seen(ids.size(), 0);
		// The compile list owns one ID per node; processor rows register separate colliders.
		for (const auto &entry : replay->Entries) {
			const auto found = std::lower_bound(
				ids.begin(), ids.end(), entry.NodeId, [](const auto &id, std::string_view name) {
					return id.first < name;
				}
			);
			if (found == ids.end() || found->first != entry.NodeId) continue;
			if (!entry.Collider || entry.Collider->OwnerNodeId != context.InlineOwnerId ||
				entry.State.Tick != context.Request.Tick ||
				entry.State.AuthoringRevision != context.Request.SimulationAuthoringRevision)
				return context.Fail(
					Status::InvalidValue,
					"source collider controls are stale or belong to another Inline owner",
					"mesh"
				);
			seen[found->second] = 1;
			if (entry.Collider->Active)
				ordered.push_back({found->second, entry.ProcessorRow, entry.Collider->Geometry});
		}
		if (std::find(seen.begin(), seen.end(), uint8_t{0}) != seen.end())
			return context.Fail(Status::InvalidValue, "source collider dependency was not evaluated", "mesh");
		std::sort(ordered.begin(), ordered.end(), [](const auto &a, const auto &b) {
			return std::tie(a.Order, a.Row) < std::tie(b.Order, b.Row);
		});
		colliders.reserve(ordered.size());
		for (const auto &item : ordered)
			colliders.push_back(item.Geometry);
		return true;
	}
}
