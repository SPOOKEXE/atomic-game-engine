#pragma once

// Taking in the people a teleport sent here, with no VM in it.
//
// **Split out of `BusServices.cpp` at v0.18, because this is a system and that
// file is a description.** `AdmitTeleports` runs as an `ecs::Scheduler` system
// on *every* world — `Runtime.hpp` carries the argument, and the short version
// is that a world can be a teleport destination without containing a single line
// of script — so compiling it in a translation unit that includes `<lua.h>` for
// four service surfaces' benefit was the exact shape rule 6 asks about: nothing
// checked that the admitter did not need a VM, and it never did.
//
// `TeleportService`'s surface still lives in `BusServices.cpp`; what the two
// share is one child name, which is below.
//
// @tier L9 · shared
// @since v0.18

#include <engine/ecs/Entity.hpp>

namespace engine::script {

	// The child a teleported player carries their data in.
	//
	// **A `StringValue` under the player rather than a component of its own.** The
	// data is an arbitrary script value; a component holding one would need a
	// type, a serialiser and a wire form for something the engine never reads. A
	// `StringValue` is authored content that already round-trips, already
	// replicates, and is already something a script can see in the explorer —
	// which is worth more here than tidiness.
	//
	// **Shared because the write and the read are in different files.**
	// `AdmitTeleports` creates it and `TeleportService:GetTeleportData` reads it
	// back, so the two must not each spell it.
	constexpr const char *TELEPORT_DATA = "TeleportData";
}
