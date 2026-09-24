#pragma once

// Viewport-local diagnostic state.
//
// This is editor session state, not an authored camera component. It stays out
// of snapshots and replication so Stop leaves both the game and the inspector
// unchanged.

#include <engine/core/types/CFrame.hpp>

#include <utility>

namespace engine::render {
	struct View;
}

namespace studio {

	// Editor-session diagnostics applied to one viewport panel.
	struct ViewportDiagnostics {
		// Whether bounded local-light influence probes are drawn over this panel.
		bool ShowLightInfluence = false;

		// Whether this panel presents camera-dependent behaviour from a saved position.
		bool FrustumLocked = false;
		// The position captured when FrustumLocked was enabled or recaptured.
		engine::core::CFrame FrozenFrustum;

		// Saves this panel's virtual camera position from inspectionFrame.
		void LockFrustum(const engine::core::CFrame &inspectionFrame) {
			FrozenFrustum = inspectionFrame;
			FrustumLocked = true;
		}

		// Returns the pose used by camera-dependent behaviour. The virtual camera
		// holds its position while retaining the live inspection direction, so a
		// user can turn to test a different culling direction without moving it.
		const engine::core::CFrame &EffectiveFrustum(const engine::core::CFrame &inspectionFrame) const {
			if (!FrustumLocked) {
				return inspectionFrame;
			}
			VirtualFrustum = engine::core::CFrame(FrozenFrustum.Position, inspectionFrame.Rotation());
			return VirtualFrustum;
		}

		// Resolves the behavior pose through the selected visual-world route while
		// leaving the inspection frame available for raster projection and picking.
		template <typename ResolveRoute>
		engine::core::CFrame ResolveBehaviourFrame(
			const engine::core::CFrame &inspectionFrame, ResolveRoute &&resolveRoute
		) const {
			engine::core::CFrame frame = EffectiveFrustum(inspectionFrame);
			std::forward<ResolveRoute>(resolveRoute)(frame);
			return frame;
		}

		// Keeps raster projection on the inspection pose and supplies the routed
		// behavior pose only when it differs.
		void ApplyCameraFrames(
			engine::render::View &view,
			const engine::core::CFrame &inspectionFrame,
			const engine::core::CFrame &behaviourFrame
		) const;

	  private:
		mutable engine::core::CFrame VirtualFrustum;
	};
}
