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
// **It is interpolated, and the buffer that does it is not in this file.**
// `engine::replication::SnapshotBuffer` holds the received ticks and decides
// where along them the world is drawn, because the decision is about the link
// rather than about drawing. What is here is the two halves that need this
// process's own knowledge: which component carries a pose, and when the store
// holds a tick in full.
//
// **A replica simulates nothing.** The only system registered below is a
// `PreRender` one, which derives what to draw and mutates no simulation state.
// A replica that ticked would be this process disagreeing with the authority
// once per tick, which is the bug replication exists to avoid.

#include <engine/core/types/CFrame.hpp>
#include <engine/ecs/Scheduler.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/replication/SnapshotBuffer.hpp>
#include <engine/scene/Components.hpp>

#include <cstdint>

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
	// @param store        The replicated world. Gains a `DrawList` and a
	//        `engine::replication::SnapshotBuffer` resource.
	// @param scheduler    Its scheduler, which gains one `PreRender` system.
	// @param interpolation How far behind the newest received tick to draw, and
	//        the rate that delay is measured against. The tick rate must be the
	//        authority's, not this client's frame rate.
	void BuildReplicatedWorld(
		engine::ecs::Store &store,
		engine::ecs::Scheduler &scheduler,
		const engine::replication::InterpolationSettings &interpolation = {}
	);

	// Records where everything in a replicated world is, at the tick that put it
	// there.
	//
	// **Called straight after the connection has applied its inbound messages,
	// not from a render system.** That instant is the one where the store holds
	// the tick the server described; a render pass reads the same rows, but a
	// pass that only ran when a frame was drawn would miss a tick whenever the
	// frame rate dipped below the tick rate, and the buffer would then be
	// interpolating across gaps that the network never produced.
	//
	// Cheap to call on a tick already recorded — it asks the buffer first and
	// walks nothing.
	//
	// @param store The replicated world, after the connection wrote into it.
	// @param tick  The last tick applied in full, from
	//        `engine::replication::Connector::Applied`. Zero does nothing:
	//        the snapshot has not landed and there is no state to record.
	void RecordReplicatedTick(engine::ecs::Store &store, uint64_t tick);

	// Points the replica's own camera at wherever this client is looking.
	//
	// **A replica has to construct its own viewer, and this is why.** A mirror
	// reflects the *eye*, so a reflection computed on the authority is correct
	// for the authority's camera and wrong for every client watching. What
	// crosses the wire is therefore the mirror — the `SurfaceCamera`, its lens
	// and the tree that says which pane it belongs to — and never the aim. Each
	// client aims it again, from where that client is standing, which is the
	// only place the right answer exists.
	//
	// The camera is minted from the **predicted** range. A replica may not mint
	// an authoritative entity — `Store::SetAdoptOnly` says why: the index would
	// collide exactly with one the authority allocates and `Apply` would be
	// right to merge them — but the reserved high range is a client's own, and a
	// camera nobody else can see is precisely what it is for.
	//
	// Created on the first call and reused after, so this is one component write
	// per frame on a steady view. Destroying the world destroys it; there is
	// nothing else to clean up, because nothing on the authority knows it
	// exists.
	//
	// @param store The replicated world.
	// @param frame Where this client's camera is, in world space.
	// @param lens  Its field of view and clipping distances.
	// @return The camera entity, or `NULL_ENTITY` when one could not be made.
	engine::ecs::Entity AimReplicaViewer(
		engine::ecs::Store &store, const engine::core::CFrame &frame, const engine::scene::Camera &lens
	);
}
