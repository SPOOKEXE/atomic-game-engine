#pragma once

// What this client does with a world it does not own.
//
// **This file used to be the duplication and is now the opposite of it.** It
// declared a `ReplicatedPosition` and a `ReplicatedVelocity` registered under
// `server.Position` and `server.Velocity`, because a snapshot travels by
// component *name* and the two programs shared no component set — so the client
// had to declare the server's types a second time and keep the layouts in step
// by hand. `mono.engine/scene` at L7 ended that: both programs register the same
// `scene` components under the same names, and a snapshot and a delta cross with
// no translation layer at all.
//
// What is left is the half that is genuinely the client's own: a replicated
// world arrives as rows and has to be *drawn*, and nothing about drawing it
// crosses the wire. That is presentation over somebody else's simulation, which
// is neither the demo scene nor the engine's business, so it lives here.
//
// **A replica simulates nothing.** The only system registered below is a
// `PreRender` one, which derives what to draw and mutates no simulation state.
// A replica that ticked would be this process disagreeing with the authority
// once per tick, which is the bug replication exists to avoid.

#include <engine/ecs/Scheduler.hpp>
#include <engine/ecs/Store.hpp>

namespace client {

	// Installs the presentation half of a replicated world.
	//
	// Called once, when the world is created and before any snapshot is
	// applied — a resource added later is still legal, but a draw list that
	// appeared halfway through a join would leave the first frames with nothing
	// to publish.
	//
	// Registers no simulation system. Nothing here writes a component.
	//
	// @param store     The replicated world.
	// @param scheduler Its scheduler, which gains one `PreRender` system.
	void BuildReplicatedWorld(engine::ecs::Store &store, engine::ecs::Scheduler &scheduler);
}
