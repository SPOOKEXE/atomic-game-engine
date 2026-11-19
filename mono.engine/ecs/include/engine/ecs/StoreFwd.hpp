#pragma once

// Names `ecs::Store` for headers that only pass one by reference or pointer.
//
// `Store.hpp` brings in columns, the component registry, the change channel and
// the job system, so a header that includes it only to name the type in a
// signature makes every file that includes the header rebuild whenever any of
// those change. Such a header includes this instead, and the source file that
// calls into the store includes `Store.hpp` itself.
//
// @tier L3 · shared

#include <engine/ecs/Entity.hpp>

namespace engine::ecs {
	class Store;
}
