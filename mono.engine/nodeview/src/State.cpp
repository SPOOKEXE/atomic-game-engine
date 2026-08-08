#include <engine/nodeview/State.hpp>

namespace engine::nodeview {

	bool Click(
		CanvasState &state, const graph::PipelineLayout &layout, const CanvasStyle &style, float x, float y
	) {
		// **The pan is subtracted here and added when drawing**, which is the
		// one place the two signs have to agree. Getting it wrong moves the
		// canvas one way and the selection the other, and it only shows once
		// somebody has scrolled far enough to notice.
		const Pick pick = PickAt(layout, style, x - state.PanX, y - state.PanY);

		const core::Name was = state.Selected;
		state.Selected = pick.Hit ? pick.Name : core::Name{};
		return state.Selected != was;
	}

	void Pan(CanvasState &state, float dx, float dy) {
		state.PanX += dx;
		state.PanY += dy;
	}

	bool ClearSelection(CanvasState &state) {
		if (!state.Selected.IsValid()) {
			return false;
		}
		state.Selected = core::Name{};
		return true;
	}

	bool IsSelected(const CanvasState &state, core::Name name) {
		// **An invalid name is never selected**, rather than matching the "no
		// selection" value. A caller looping over nodes asks about each one, and
		// a node with no name would otherwise light up whenever nothing was
		// chosen.
		return name.IsValid() && state.Selected == name;
	}
}
