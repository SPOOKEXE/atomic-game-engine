#pragma once

#include <engine/gui/Input.hpp>

#include <vector>

namespace studio {
	// The router reuses its event buffer, so retain earlier semantic events before
	// routing the pointer and append the new events while their span is valid.
	inline void AppendViewportPointerEvents(
		engine::gui::Router &router,
		engine::ecs::Store &store,
		const engine::gui::DrawList &list,
		const engine::gui::Pointer &pointer,
		std::vector<engine::gui::GuiEvent> &events
	) {
		const auto produced = router.Update(store, list, pointer);
		events.insert(events.end(), produced.begin(), produced.end());
	}
}
