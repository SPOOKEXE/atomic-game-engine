#pragma once

// What each named material feels like, once per world instead of once per row.
//
// `Surface` is a `core::Name` and this is what the name indexes. The point is
// the arithmetic: friction and restitution are the same two floats on every
// wooden crate in the world, so storing them per entity buys a column that is
// mostly duplicates and a load per body per contact. `ecs/AGENTS.md` calls this
// exact shape out - a shared fact stored per-entity belongs in a resource, and
// the loop reads it once.
//
// **There is deliberately no get-or-default.** `Find` returns `nullptr` for a
// material nobody registered, and the caller decides what that means. A silent
// default is a typo in a material name that behaves like working code for a
// month and then explains a bug nobody can reproduce.
//
// @tier L7 · shared

#include <engine/core/Name.hpp>

#include <cstddef>
#include <vector>

namespace engine::scene {

	// The contact coefficients for one material.
	//
	// @since v0.4
	struct SurfaceProperties {
		// Coulomb friction coefficient. Combined with the other body's by the
		// narrow phase, which is where the combining rule belongs - a table
		// that pre-combined pairs would be a table of every pair.
		float Friction = 0.5f;

		// Restitution, 0 for a dead stop and 1 for a lossless bounce.
		float Restitution = 0.0f;
	};

	// One row: the name, and what it resolves to.
	//
	// @since v0.4
	struct SurfaceRow {
		// The material's stable name, as a `Surface` component carries it.
		core::Name Material;

		// What that material feels like.
		SurfaceProperties Properties;
	};

	// The world's material table.
	//
	// A resource, so it is one per world and reached by name rather than by a
	// query. Stored as a flat vector in insertion order and searched linearly,
	// which is the boring option and the right one twice over: a world has tens
	// of materials and an integer compare over tens of rows beats a hash, and
	// insertion order is program order - so two runs of one scene hold an
	// identical table and a snapshot of it is byte-identical. A hash map would
	// give up the second property to improve a lookup nobody has measured.
	//
	// Revisit with a number attached, not on the strength of the word "linear".
	//
	// @since v0.4
	struct SurfaceTable {
		// The rows, in the order they were first set.
		//
		// Public because the serialiser this module registers walks it, and a
		// friend declaration to hide four bytes from a caller that could write
		// them through `Set` anyway is ceremony.
		std::vector<SurfaceRow> Rows;

		// Records what a material feels like, replacing any existing row.
		//
		// Replacing in place rather than appending, so a table set twice from a
		// reload holds one row per material rather than a history of them.
		//
		// @param material   The material's stable name.
		// @param properties What it feels like.
		void Set(core::Name material, const SurfaceProperties &properties);

		// What a material feels like, or nothing.
		//
		// @param material The name to resolve.
		// @return A pointer into `Rows`, invalidated by the next `Set`, or
		//         `nullptr` when the material was never registered.
		const SurfaceProperties *Find(core::Name material) const;

		// The number of registered materials.
		//
		// @return The current row count.
		size_t Count() const {
			return Rows.size();
		}
	};
}
