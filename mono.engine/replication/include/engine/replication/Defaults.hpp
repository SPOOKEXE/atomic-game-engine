#pragma once

// What a world replicates, in one place.
//
// **Three modules' components and one rule that decides between them.**
// `scene.` is what the world is, `gui.` is what the interface on it is, and
// five `script.` rows are what a script is - a path per language, which one is
// active, whether it is switched off, and the program itself. The rule is one
// sentence: **a row crosses when the authority decided it, and stays local when
// the machine looking at the world decided it.** A `TextLabel`'s words are the
// first; where that label landed on this screen, which box has this keyboard,
// and where this pointer is are the second.
//
// **This was written out three times and one copy had already drifted.**
// `DEFERRED.md` D00018 recorded the table appearing in `mono.server`,
// `mono.unified_server_client` and `mono.studio`, and said all three agreed -
// "which is the only reason this is an entry and not a bug". By v0.13 that was
// true of two of them: `mono.server` and `mono.studio` still matched row for
// row, and the diagnostic harness was missing `scene.Camera`,
// `scene.SurfaceCamera` and `ecs.Hierarchy`.
//
// **The harness is the copy whose comment says it exists to make disagreement
// observable**, and it was the one that disagreed. Nothing observed it: the
// three rows it lacks are a mirror, a lens and a parent link, so a diagnostic
// meant to mirror the server's behaviour quietly stopped exercising reflections
// at all. That is the entry's own prediction - nothing in the build compares the
// copies - arriving in the least useful place to have it.
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
// the way the wire does - as *strings* - and `Authority::Replicate` already
// takes a `core::Name`. So this module gains no include of `scene`, no link
// against it, and no knowledge of what a `Transform` contains. It is a default
// policy expressed in the vocabulary `replication` already speaks, not a
// dependency on the module that owns the types.
//
// **What it is not is the real answer, and that has not changed.** The entry's
// own conclusion stands: a `<Replicated>` section in the game document is a
// per-game declaration read by whoever loads it, which deletes the table rather
// than moving it - and is the only version that also lets a game replicate a
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
		// every tick and read never - and *not* signing them is the bug v0.7
		// fixed, where a part recoloured by a script kept its old colour on
		// every client for ever.
		//
		// Getting one entry wrong is silent in both directions: the wrong
		// detector sends nothing and reports nothing.
		//
		// **The third reason to observe something is not about how often it is
		// written.** A signature hashes the object representation, and a
		// `std::string`'s object representation is a pointer - so `gui.Label`,
		// `gui.Entry` and `script.Program` cannot be signed at all.
		// `Authority::Resign` declines a non-trivial component and says to
		// observe it instead, and `Authority::Survey` is what turns the
		// observation on: declaring the detector here is the whole of the
		// wiring, because a host that had to remember `Store::Observe` is a host
		// that can forget it.
		ChangeDetection Detection = ChangeDetection::Signature;

		// The component whose presence on an entity takes *this* component's
		// deltas off the wire for that entity, or empty for none.
		//
		// **Here rather than at each host, because there are two hosts.**
		// `server::Server` and `studio::PlayLink` both build an `Authority` by
		// walking this table, and a filter applied at one of them is a
		// difference between playing in the editor and playing for real - which
		// is the hardest kind of bug to see, because both look correct alone.
		//
		// Empty for every row but two, and the second one is not about
		// bandwidth. `gui.Label` is suppressed by `gui.Entry` because a
		// `TextBox` is a box somebody types into: `gui::Type` writes the text in
		// the replica, so the two ends are *meant* to disagree and an authority
		// going on offering its own copy would wipe a half-typed word. See
		// `Authority::SuppressWhenTagged`.
		//
		// @since v0.15
		std::string_view Suppressor;
	};

	// Whether a component is deliberately kept off the wire.
	//
	// **The list is short and every entry has a reason**, which is the whole
	// point of inverting this: a component added to `scene` tomorrow crosses
	// without anybody remembering to add it, and one that must *not* cross is a
	// decision somebody wrote down here.
	//
	// **The interface set added three at v0.15 and they are the same three
	// shapes the scene set already had**: a rectangle the local layout computes,
	// a canvas the local camera fits, and this machine's own keyboard focus. The
	// last is the one that would be *wrong* rather than wasteful - there is one
	// `gui.GuiServiceState` per world, so replicating it would have every client
	// writing one row about which box each of them is typing into.
	//
	// @param component The component's registered name.
	// @return `true` for a component the default set leaves out.
	// @since v0.13
	bool LocalToTheClient(std::string_view component);

	// The components a world replicates.
	//
	// **Everything the world holds, less the list above.** This was a hand-kept
	// allow-list of nine names and it was the wrong shape: a component added to
	// `scene` did not cross until somebody remembered, and "somebody remembered"
	// is exactly what three copies of this table had already failed at. The
	// question a host should answer is which components are *local*, and that is
	// a much shorter list than which are shared.
	//
	// **`Authority::Replicate` stays opt-in and this does not change that.** Its
	// argument - that a world holds state no client has business receiving, so a
	// default of everything makes leaking one the consequence of forgetting -
	// is answered by `LocalToTheClient` rather than overridden: the deciding is
	// still done, once, in a place with the reasons beside it.
	//
	// **Call it after components are registered.** It walks the registry, so a
	// host that asked before `RegisterSceneComponents`, `RegisterGuiClasses` and
	// `ScriptClass` would get whatever had been registered by then. Every caller
	// already registers at start-up and replicates when a world is built.
	//
	// @return The table, valid for the lifetime of the process.
	// @since v0.13
	std::span<const ReplicatedComponent> DefaultReplicatedComponents();
}
