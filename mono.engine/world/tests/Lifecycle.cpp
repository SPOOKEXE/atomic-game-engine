// The world lifetime policy, which existed only inside a running editor before.
//
// **Every case here was previously reachable by opening the studio and waiting
// five minutes.** That is the actual argument for hoisting it — `DEFERRED.md`
// D00017 makes the case about a second copy, and it is right, but the dividend
// that arrived first is that the three refusals now have assertions instead of
// comments.

#include <engine/testing/Suite.hpp>
#include <engine/world/Enums.hpp>
#include <engine/world/Lifecycle.hpp>

#include <catch2/catch_test_macros.hpp>

TEST_SUITE_ID("engine.world.lifecycle")

using engine::world::DecideLifecycle;
using engine::world::LifecycleAction;
using engine::world::LifecycleInputs;
using engine::world::WorldState;

namespace {
	// An active world that has been empty for an hour, which is the arrangement
	// every refusal below is a refusal *of*. Written as the default so each case
	// changes exactly the one fact it is about.
	LifecycleInputs Stale() {
		LifecycleInputs inputs;
		inputs.State = WorldState::Active;
		inputs.Occupied = false;
		inputs.IdleSeconds = 3600.0;
		inputs.IdleLimit = 300.0;
		inputs.LastWorld = false;
		return inputs;
	}
}

TEST_CASE("an empty world past its limit suspends", "[world][lifecycle]") {
	CHECK(DecideLifecycle(Stale()) == LifecycleAction::Suspend);
}

TEST_CASE("an occupied world never suspends, however long its clock", "[world][lifecycle]") {
	LifecycleInputs inputs = Stale();
	inputs.Occupied = true;

	// **Occupancy outranks the clock rather than resetting it**, which is what
	// lets a caller leave `IdleSeconds` stale while somebody is standing in a
	// world. Closing a world somebody is watching freezes it in front of them
	// with no visible cause.
	CHECK(DecideLifecycle(inputs) == LifecycleAction::Leave);
}

TEST_CASE("the last world is never suspended", "[world][lifecycle]") {
	LifecycleInputs inputs = Stale();
	inputs.LastWorld = true;

	// A universe with every world suspended is a game that has stopped without
	// saying so.
	CHECK(DecideLifecycle(inputs) == LifecycleAction::Leave);
}

TEST_CASE("a world inside its limit is left alone", "[world][lifecycle]") {
	LifecycleInputs inputs = Stale();
	inputs.IdleSeconds = 299.0;
	CHECK(DecideLifecycle(inputs) == LifecycleAction::Leave);

	// The boundary is inclusive at the limit, so a limit of zero suspends
	// immediately rather than never — which is what `--idle-close 0` has to
	// mean for the flag to be usable as an off switch in the other direction.
	inputs.IdleSeconds = 300.0;
	CHECK(DecideLifecycle(inputs) == LifecycleAction::Suspend);
}

TEST_CASE("a suspended world wakes for its inbox and for nothing else", "[world][lifecycle]") {
	LifecycleInputs inputs = Stale();
	inputs.State = WorldState::Suspended;

	CHECK(DecideLifecycle(inputs) == LifecycleAction::Leave);

	inputs.InboxWaiting = true;
	CHECK(DecideLifecycle(inputs) == LifecycleAction::Resume);
}

TEST_CASE("occupancy cannot wake a suspended world", "[world][lifecycle]") {
	LifecycleInputs inputs = Stale();
	inputs.State = WorldState::Suspended;
	inputs.Occupied = true;

	// **Nothing can occupy a world that is not running.** Somebody arriving in
	// one is a teleport, and a teleport is a message — so the inbox is the only
	// thing that can say a suspended world is wanted, and treating occupancy as
	// a second answer would be a race with whatever wrote it.
	CHECK(DecideLifecycle(inputs) == LifecycleAction::Leave);
}

TEST_CASE("a faulted world belongs to the supervisor", "[world][lifecycle]") {
	LifecycleInputs inputs = Stale();
	inputs.State = WorldState::Faulted;

	// Suspending it would take a world out of the quarantine that is trying to
	// restore it from its snapshot.
	CHECK(DecideLifecycle(inputs) == LifecycleAction::Leave);
}

TEST_CASE("an idle-rate world is not suspended out from under itself", "[world][lifecycle]") {
	LifecycleInputs inputs = Stale();
	inputs.State = WorldState::Idle;

	// `Idle` is already the reduced-rate state. Deciding between it and
	// `Suspended` is a policy no host has needed yet, and answering it here
	// would put a second opinion beside `SetState`.
	CHECK(DecideLifecycle(inputs) == LifecycleAction::Leave);
}

// --- what a state says about ticking -----------------------------------------

// **`Ticks` exists because callers were spelling it `state == Active` and two
// states tick.** The one that got caught was in the editor: an interpolation
// alpha derived from a state test drew an `Idle` world at the tick rate while
// it was simulating perfectly well, and — with the other half of the same
// question missing — drew every part of an *edited* world at the origin. See
// `studio/Presentation.hpp`.
//
// Enumerated rather than tested in two groups, so adding a state to `WorldState`
// and forgetting it here is a compile error in the switch and a missing line
// in this list.
TEST_CASE("two world states tick and three do not", "[world][lifecycle]") {
	CHECK(engine::world::Ticks(WorldState::Active));
	CHECK(engine::world::Ticks(WorldState::Idle));

	CHECK_FALSE(engine::world::Ticks(WorldState::Suspended));
	CHECK_FALSE(engine::world::Ticks(WorldState::Faulted));

	// **`Remote` is a record of a world this process does not hold**, so it is
	// not merely not ticking here — there is no storage to tick.
	CHECK_FALSE(engine::world::Ticks(WorldState::Remote));
}
