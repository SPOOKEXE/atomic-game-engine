#pragma once

#include "EvaluationBudget.hpp"

#include <engine/imagegraph/Document.hpp>

#include <algorithm>
#include <array>
#include <limits>

namespace engine::imagegraph::detail {
	inline bool IsSourceRigidActor(std::string_view type) {
		constexpr std::array<std::string_view, 17> types{
			"pc.rigid_activate",
			"pc.rigid_explode",
			"pc.rigid_force_apply",
			"pc.rigid_fracture",
			"pc.rigid_joint_fix",
			"pc.rigid_joint_rotate",
			"pc.rigid_object",
			"pc.rigid_object_get_collision",
			"pc.rigid_object_segment",
			"pc.rigid_object_spawner",
			"pc.rigid_override",
			"pc.rigid_path_collider",
			"pc.rigid_render",
			"pc.rigid_render_id",
			"pc.rigid_sensor",
			"pc.rigid_variable",
			"pc.rigid_wall"
		};
		return std::find(types.begin(), types.end(), type) != types.end();
	}

	// Native ordering follows the already validated topology. Prefix routes retain
	// disconnected actors without moving effects across a render or an observation.
	inline Status AppendRigidSchedule(
		const Document &document,
		Plan &compiled,
		EvaluationBudget &budget,
		AllocationReservation &planCharge,
		Diagnostic &diagnostic
	) {
		constexpr size_t absent = std::numeric_limits<size_t>::max();
		const auto ownerOf = [&](size_t actor) {
			if (!IsSourceRigidActor(document.Nodes[actor].Type)) return absent;
			for (const auto &route : compiled.InlineOwnerDependencies)
				if (route.Consumer == actor && !route.ControlsOnly &&
					document.Nodes[route.Owner].Type == "pc.rigid_group_inline")
					return route.Owner;
			return absent;
		};
		bool any = false;
		for (const size_t index : compiled.NodeOrder)
			if (ownerOf(index) != absent) {
				any = true;
				break;
			}
		if (!any) return Status::Ok;
		auto scratchCharge = budget.Reserve(document.Nodes.size() * sizeof(size_t));
		if (!scratchCharge) {
			diagnostic = {Status::LimitExceeded, {}, {}, "rigid schedule exceeds its workspace byte bound"};
			return diagnostic.Code;
		}
		std::vector<size_t> previous(document.Nodes.size(), absent);
		size_t additions = 0;
		const auto visit = [&](auto &&append) {
			std::fill(previous.begin(), previous.end(), absent);
			for (const size_t actor : compiled.NodeOrder) {
				const size_t owner = ownerOf(actor);
				if (owner == absent) continue;
				const size_t before = previous[owner];
				previous[owner] = actor;
				if (before == absent) continue;
				const InlineControlDependency edge{actor, before};
				if (std::find(
						compiled.InlineControlDependencies.begin(),
						compiled.InlineControlDependencies.end(),
						edge
					) == compiled.InlineControlDependencies.end())
					append(edge);
			}
		};
		visit([&](InlineControlDependency) { ++additions; });
		const size_t existing = compiled.EffectiveLinks.size() + compiled.GroupSurfaceDependencies.size() +
								compiled.InlineOwnerDependencies.size() +
								compiled.InlineControlDependencies.size() +
								compiled.PcxNamedDependencies.size();
		if (additions > Limits::MaximumLinks || (additions && existing > Limits::MaximumLinks - additions)) {
			diagnostic = {Status::LimitExceeded, {}, {}, "rigid schedule exceeds the graph route bound"};
			return diagnostic.Code;
		}
		if (!additions) return Status::Ok;
		const size_t required = compiled.InlineControlDependencies.size() + additions;
		if (required > compiled.InlineControlDependencies.capacity()) {
			// Charge replacement capacity while the old allocation is still live. Its
			// existing charge remains conservative until the enclosing compile
			// candidate is destroyed.
			auto storage = budget.Reserve(required * sizeof(InlineControlDependency));
			if (!storage || !planCharge.Merge(std::move(*storage))) {
				diagnostic = {Status::LimitExceeded, {}, {}, "rigid routes exceed their live byte bound"};
				return diagnostic.Code;
			}
			compiled.InlineControlDependencies.reserve(required);
		}
		visit([&](InlineControlDependency edge) { compiled.InlineControlDependencies.push_back(edge); });
		return Status::Ok;
	}
} // namespace engine::imagegraph::detail
