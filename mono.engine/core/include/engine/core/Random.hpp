#pragma once

// Deterministic values, indexed rather than streamed.
//
// Two runs of a server have to agree, and a client replaying a recording has to
// put the same cube in the same place. That rules out the standard generators:
// std::mt19937 is specified, but std::uniform_real_distribution is not, and the
// same seed gives different numbers on different standard libraries. It equally
// rules out std::hash, which may differ between runs of the same binary.
//
// This was a copy-pasted integer mixer in mono.client/src/Demo.cpp and again in
// mono.server/src/Simulation.cpp — the same function twice, with a comment in
// the second admitting it. It is SHA-256 from Crypto++ now, in one place, which
// is a specified algorithm rather than a constant nobody can check.
//
// **Indexed, not sequential.** Every function here is pure: Float(index, salt)
// depends on nothing but its arguments. Spawning entity 500 on its own gives the
// value it would have had in a loop from zero, which is what makes these safe to
// call from EachParallel or from a spawn path that skips entities. A stateful
// generator would look equivalent and quietly not be.
//
// Salts are how one index gives several independent values — position wants
// three and a colour wants three more. Any two distinct salts give independent
// sequences; the call sites use small primes only because they read well.
//
// Not for anything security-sensitive, despite what is underneath. It is a
// stable, portable, well-distributed number and no attempt is made to keep the
// seed secret.
//
// **It costs about 47 nanoseconds a call, and that is a load-time budget rather
// than a per-frame one.** Every function here is a full SHA-256 compression, so
// `engine.core.bench.values` measures it at roughly fifty times the integer
// mixer it replaced — a position built from three salts is around 146 ns, which
// makes spawning a hundred thousand entities about fifteen milliseconds of
// hashing on its own.
//
// That is the right price for determinism at a spawn point, a world seed or a
// content generator, all of which run once. It is the wrong price inside a
// system that runs every tick: calling this per entity per frame costs more
// than iterating the entire world does — compare the figure against
// `engine.ecs.bench.iteration`, where `Each` over a hundred thousand entities
// is a couple of nanoseconds a row.
//
// If a per-frame call site ever genuinely needs this, the answer is to compute
// the values once at spawn and store them on the entity, not to make this
// function cheaper: the specified algorithm is exactly what the type is for.
//
// @tier L0 · shared

#include <cstdint>

namespace engine::core {

	// Produces portable deterministic values from an index and an independent salt.
	//
	// @threadsafe
	class Random {
	  public:
		// The first 32 bits of SHA-256 over the big-endian `(index, salt)` pair.
		static uint32_t Bits(uint32_t index, uint32_t salt);

		// A uniform float in [0, 1) — half-open, so it never returns 1.0f.
		//
		// Exact across platforms rather than merely deterministic: the result is
		// a 24-bit integer scaled by a power of two, which IEEE-754 represents
		// without rounding. There is no last-bit disagreement between compilers
		// to worry about.
		static float Float(uint32_t index, uint32_t salt);

		// Returns `minimum + Float() * (maximum - minimum)`. When `minimum` is less
		// than `maximum`, the result is in `[minimum, maximum)`.
		static float Range(uint32_t index, uint32_t salt, float minimum, float maximum);
	};
}
