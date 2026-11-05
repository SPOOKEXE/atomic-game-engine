#pragma once
#include <engine/ecs/Entity.hpp>

#include <array>
#include <cstddef>
#include <span>

namespace engine::gui::detail {
	// Context exposes Collector, Modal, NextModal, FocusBeforeModal and
	// SelectionBeforeModal. Access supplies Known, Alive, Collector, Inside,
	// IsEntry and Selectable. Unknown retained entities fail closed.
	struct ModalProjection {
		ecs::Entity Focus, Selection;
		std::array<ecs::Entity, 64> FocusTargets{};
		size_t FocusCount = 0;
		bool Valid = true;
	};
	template <class Access, class Context>
	ModalProjection ProjectModalTransition(
		const Access &access,
		std::span<const Context> contexts,
		ecs::Entity focused,
		ecs::Entity selected,
		bool hasServiceState
	) {
		using ecs::NULL_ENTITY;
		ModalProjection out;
		out.Focus = focused;
		out.Selection = selected;
		if (!hasServiceState) {
			out.Focus = {};
			out.Selection = {};
			return out;
		}
		const auto known = [&](ecs::Entity entity) { return entity == NULL_ENTITY || access.Known(entity); };
		auto focus = [&](ecs::Entity target) {
			if (!known(out.Focus) || !known(target)) {
				out.Valid = false;
				return;
			}
			if (out.Focus == target) return;
			if (target != NULL_ENTITY && !access.IsEntry(target)) return;
			if (out.FocusCount == out.FocusTargets.size()) {
				out.Valid = false;
				return;
			}
			out.FocusTargets[out.FocusCount++] = target;
			out.Focus = target;
		};
		for (const auto &context : contexts) {
			const auto top = context.NextModal;
			if (top == context.Modal) continue;
			if (context.Modal != NULL_ENTITY && top == NULL_ENTITY) {
				const auto previousFocus = context.FocusBeforeModal;
				const auto previousSelection = context.SelectionBeforeModal;
				if (!known(previousFocus) || !known(previousSelection)) {
					out.Valid = false;
					return out;
				}
				if (previousFocus != NULL_ENTITY && access.Alive(previousFocus) &&
					access.Collector(previousFocus) == context.Collector && access.IsEntry(previousFocus))
					focus(previousFocus);
				if (previousSelection != NULL_ENTITY && access.Alive(previousSelection) &&
					access.Collector(previousSelection) == context.Collector &&
					access.Selectable(previousSelection))
					out.Selection = previousSelection;
			}
			if (!out.Valid) return out;
			if (top == NULL_ENTITY) continue;
			if (!known(out.Focus) || !known(out.Selection) || !known(top)) {
				out.Valid = false;
				return out;
			}
			if (out.Focus != NULL_ENTITY && access.Collector(out.Focus) == context.Collector &&
				!access.Inside(out.Focus, top))
				focus(NULL_ENTITY);
			if (out.Selection != NULL_ENTITY && access.Collector(out.Selection) == context.Collector &&
				!access.Inside(out.Selection, top))
				out.Selection = NULL_ENTITY;
			if (!out.Valid) return out;
		}
		return out;
	}
}
