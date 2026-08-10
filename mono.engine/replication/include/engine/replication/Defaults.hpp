#pragma once

// What a scene built out of `Engine::scene` replicates, in one place.
//
// **This was written out three times and one copy had already drifted.**
// `DEFERRED.md` D00018 recorded the table appearing in `mono.server`,
// `mono.unified_server_client` and `mono.studio`, and said all three agreed —
// "which is the only reason this is an entry and not a bug". By v0.13 that was
// true of two of them: `mono.server` and `mono.studio` still matched row for
// row, and the diagnostic harness was missing `scene.Camera`,
// `scene.SurfaceCamera` and `ecs.Hierarchy`.
//
// **The harness is the copy whose comment says it exists to make disagreement
// observable**, and it was the one that disagreed. Nothing observed it: the
// three rows it lacks are a mirror, a lens and a parent link, so a diagnostic
// meant to mirror the server's behaviour quietly stopped exercising reflections
// at all. That is the entry's own prediction — nothing in the build compares the
// copies — arriving in the least useful place to have it.
//
// Two of three agreeing is not the entry being wrong; it is the entry being
// half-lucky, and the half that was not lucky is the one nobody would look at.
//
// ## Why it is here, against the entry's objection
//
// D00018 considered this and refused it: putting the table in `replication`
// "makes a module that must not know what a component *is* name four of them,
// which is the property that keeps `net` and `replication` separable at all".
//
// **The objection is answered by what actually crosses.** This names components
// the way the wire does — as *strings* — and `Authority::Replicate` already
// takes a `core::Name`. So this module gains no include of `scene`, no link
// against it, and no knowledge of what a `Transform` contains. It is a default
// policy expressed in the vocabulary `replication` already speaks, not a
// dependency on the module that owns the types.
//
// **What it is not is the real answer, and that has not changed.** The entry's
// own conclusion stands: a `<Replicated>` section in the game document is a
// per-game declaration read by whoever loads it, which deletes the table rather
// than moving it — and is the only version that also lets a game replicate a
// component `scene` does not own. That is now *cheaper* than it was, because
// there is one caller to change instead of three.
//
// @tier L12 · shared

#include <engine/core/Name.hpp>
#include <engine/replication/Authority.hpp>

#include <span>
#include <string_view>

namespace engine::replication {

	// One component and how the authority notices it changing.
	//
	// @since v0.13
	struct ReplicatedComponent {
		// The registered component name, as `ecs::Components` spells it.
		std::string_view Name;

		// How a change is found.
		//
		// **The pairing is not arbitrary and that is what made drift
		// expensive.** A `Transform` is written every tick by a system, so the
		// dirty bits already know and hashing it would be a pass over the world
		// to learn what was free. `Bounds` and `Visual` are written once by a
		// script and then never, so observing them buys a dirty column paid for
		// every tick and read never — and *not* signing them is the bug v0.7
		// fixed, where a part recoloured by a script kept its old colour on
		// every client for ever.
		//
		// Getting one entry wrong is silent in both directions: the wrong
		// detector sends nothing and reports nothing.
		ChangeDetection Detection = ChangeDetection::Signature;
	};

	// The components a `scene`-built world replicates.
	//
	// **A default rather than a rule.** A host declares these and may declare
	// more; nothing here forces the set on anybody, which is what keeps
	// `Authority::Replicate` opt-in.
	//
	// @return The table, valid for the lifetime of the process.
	// @since v0.13
	std::span<const ReplicatedComponent> DefaultReplicatedComponents();
}
