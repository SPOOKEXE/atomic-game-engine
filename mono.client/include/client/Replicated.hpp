#pragma once

// The components this client expects a server to send it.
//
// **This is duplication, it is named as such, and it has an owner.** The server
// declares `server.Position` and `server.Velocity` in its own
// `Simulation.hpp`; a snapshot travels by component *name*, so the receiving
// process has to have registered a type under the same name with the same
// layout or the delta resolves to nothing. Two declarations of one wire type is
// exactly the shape that drifts — one side gains a field, the other does not,
// and the symptom is a component that silently stops arriving.
//
// **v0.4 deletes this file.** `mono.engine/scene` at L7 is the item that owns
// both programs' component definitions, and its roadmap line already names
// `Position` and `Velocity` in `mono.server/include/server/Simulation.hpp` among
// the definitions it removes. This header is the client's half of the same
// removal, and the reason it can be written down now is that the wire made the
// duplication load-bearing rather than merely untidy.
//
// Until then, the layouts here are checked against the server's by a
// `static_assert` on size and by the join test, which fails if a component does
// not arrive.

#include <engine/core/types/Vector3.hpp>

namespace client {

	// Where a replicated entity is, as the server sees it.
	//
	// Must match `server::Position`. Not the demo's `Transform`: the demo world
	// is this process's own and the replicated world is somebody else's, and a
	// single type used for both would be one type meaning two things.
	struct ReplicatedPosition {
		// Metres from the origin, in world space.
		engine::core::Vector3 Value;
	};

	// How fast a replicated entity is going.
	//
	// Must match `server::Velocity`. Received rather than integrated here — a
	// replica does not simulate what the server simulates, it is told.
	struct ReplicatedVelocity {
		// Metres per second.
		engine::core::Vector3 Value;
	};

	// Registers the replicated components under the names the server sends.
	//
	// The names are the server's, not this program's, because a name is the
	// identity on the wire. Called before a connection is opened, since a
	// snapshot that arrives before registration resolves to nothing.
	void RegisterReplicatedComponents();
}
