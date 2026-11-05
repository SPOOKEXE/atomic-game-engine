#pragma once

#include <engine/gui/Components.hpp>
#include <engine/gui/Services.hpp>

#include <cmath>
#include <limits>
#include <unordered_set>

namespace engine::gui::detail {
	struct NavigationTrace {
		ecs::Entity RejectedOverride;
		bool Scored = false;
		bool Override = false;
		float Score = 0.0f;
	};

	// Access supplies the ordered scoped commands and their live or retained facts.
	template <class Access>
	ecs::Entity ChooseNavigation(
		const Access &access,
		ecs::Entity current,
		bool autoSelect,
		SelectionMove move,
		NavigationTrace *trace = nullptr
	) {
		using core::Vector2;
		using ecs::Entity;
		using ecs::NULL_ENTITY;
		if (trace != nullptr) *trace = {};
		std::unordered_set<uint64_t> seen;
		Vector2 from;
		if (current == NULL_ENTITY || !access.Selectable(current) || !access.Centre(current, from)) {
			if (!autoSelect) return NULL_ENTITY;
			Entity seed;
			int32_t seedOrder = 0;
			for (size_t index = 0; index < access.CommandCount(); ++index) {
				const Entity source = access.CommandSource(index);
				if (!seen.insert(source.Id).second) continue;
				Vector2 centre;
				if (!access.Selectable(source) || !access.Centre(source, centre)) continue;
				const Selection *selection = access.SelectionOf(source);
				const int32_t order = selection != nullptr ? selection->Order : 0;
				if (seed == NULL_ENTITY || order < seedOrder) {
					seed = source;
					seedOrder = order;
				}
			}
			return seed;
		}
		if (const Selection *selection = access.SelectionOf(current)) {
			const Entity named = move == SelectionMove::Up	   ? selection->NextUp
								 : move == SelectionMove::Down ? selection->NextDown
								 : move == SelectionMove::Left ? selection->NextLeft
															   : selection->NextRight;
			Vector2 namedCentre;
			if (named != NULL_ENTITY && access.Selectable(named) && access.Centre(named, namedCentre)) {
				if (trace) trace->Override = true;
				return named;
			}
			if (trace) trace->RejectedOverride = named;
		}
		if (trace) trace->Scored = true;
		Entity best;
		float bestScore = std::numeric_limits<float>::max();
		for (size_t index = 0; index < access.CommandCount(); ++index) {
			const Entity candidate = access.CommandSource(index);
			if (candidate == current || !seen.insert(candidate.Id).second || !access.Selectable(candidate))
				continue;
			Vector2 to;
			if (!access.Centre(candidate, to)) continue;
			const float dx = to.X - from.X, dy = to.Y - from.Y;
			float along = 0.0f, across = 0.0f;
			switch (move) {
			case SelectionMove::Up:
				along = -dy;
				across = std::abs(dx);
				break;
			case SelectionMove::Down:
				along = dy;
				across = std::abs(dx);
				break;
			case SelectionMove::Left:
				along = -dx;
				across = std::abs(dy);
				break;
			case SelectionMove::Right:
				along = dx;
				across = std::abs(dy);
				break;
			}
			if (along <= 0.0f) continue;
			const float score = along + across * 0.125f;
			if (score < bestScore) {
				bestScore = score;
				best = candidate;
			}
		}
		if (trace) trace->Score = bestScore;
		return best;
	}
}
