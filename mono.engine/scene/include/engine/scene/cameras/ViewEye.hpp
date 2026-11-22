#pragma once

// A frame-local view of a camera without changing its authored transform.
//
// @tier L7 · shared

#include <engine/core/types/CFrame.hpp>
#include <engine/ecs/Entity.hpp>

namespace engine::ecs {
	class Store;
}

namespace engine::scene {
	// A temporary view of one camera, used only while presenting a world.
	// The camera instance remains the owner of its authored transform and lens.
	// A null camera disables the override. The resource is cleared on restore.
	struct ViewEye {
		// The camera whose presentation uses this frame.
		ecs::Entity Camera;
		// The frame presented for that camera.
		core::CFrame Frame;
	};

	// Resolves the temporary view when it names this camera, otherwise its transform.
	//
	// @param store The world containing the camera and any temporary view.
	// @param camera The camera to resolve.
	// @param[out] frame The resolved frame when one exists.
	// @return `false` when the camera has no live transform.
	bool ResolveViewEye(const ecs::Store &store, ecs::Entity camera, core::CFrame &frame);
}
