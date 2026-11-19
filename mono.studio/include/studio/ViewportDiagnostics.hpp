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

		// Whether this panel presents camera-dependent behaviour from a saved camera.
		bool FrustumLocked = false;
		// The pose captured when FrustumLocked was enabled or recaptured.
		engine::core::CFrame FrozenFrustum;
		// The lens captured with it, so the drawn marker keeps its shape while
		// the inspection camera moves or the panel resizes. Zero means unknown.
		//@{
		float FrozenFieldOfView = 0.0f;
		float FrozenAspectRatio = 0.0f;
		//@}

		// Saves this panel's virtual camera, pose and lens, from the inspection view.
		void LockFrustum(
			const engine::core::CFrame &inspectionFrame, float fieldOfView = 0.0f, float aspectRatio = 0.0f
		) {
			FrozenFrustum = inspectionFrame;
			FrozenFieldOfView = fieldOfView;
			FrozenAspectRatio = aspectRatio;
			FrustumLocked = true;
		}

		// Returns the pose used by camera-dependent behaviour. A locked virtual
		// camera holds its whole pose, so culling, LOD and lights stay where the
		// marker is drawn while the inspection camera flies around them.
		const engine::core::CFrame &EffectiveFrustum(const engine::core::CFrame &inspectionFrame) const {
			return FrustumLocked ? FrozenFrustum : inspectionFrame;
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
	};
}
