#pragma once

// A closed interval of numbers.
//
// Roblox's `NumberRange`, and it is two floats with one useful question - is a
// value inside - plus the sampling a particle emitter does with it. Kept as its
// own type rather than a `Vector2` because the two mean different things: a
// `Vector2` has an X and a Y that are independent, and this has a low and a high
// that are ordered. Passing one where the other is expected compiles either way,
// so the names are what stop it.
//
// @tier L1 · shared

namespace engine::core {

	// A closed interval `[Minimum, Maximum]`.
	//
	// Nothing here reorders a range built backwards. `Contains` on one is false
	// for every value, which is the answer that makes the mistake visible at the
	// first test rather than at the tenth - the same choice `AABB` makes.
	//
	// @since v0.6
	struct NumberRange {
		// The low end, included.
		float Minimum = 0.0f;

		// The high end, included.
		float Maximum = 0.0f;

		// Constructs the range containing only zero.
		constexpr NumberRange() = default;

		// Constructs the range containing only `value`, which is Roblox's
		// one-argument form.
		//
		// @param value Both ends.
		explicit constexpr NumberRange(float value) : Minimum(value), Maximum(value) {}

		// Constructs a range from both ends.
		//
		// @param minimum The low end.
		// @param maximum The high end.
		constexpr NumberRange(float minimum, float maximum) : Minimum(minimum), Maximum(maximum) {}

		// Reports whether both ends are exactly equal.
		constexpr bool operator==(const NumberRange &other) const {
			return Minimum == other.Minimum && Maximum == other.Maximum;
		}

		// The distance between the ends. Negative on a backwards range.
		constexpr float Span() const {
			return Maximum - Minimum;
		}

		// Reports whether a value lies in the interval, ends included.
		constexpr bool Contains(float value) const {
			return value >= Minimum && value <= Maximum;
		}

		// The value `alpha` of the way from the low end to the high one.
		//
		// Unclamped, matching every other `Lerp` in these headers: clamping here
		// would silently swallow an out-of-range alpha a caller wanted to know
		// about.
		constexpr float Lerp(float alpha) const {
			return Minimum + Span() * alpha;
		}

		// A value brought inside the interval.
		constexpr float Clamp(float value) const {
			return value < Minimum ? Minimum : (value > Maximum ? Maximum : value);
		}
	};
}
