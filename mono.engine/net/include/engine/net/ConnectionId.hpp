#pragma once

// A handle to one connection, and never a pointer to one.
//
// AGENTS.md rule 3: nothing crossing a world boundary is a pointer. A connection
// is exactly that kind of thing - a world publishes to it, a supervisor reports
// on it, a script asks it for statistics - and the moment one of those holds a
// `Connection *`, thread-per-world and process-per-world stop being
// interchangeable and the thing that ended it is invisible.
//
// So this is `ecs::Entity`'s shape, for `ecs::Entity`'s reason: an index into a
// dense table, plus a generation that makes a stale handle *detectably* stale
// rather than silently pointing at whoever took the slot next.
//
// **A reconnect is a new id.** The generation moves, and a caller holding the
// old one gets a refusal rather than the new player's connection. A handle that
// can come back to life is a handle every caller has to re-check after every
// await, and that is the check nobody writes.
//
// @tier L11 · shared

#include <compare>
#include <cstddef>
#include <cstdint>
// For std::hash. Specialising it without the primary template in scope is a
// wall of errors pointing at the standard library rather than at this file.
#include <functional>

namespace engine::net {

	// A connection, identified by slot and generation.
	//
	// @since v0.3
	struct ConnectionId {
		// Which slot in the host's table. Dense, so a host holding four hundred
		// connections iterates four hundred contiguous rows.
		uint32_t Index = 0;

		// How many times that slot has been used. Zero is never issued, so a
		// default-constructed id is not a valid one - see IsValid.
		uint32_t Generation = 0;

		// Whether this could name a live connection.
		//
		// Cheap and local: it says the handle was issued at all, not that the
		// connection is still up. The host answers the second question, because
		// only the host knows.
		bool IsValid() const {
			return Generation != 0;
		}

		// Whether two handles name the same connection at the same generation.
		bool operator==(const ConnectionId &other) const = default;

		// Ordering, so a handle can key a sorted container without a caller
		// inventing a comparator that disagrees with somebody else's.
		auto operator<=>(const ConnectionId &other) const = default;
	};
}

namespace std {

	// Hashing, so a ConnectionId keys an unordered container.
	//
	// The two fields are combined rather than added: a host that has issued many
	// generations of a few slots and one that has issued few generations of many
	// slots would otherwise collide constantly, and both are ordinary.
	template <> struct hash<engine::net::ConnectionId> {
		// @param id The handle to hash.
		// @return Its hash.
		size_t operator()(const engine::net::ConnectionId &id) const noexcept {
			return (static_cast<size_t>(id.Generation) << 32) ^ static_cast<size_t>(id.Index);
		}
	};
}
