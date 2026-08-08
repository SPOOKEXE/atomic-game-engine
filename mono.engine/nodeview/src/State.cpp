#include <engine/nodeview/State.hpp>

namespace engine::nodeview {

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
