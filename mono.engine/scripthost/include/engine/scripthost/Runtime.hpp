#pragma once

// Picking a VM.
//
// **Two hundred lines, one decision, and it is the only place both adapters are
// named.** `scriptluau` and `scriptjs` each sit at L10 above `script` and know
// nothing of each other; a module that named one of them from inside the other
// would be a lateral edge and would also make one language the host of the
// other, which is exactly what "two languages, two VMs, one binding surface"
// refuses.
//
// So the choice lives one layer up, on its own, and every program links this
// rather than either adapter.
//
// @tier L11 · shared
// @since v0.19

#include <engine/script/Language.hpp>
#include <engine/script/Runtime.hpp>

#include <memory>

namespace engine::ecs {
	class Store;
}

namespace engine::script {

	// Opens a VM of the given language over `store`.
	//
	// @param store    The world scripts create instances in. Outlives the result.
	// @param language Which VM.
	// @param limits   What bounds a script.
	// @return The runtime.
	std::unique_ptr<Runtime>
	MakeRuntime(ecs::Store &store, Language language, const RuntimeLimits &limits = {});
}
