#pragma once

// Tags: a name outside the process, a bit inside it.
//
// `ROADMAP.md` v0.9 asks for tagging so the render pipeline can filter - "a
// redirected pipeline for tagged objects". This is that mechanism, and it is
// `SurfaceTable`'s shape almost exactly: the world holds a table mapping names
// to rows, a component holds the row, and the hot path never sees a string.
//
// **Thirty-two tags per world, and the ceiling is deliberate.** A mask on a
// draw instance is what makes "does this surface draw this thing" an `and`
// rather than a set lookup per instance per view, and a wider mask is bytes on
// every drawable row. Roblox's `CollectionService` has no such limit and pays
// for it with a hash lookup; this is a rendering filter rather than a general
// grouping service, and thirty-two is far past what a scene divides itself
// into. Registering a thirty-third fails rather than silently aliasing onto an
// existing bit - an alias would mean one tag's objects appearing in another's
// pass, which is invisible until somebody notices the wrong thing reflected.
//
// @tier L7 · shared

#include <engine/core/Name.hpp>
#include <engine/ecs/Entity.hpp>
#include <engine/scene/Components.hpp>

#include <cstdint>
#include <vector>

namespace engine::ecs {
	class Store;
}

namespace engine::scene {

	// The names a world's tag bits stand for.
	//
	// A resource, for `SurfaceTable`'s reason: it is one table per world rather
	// than a fact repeated on every row.
	//
	// @since v0.9
	struct TagTable {
		// How many tags one world may have.
		static constexpr size_t MAXIMUM = 32;

		// The registered names, in registration order. A tag's bit is its index
		// in this list.
		//
		// **Order is registration order and never sorted**, because the index
		// *is* the bit: sorting would renumber every mask already stored on a
		// row. `SurfaceTable::Rows` carries the same rule for the same reason.
		std::vector<core::Name> Names;

		// The bit a name stands for, registering it if it is new.
		//
		// @param name The tag.
		// @return The mask with that one bit set, or zero when the table is
		//         full or the name is invalid.
		uint32_t Register(const core::Name &name);

		// The bit a name stands for, without registering it.
		//
		// @param name The tag.
		// @return The mask, or zero when the name is not in the table.
		uint32_t Find(const core::Name &name) const;

		// Every registered name whose bit is set in a mask.
		//
		// For a save file and a properties panel - the two places a mask has to
		// become text again.
		//
		// @param mask The mask.
		// @return The names, in bit order.
		std::vector<core::Name> Describe(uint32_t mask) const;
	};

	// The world's tag table, creating it on first use.
	//
	// @param store The world.
	// @return The table.
	TagTable &TagsOf(ecs::Store &store);

	// Adds a tag to an entity, registering the name if the world has not seen
	// it.
	//
	// @param store  The world.
	// @param entity The entity. Must have a `Tags` component; every `BasePart`
	//               does.
	// @param name   The tag.
	// @return `false` when the entity has no `Tags` component or the table is
	//         full.
	bool AddTag(ecs::Store &store, ecs::Entity entity, const core::Name &name);

	// Removes a tag from an entity.
	//
	// **The name stays in the table.** Removing it would free a bit and
	// renumber nothing, so a mask stored on another row would keep a bit whose
	// meaning had changed - the alias failure this file exists to avoid,
	// arriving through the back door.
	//
	// @param store  The world.
	// @param entity The entity.
	// @param name   The tag.
	// @return `false` when the entity has no `Tags` component.
	bool RemoveTag(ecs::Store &store, ecs::Entity entity, const core::Name &name);

	// Whether an entity carries a tag.
	//
	// @param store  The world.
	// @param entity The entity.
	// @param name   The tag.
	// @return `true` when the entity has the tag.
	bool HasTag(const ecs::Store &store, ecs::Entity entity, const core::Name &name);

	// Whether a mask satisfies a filter.
	//
	// **An empty filter matches everything**, which is what makes tag filtering
	// free for every scene that does not use it: a surface camera with no
	// filter draws the world, and one with a filter draws its group.
	//
	// @param tags   The instance's mask.
	// @param filter The filter's mask.
	// @return `true` when the filter is empty or the two share a bit.
	constexpr bool MatchesTags(uint32_t tags, uint32_t filter) {
		return filter == 0 || (tags & filter) != 0;
	}
}
