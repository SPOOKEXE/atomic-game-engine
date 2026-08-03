#pragma once

// Which layers a thing is on, and which it is tested against.
//
// **A struct wrapping the bits rather than a bare `uint32_t`.** A collider
// carries two of these — the layer it occupies and the set it collides with —
// and they are the same width and the same type. Swapping them at a call site
// compiles, runs, and returns a plausible wrong answer for the rest of the
// project's life. Naming the type does not stop that on its own, but it stops
// an ordinary integer wandering in from somewhere else and being treated as a
// mask, which is the more common half of the mistake.
//
// **A layer bit index is session-local.** Nothing here serialises one, and
// nothing should: an index is a number derived from whatever order the layers
// were declared in, which is rule 4 of the root `AGENTS.md` almost word for
// word. A save file or a wire format names a layer with a string and resolves
// it to an index once at load.
//
// @tier L6 · shared

#include <cstdint>

namespace engine::spatial {

	// A set of layers, one bit each.
	//
	// Default-constructed it is **empty and matches nothing**. That is the
	// deliberate choice between two failures: a proxy nothing can see fails the
	// first test written against it, and a proxy everything can see is a
	// trigger firing wrongly some months later. Empty is also the identity for
	// `|`, so accumulating a set out of nothing works without a special case.
	//
	// @since v0.4
	struct LayerMask {
		// How many layers fit. Fixed by the width of `Bits`, and named so that
		// `Only` has something to bound an index against.
		static constexpr uint32_t LAYER_COUNT = 32;

		// The set, one bit per layer. Bit `n` is layer `n`.
		uint32_t Bits = 0;

		// Constructs the empty set, which matches nothing.
		constexpr LayerMask() = default;

		// Constructs a set from raw bits.
		//
		// Explicit, so an integer that happens to be lying around cannot become
		// a mask by accident — which is the reason this is a type at all.
		//
		// @param bits One bit per layer.
		explicit constexpr LayerMask(uint32_t bits) : Bits(bits) {}

		// The set of every layer.
		static constexpr LayerMask All() {
			return LayerMask{0xFFFFFFFFu};
		}

		// The empty set, which matches nothing.
		static constexpr LayerMask None() {
			return LayerMask{0u};
		}

		// The set holding exactly one layer, or the empty set for an index out of range.
		//
		// Out of range returns `None()` rather than clamping or wrapping,
		// because shifting a 32-bit value by 32 is undefined behaviour and the
		// answer a compiler picks for it varies with the optimisation level.
		//
		// @param index Which layer, from zero to `LAYER_COUNT` minus one.
		static constexpr LayerMask Only(uint32_t index) {
			if (index >= LAYER_COUNT) {
				return None();
			}
			return LayerMask{1u << index};
		}

		// Reports whether the two sets share **any** layer.
		//
		// A shared bit, not equality. Equality would mean a query could only
		// ever find proxies on exactly the same set of layers it named, which
		// is never what a layer mask is for.
		constexpr bool Overlaps(LayerMask other) const {
			return (Bits & other.Bits) != 0u;
		}

		// Returns the union of the two sets.
		constexpr LayerMask operator|(LayerMask other) const {
			return LayerMask{Bits | other.Bits};
		}

		// Returns the intersection of the two sets.
		constexpr LayerMask operator&(LayerMask other) const {
			return LayerMask{Bits & other.Bits};
		}

		// Reports whether the two sets hold exactly the same layers.
		constexpr bool operator==(LayerMask other) const {
			return Bits == other.Bits;
		}
	};
}
