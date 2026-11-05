#pragma once

#include "PresentationFrameMetadata.hpp"

#include <engine/ecs/Store.hpp>
#include <engine/gui/InputContinuity.hpp>
#include <engine/render/Renderer.hpp>

#include <client/Accessibility.hpp>

namespace client {
	// The caller supplies the current list from its normal GUI compile and keeps
	// the borrowed frame stable without reentering Store or mutating the queue.
	inline bool MatchesAccessibilityOrigin(
		const AccessibilityAction &action,
		engine::ecs::Store &store,
		const engine::gui::DrawList &list,
		const engine::gui::Router &router,
		uint64_t routerToken,
		uint64_t runtimeIdentity,
		engine::render::SpatialSnapshotStatus status,
		uint64_t displayedSequence,
		bool legacyAllowed,
		const engine::gui::PointerFrame *frame,
		const PresentationFrameMetadata *metadata
	) {
		if (!IsCurrentAccessibilityAction(action, store.Identity(), list)) return false;
		if (action.FrameSequence == 0)
			return legacyAllowed && (status == engine::render::SpatialSnapshotStatus::Unsupported ||
									 (status == engine::render::SpatialSnapshotStatus::NotRequested &&
									  displayedSequence == 0));
		if (status != engine::render::SpatialSnapshotStatus::Ready ||
			action.FrameSequence != displayedSequence || frame == nullptr || metadata == nullptr ||
			!metadata->Semantics || metadata->Store != store.Identity() ||
			metadata->Runtime != runtimeIdentity || frame->InitialInputContinuity() == nullptr ||
			frame->InitialRouterContinuity() == nullptr)
			return false;
		return engine::gui::MatchesInputContinuity(store, *frame->InitialInputContinuity()) &&
			   router.MatchesRouterContinuity(store, *frame->InitialRouterContinuity(), routerToken) &&
			   router.MatchesTargetedSemanticFrame(
				   store,
				   list,
				   *frame,
				   *frame->InitialRouterContinuity(),
				   routerToken,
				   action.Target,
				   action.Collector,
				   action.Action
			   );
	}
}
