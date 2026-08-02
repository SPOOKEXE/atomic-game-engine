#pragma once

// The binary form of a world, and the only thing that knows it.
//
// Three operations over one format: write a world out, replace a world with
// one, and merge one into a world that is already running. They are together
// because they are one format — a reader that drifts from its writer is the
// failure this codec exists to make impossible, and the two halves are only
// obviously in step while they are next to each other.
//
// **Component types are recorded by name, never by id.** Ids are assigned in
// interning order and that order is a property of a process rather than of a
// world, so a snapshot keyed by them would restore into the wrong columns in
// the next process to read it — which is every consumer there is: a restart
// after a crash, a world moving between hosts, a recording replayed by a later
// build. Names cost a table at the front and nothing per row.
//
// **The entity directory is reproduced exactly**, index and generation alike,
// rather than re-allocated in order. A component may hold an `Entity` — a
// parent, a target, an owner — and those handles only survive if the directory
// that issued them comes back unchanged.
//
// **The bytes are stable.** Two saves of the same world produce the same
// bytes, and a save after a load produces them again, which is what lets a
// recording be compared and a determinism job mean anything. Everything read
// out of an unordered container is therefore sorted before it is written.
//
// Private, because the format is not an API. `Store` exposes the three
// operations; nothing outside this module names the layout they agree on.
//
// @tier L3 · shared

#include "StoreState.hpp"

#include <engine/core/Bytes.hpp>
#include <engine/ecs/Enums.hpp>

#include <string>
#include <string_view>

namespace engine::ecs {

	// Writes the whole world: entities, tables, resources, names, clock.
	//
	// @param state  The world to write.
	// @param name   The world's diagnostic name, which is carried in the
	//               snapshot and used in the refusal messages.
	// @param writer The writer to append to.
	// @return `false` when the world holds a component with no serialisation,
	//         which is refused rather than written as bytes nothing can read
	//         back.
	bool SaveSnapshot(const StoreState &state, std::string_view name, core::ByteWriter &writer);

	// Replaces a world's entire contents with a snapshot.
	//
	// On any failure the world is left **empty** rather than half-restored: a
	// world that is partly one snapshot and partly another is worse than no
	// world at all, because it looks like it works.
	//
	// @param[in,out] state  The world to overwrite.
	// @param[in,out] name   The world's diagnostic name. Replaced by the one the
	//                       snapshot carries, and used in messages until then.
	// @param[in]     reader The reader to consume.
	// @return `false` on a corrupt, truncated or wrong-version snapshot, or on
	//         one naming a component this build does not have.
	bool LoadSnapshot(StoreState &state, std::string &name, core::ByteReader &reader);

	// Merges a snapshot into a world that is already running.
	//
	// Entity handles are matched by index *and* generation, so an entity the
	// sender destroyed and recreated is a different entity here too rather than
	// the old one wearing new values.
	//
	// On failure the world is **left as it was** — not cleared, and not
	// half-merged. A replica that lost its world to a corrupt packet would be
	// worse off than one that ignored it.
	//
	// @param state  The live world to reconcile.
	// @param reader The snapshot to apply.
	// @param mode   What to do with entities the snapshot does not mention.
	// @return `false` when the snapshot could not be read, in which case
	//         nothing was touched.
	bool ApplySnapshot(StoreState &state, core::ByteReader &reader, ApplyMode mode);
}
