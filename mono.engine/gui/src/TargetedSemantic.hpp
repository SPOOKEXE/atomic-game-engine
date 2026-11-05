#pragma once

#include "PointerFrameFacts.hpp"

#include <engine/ecs/Store.hpp>

namespace engine::gui::detail {

	// The caller validates Router/Input continuity and projects SyncModals before
	// evaluating the two modal permissions. The Store and frame must remain stable
	// until the corresponding UpdateSemantic call finishes.
	template <class CollectorOf, class IsButton>
	bool MatchesTargetedSemantic(
		const ecs::Store &store,
		const DrawList &list,
		const PointerFrameFacts &facts,
		ecs::Entity target,
		ecs::Entity expectedCollector,
		SemanticAction action,
		bool liveModalAllowed,
		bool frameModalAllowed,
		CollectorOf &&collectorOf,
		IsButton &&isButton
	) {
		if (!facts.Valid || facts.StoreIdentity != store.Identity() || target == ecs::NULL_ENTITY ||
			(action != SemanticAction::Activate && action != SemanticAction::Focus))
			return false;

		const DrawCommand *liveCommand = nullptr;
		for (const DrawCommand &command : list.Commands) {
			if (command.Source == target && command.Bounds.Intersects(command.Clip)) {
				liveCommand = &command;
				break;
			}
		}
		const PointerFrameCommand *frameCommand = nullptr;
		for (const PointerFrameCommand &command : facts.Commands) {
			if (command.Source == target && command.Bounds.Intersects(command.Clip)) {
				frameCommand = &command;
				break;
			}
		}
		if (liveCommand == nullptr || frameCommand == nullptr ||
			liveCommand->Collector != expectedCollector || frameCommand->Collector != expectedCollector ||
			collectorOf(target) != expectedCollector || facts.Collector(target) != expectedCollector ||
			!liveModalAllowed || !frameModalAllowed)
			return false;

		const PointerFrameNode *node = facts.Find(target);
		const Element *element = store.Get<Element>(target);
		if (!store.Alive(target) || node == nullptr || !node->Alive || element == nullptr ||
			!node->ElementValue || !element->Visible || !element->Interactable ||
			!node->ElementValue->Visible || !node->ElementValue->Interactable)
			return false;

		if (action == SemanticAction::Activate) {
			return isButton(target) && node->GuiButton && element->Selectable &&
				   node->ElementValue->Selectable;
		}
		const Entry *entry = store.Get<Entry>(target);
		if ((entry != nullptr) != node->TextEntry) return false;
		if (entry == nullptr) return element->Selectable && node->ElementValue->Selectable;

		const Label *label = store.Get<Label>(target);
		return entry->ClearTextOnFocus == node->EntryClearOnFocus &&
			   (label != nullptr) == node->EntryHasLabel &&
			   (label == nullptr || label->Text == node->EntryText);
	}

} // namespace engine::gui::detail
