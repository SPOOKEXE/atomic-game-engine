#pragma once

// Viewport-local diagnostic state.
//
// This is editor session state, not an authored camera component. It stays out
// of snapshots and replication so Stop leaves both the game and the inspector
// unchanged.

#include <engine/core/types/CFrame.hpp>

namespace studio {

	// Editor-session diagnostics applied to one viewport panel.
	struct ViewportDiagnostics {
		// Whether bounded local-light influence probes are drawn over this panel.
		bool ShowLightInfluence = false;

		// Whether this panel submits its draw rows through a frozen frustum.
		bool FrustumLocked = false;
		// The pose captured when FrustumLocked was enabled or recaptured.
		engine::core::CFrame FrozenFrustum;

		// Freezes this panel's effective culling pose at inspectionFrame.
		void LockFrustum(const engine::core::CFrame &inspectionFrame) {
			FrozenFrustum = inspectionFrame;
			FrustumLocked = true;
		}

		// Returns the pose used for culling, while the caller keeps using its own
		// inspection frame for rendering and overlay projection.
		const engine::core::CFrame &EffectiveFrustum(const engine::core::CFrame &inspectionFrame) const {
			return FrustumLocked ? FrozenFrustum : inspectionFrame;
		}
	};
}
