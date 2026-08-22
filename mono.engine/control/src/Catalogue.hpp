#pragma once

// The component table, built once and answered twice.
//
// **Two surfaces want the same fact and neither may hold its own copy.**
// `engine_components` is a tool a model calls; `atomic://components` is a
// resource a client attaches without being asked. A second walk of
// `ecs::Components` would be two descriptions of one registry that agree until
// somebody adds a field to one of them, which is exactly the duplication rule 2
// calls the expensive kind.
//
// Private to this module: it names `nlohmann::json`, and `Surface.hpp` carries
// only the forward declaration.

#include <nlohmann/json_fwd.hpp>

namespace engine::control {

	// Every component type this process registers, sorted by name.
	//
	// @return An object with `components` and `count`.
	nlohmann::json ComponentCatalogue();
}
