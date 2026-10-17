#pragma once

// An explicit modal scope modifier. Its parent element is the scope root.
//
// @tier L7 · shared

namespace engine::gui {
	// Restricts input to the parent element's modal scope.
	struct ModalScope {
		// Whether this scope currently intercepts input.
		bool Enabled = true;
	};

	// Registers the modal scope component under its stable schema name.
	void RegisterModalComponents();
}
