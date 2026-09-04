#pragma once

// arch-waiver public-header: forward ECS API. Simulation hosts share this
// complete tick-time contract rather than creating local clocks.

// The world's clock, as a resource.
//
// Time is world state rather than an argument. A system handed a delta can be
// handed the wrong one - the frame's instead of the tick's - and the result is
// a simulation that behaves differently at 30 fps and at 300 with nothing in
// the code looking wrong. Reading the clock from the world deletes the
// parameter that made the mistake possible, so the two deltas are two named
// fields instead of one that means different things at different call sites.
//
// The dedicated clock API is the writer: `Store::AdvanceTick` owns the
// simulation fields and `Store::SetFrame` owns the presentation ones. Callers
// must not mutate or remove WorldTime through Store's generic resource API.
//
// @tier L3 · shared

#include <cstdint>

namespace engine::ecs {

	// A snapshot of one world's simulation and presentation clocks.
	//
	// Store::Time returns this aggregate by value so systems using the clock API
	// cannot mutate it or retain a reference invalidated by resource movement.
	struct WorldTime {
		// Simulated seconds since the world started, advanced by Delta per
		// tick. Not wall time: two runs of the same world at different frame
		// rates agree on it, which is what makes them comparable.
		double Elapsed = 0.0;

		// The fixed tick delta. What a simulation system integrates by.
		float Delta = 1.0f / 60.0f;

		// Explicit padding, and it is load-bearing.
		//
		// `Tick` needs eight-byte alignment, so without this the compiler
		// leaves four bytes of *uninitialised* padding here - and a trivially
		// copyable component is serialised as its object representation, so
		// those four bytes go straight into the snapshot. Two runs of the same
		// scene then produce different files, which breaks a recording, a CI
		// determinism diff, and any byte comparison of two worlds.
		//
		// Found by `just determinism`, which is why that job exists. See
		// `ecs/docs/TODO.md` on the general fix.
		uint32_t Reserved = 0;

		// Completed simulation ticks since the world started.
		uint64_t Tick = 0;

		// Wall seconds the frame took. What a presentation system may use, and
		// what a simulation system must not - it varies with the display.
		//
		// Stays zero on a headless world, because nothing there draws.
		float FrameDelta = 0.0f;

		// How far the frame being drawn sits between the last tick and the
		// next, 0..1. Zero on a frame landing exactly on a tick boundary.
		float Alpha = 0.0f;
	};
}
