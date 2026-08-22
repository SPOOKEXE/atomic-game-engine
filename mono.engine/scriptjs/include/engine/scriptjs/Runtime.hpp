#pragma once

// Opening a QuickJS VM over a world.
//
// **`scriptluau`'s twin, and deliberately the same shape.** The point of two VMs
// over one binding surface is that neither language is the real one, so the two
// adapters are two modules of equal standing above `script` rather than one
// hosting the other.
//
// A caller that wants "a runtime of whichever language this file is written in"
// wants `engine/scripthost/Runtime.hpp` instead.
//
// @tier L10 · shared
// @since v0.19

#include <engine/script/Runtime.hpp>

#include <memory>

namespace engine::ecs {
	class Store;
}

namespace engine::script {

	// Opens a QuickJS VM over `store`.
	//
	// @param store  The world scripts create instances in. Outlives the result.
	// @param limits What bounds a script.
	// @return The runtime.
	std::unique_ptr<Runtime> MakeJavaScriptRuntime(ecs::Store &store, const RuntimeLimits &limits = {});
}
