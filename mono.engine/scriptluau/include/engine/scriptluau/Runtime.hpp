#pragma once

// Opening a Luau VM over a world.
//
// **The whole public surface of this module is one function**, and that is what
// the split bought. Everything a script can name - the class tree, the property
// surface, the services, the signals, the tick - is `script`'s and has no VM in
// it; what lives here is the part that meets a `lua_State *`, and nothing
// outside this directory ever holds one.
//
// A caller that wants "a runtime of whichever language this file is written in"
// wants `engine/scripthost/Runtime.hpp` instead. This header is for a caller
// that has already decided, and for that factory.
//
// @tier L10 · shared
// @since v0.19

#include <engine/script/Runtime.hpp>

#include <memory>

namespace engine::ecs {
	class Store;
}

namespace engine::script {

	// Opens a Luau VM over `store`.
	//
	// @param store  The world scripts create instances in. Outlives the result.
	// @param limits What bounds a script.
	// @return The runtime.
	std::unique_ptr<Runtime> MakeLuauRuntime(ecs::Store &store, const RuntimeLimits &limits = {});
}
