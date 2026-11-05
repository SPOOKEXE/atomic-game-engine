#pragma once

#include "PointerFrameFacts.hpp"

#include <engine/ecs/Store.hpp>
#include <engine/gui/Services.hpp>

namespace engine::gui::detail {
	// The caller validates refreshed input and router continuity first. The
	// Store and router must stay stable until Type applies this queued input.
	template <class CollectorOf>
	bool MatchesTypingFrame(
		const ecs::Store &store,
		const DrawList &list,
		const PointerFrameFacts &facts,
		CollectorOf &&collectorOf
	) {
		if (!facts.Valid || facts.StoreIdentity != store.Identity()) return false;
		const ecs::Entity focused = FocusedTextBox(store);
		if (focused == ecs::NULL_ENTITY) return true;
		const PointerFrameNode *node = facts.Find(focused);
		const Entry *entry = store.Get<Entry>(focused);
		if (!node || !node->Alive || !node->TextEntry || !entry ||
			entry->TextEditable != node->EntryEditable || entry->MultiLine != node->EntryMultiline)
			return false;
		if ((store.Get<Label>(focused) != nullptr) != node->EntryHasLabel) return false;
		const auto collector = collectorOf(focused);
		if (collector != facts.Collector(focused)) return false;
		bool liveCommand = false, savedCommand = false;
		for (const auto &command : list.Commands)
			if (command.Source == focused && command.Collector == collector) {
				liveCommand = true;
				break;
			}
		for (const auto &command : facts.Commands)
			if (command.Source == focused && command.Collector == collector) {
				savedCommand = true;
				break;
			}
		return liveCommand && savedCommand;
	}
}
