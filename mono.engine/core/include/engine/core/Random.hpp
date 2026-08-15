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
// mono.server/src/Simulation.cpp - the same function twice, with a comment in
// the second admitting it. It is one function now, and a *specified* one rather
// than a constant nobody can check, which was always the real objection to the
// copies.
//
// **The specified function is SplitMix64's finaliser, and it was SHA-256 until
// v0.15.** Both are specified, so the property this file exists for is
// unchanged; what changed is that the bottom layer of the engine no longer
// depends on a cryptography library to produce a number. `Engine::core` linked
// Crypto++ for this one call and nothing else, and everything links `core`.
//
// **That is dependency hygiene, and it is not a size win - say so out loud.**
// `net` and `assets` are what put Crypto++ into the client, the server and the
// origin, and they still do; all three carry exactly what they carried before.
// What the swap buys is that a program linking `core` and neither `net` nor
// `assets` no longer drags a hash library in behind `Random::Float`, and that
// `THIRD_PARTY_NOTICES.md` no longer owes an entry to the module every other
// module depends on. `docs/retired/DEFERRED.md` D00004 has the measurements.
//
// **Every seeded stream moved when this changed.** `Random.new(seed)` in both
// script VMs draws through here, so a world that placed a tree at a particular
// spot places it somewhere else now. That was the accepted cost, not an
// oversight: the numbers are a specified function of their arguments, and which
// specified function was never a promise.
//
// **Indexed, not sequential.** Every function here is pure: Float(index, salt)
// depends on nothing but its arguments. Spawning entity 500 on its own gives the
// value it would have had in a loop from zero, which is what makes these safe to
// call from EachParallel or from a spawn path that skips entities. A stateful
// generator would look equivalent and quietly not be.
//
// Salts are how one index gives several independent values - position wants
// three and a colour wants three more. Any two distinct salts give independent
// sequences; the call sites use small primes only because they read well.
//
// **Two nanoseconds a call, and the warning that used to stand here is gone
// with the reason for it.** SHA-256 cost about 47 ns, so this header spent five
// paragraphs saying it was a load-time budget and must not be called per entity
// per frame. On the `bench` preset `engine.core.bench.values` now measures
// `Float` at 2 ns and a three-salt position at 7 ns, against 1 ns for a bare
// xorshift32 in the same run. There is no longer a call site this is too dear
// for.
//
// **Not for anything security-sensitive, and that is a property of the type
// rather than of what is under it.** It is a stable, portable, well-distributed
// number, and no attempt is made to keep anything about it secret - a stream is
// a pure function of two arguments an attacker can guess. Anything that needs
// to be unpredictable takes `net`'s handshake or `assets`' grant key, both of
// which draw from the operating system and both of which say so in their own
// headers. Nothing in the tree reaches here for a key, a nonce or a token, and
// nothing should start.
//
// @tier L0 · shared

#include <cstdint>

namespace engine::core {

	// Produces portable deterministic values from an index and an independent salt.
	//
	// @threadsafe
	class Random {
	  public:
		// The top 32 bits of SplitMix64's finaliser over the `(salt, index)` pair.
		//
		// The finaliser is Steele, Lea and Flood's `mix64variant13`, published in
		// *Fast Splittable Pseudorandom Number Generators* (OOPSLA 2014) and
		// spelled here as Vigna's public-domain `splitmix64.c` spells it.
		// `tests/Random.cpp` pins the sequence it produces, against the constant
		// every reference implementation of it prints.
		static uint32_t Bits(uint32_t index, uint32_t salt);

		// A uniform float in [0, 1) - half-open, so it never returns 1.0f.
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
