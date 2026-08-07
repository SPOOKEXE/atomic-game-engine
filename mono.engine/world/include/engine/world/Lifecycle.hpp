#pragma once

// When a world should stop ticking, and when it should start again.
//
// **This is a policy, and it existed in exactly one program.** `mono.studio`
// worked it out — a world idle for long enough suspends, a suspended world with
// something in its inbox resumes, and three exceptions that were each arrived at
// by being wrong first. `mono.server` has none of it: `--game FILE.agame` loads
// every world in the file and ticks all of them forever, and there is no
// `SetState` anywhere under `mono.server/src`.
//
// `docs/DEFERRED.md` D00017 is the entry, and its useful half is not that an
// orchestration module is missing. It is that **the risk here is a second copy**.
// This repository's most expensive recurring bug is one policy written twice —
// `CapturePreviousTransforms` was five lines in `examples` that the studio needed
// too, `ReadSource` exists so a source cache cannot be consulted from one entry
// point and not another, and there is deliberately one bus router so a world's
// behaviour does not change by being hosted elsewhere. A server that grew its own
// idle policy would make a world that closes in the editor and not on the server,
// with nothing reporting the difference.
//
// ## What is here and what is not
//
// **The decision, not the gathering.** Whether somebody is *looking* at a world
// is a question only an editor can answer, and whether a world is inside a scoped
// run is a `WorldRun` concept that means nothing to a server — so those arrive as
// facts in `LifecycleInputs` rather than being asked for here. What this owns is
// the part that must not differ between hosts: the thresholds, the ordering of
// the tests, and the three refusals.
//
// **Not placement.** Which host a world runs on, and what happens when that host
// dies, is the other half of D00017 and genuinely has no caller — building it now
// would be the guess that entry warns about. This is lifetime only.
//
// @tier L4 · server

#include <engine/world/Enums.hpp>

namespace engine::world {

	// What to do with one world this tick.
	//
	// @since v0.10
	enum class LifecycleAction : uint8_t {
		// Leave it as it is. The ordinary answer, and the one every refusal
		// below produces — a refusal here means "not this", never "do the
		// opposite".
		Leave,

		// Start ticking it again.
		Resume,

		// Stop ticking it.
		Suspend,
	};

	// What a host knows about one world when it asks.
	//
	// @since v0.10
	struct LifecycleInputs {
		// What the world is doing now.
		WorldState State = WorldState::Active;

		// Whether anything is using it.
		//
		// **A host's own question, and the reason this is an input.** For a
		// server that is a player standing in it; for the studio it is the
		// active scene, the player's world, *or a viewport panel showing it* —
		// and that last one was a real bug: with two panels open on two worlds,
		// the one that was not the active scene got closed under the panel
		// showing it, which looks exactly like the second viewport being broken.
		bool Occupied = false;

		// Whether something is sitting in its inbox.
		//
		// **Only meaningful while suspended, which is what makes it reliable.**
		// A running world replaces its inbox every barrier, so a message can
		// come and go between two frames; a suspended world is the one world
		// whose inbox nothing drains, so anything here is a teleport or a
		// message waiting on a world that is closed.
		bool InboxWaiting = false;

		// How long it has gone without occupancy, in seconds.
		double IdleSeconds = 0.0;

		// How long it may do so before being suspended.
		double IdleLimit = 300.0;

		// Whether suspending this one would leave the universe with nothing
		// ticking.
		bool LastWorld = false;
	};

	// Decides what to do with one world.
	//
	// **Pure, and that is what makes it testable rather than observable.** The
	// studio's version of this was reachable only by running an editor for five
	// minutes; every refusal below now has a case.
	//
	// The three refusals, in the order they are applied:
	//
	// - A world that is not `Active` or `Suspended` is left alone. `Faulted`
	//   belongs to the supervisor and `Idle` is already the reduced-rate state
	//   this would otherwise suspend out from under.
	// - An occupied world is never suspended, however long its idle clock says.
	//   Closing a world somebody is watching freezes it in front of them with no
	//   visible cause.
	// - The last world is never suspended. A universe with every world suspended
	//   is a game that has stopped without saying so.
	//
	// @param inputs What the host knows.
	// @return What to do.
	LifecycleAction DecideLifecycle(const LifecycleInputs &inputs);
}
