#pragma once

// An explicit modal scope modifier. Its parent element is the scope root.
//
// @tier L7 · shared

namespace engine::gui {
	struct ModalScope {
		bool Enabled = true;
	};

	void RegisterModalComponents();
}
