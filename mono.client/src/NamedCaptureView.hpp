#pragma once

#include <engine/render/WorldView.hpp>

namespace client {
	// Rebuilds camera-dependent presentation data after a capture selected a
	// named camera. The caller commits the view only when this binding succeeds.
	template <class Abort>
	bool BindNamedCaptureWorldView(
		engine::ecs::Store &store,
		const engine::render::WorldViewBinding &binding,
		const engine::core::Vector2 &extent,
		engine::render::View &view,
		engine::render::WorldViewFrame &frame,
		engine::render::WorldCameraFrame &camera,
		Abort &&abort
	) {
		engine::render::CollectWorldView(store, view.WorldName, frame);
		engine::render::CollectWorldCamera(store, view, extent, camera);
		if (engine::render::BindWorldView(frame, camera, binding, view)) return true;
		abort(view);
		return false;
	}
}
