#pragma once

// Reaching the map's storage. Private to this module.
//
// `GridInternals`' argument, one structure across: the suite and the benchmark
// beside it are the only readers, neither is another module, and a set of
// public accessors would turn the layout into an API somebody outside could
// depend on. What a test needs here is the retained capacity - the claim that a
// rebuild over a steady scene allocates once - which is exactly the kind of
// fact a public header should not carry.

#include <engine/spatial/ChunkMap.hpp>

#include <cstddef>

namespace engine::spatial {

	// Reaches the map's storage, and nothing else.
	struct ChunkInternals {
		// How much member storage is retained. What a test observes to show that
		// a second rebuild reused the first's allocation.
		static size_t MemberCapacity(const ChunkMap &map) {
			return map.Members.capacity();
		}

		// Where the member storage lives. Unchanged across a rebuild means the
		// vector did not reallocate, which is the stronger half of the claim.
		static const void *MemberData(const ChunkMap &map) {
			return map.Members.data();
		}

		// How much placement scratch is retained. The sort buffer is the largest
		// allocation here and is the one most likely to be re-made by accident.
		static size_t PlacementCapacity(const ChunkMap &map) {
			return map.Placements.capacity();
		}
	};
}
