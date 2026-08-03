#pragma once

// A value that changes over a normalised zero-to-one span — a gradient, in the
// general sense.
//
// **Fixed capacity, and that is a determinism decision rather than a
// micro-optimisation.** A sequence with a heap-allocated keypoint list is not
// trivially copyable, so it cannot be a component, cannot be written into a
// snapshot as its object representation and cannot cross a bus as bytes. Twenty
// keypoints is Roblox's own cap, so the limit costs an author nothing they were
// going to use, and it buys the type a place in every one of those paths.
//
// **The unused tail is zeroed, not left alone.** `Count` says how many
// keypoints matter, but the bytes past it still reach a snapshot, and
// uninitialised padding is the failure `ecs::WorldTime` learned the expensive
// way — two runs of one scene producing different bytes. Default member
// initialisers on the keypoint types are what make the whole array defined.
//
// **Evaluation clamps at both ends and interpolates between.** A sequence with
// no keypoints evaluates to a default-constructed value rather than reading past
// the end, because a caller that built one empty gets a visible flat result
// instead of whatever was in memory.
//
// @tier L1 · shared

#include <engine/core/types/Color3.hpp>

#include <cstdint>

namespace engine::core {

	// The largest number of keypoints any sequence holds.
	//
	// Roblox's cap. See the file comment for why there is one at all.
	inline constexpr uint32_t SEQUENCE_CAPACITY = 20;

	// The index of the **last** keypoint at or before `time`.
	//
	// "Last" rather than "first" is what makes a step behave, and it was worth a
	// failing test to find: two keypoints sharing a time are how an author
	// writes a hard edge, and a scan that stopped at the first of the pair
	// returned the value the gradient was stepping *away* from. Sampling exactly
	// on the seam then read one colour while a hair past it read the other.
	//
	// Shared by both sequences because the rule has to be the same in each — a
	// number ramp and a colour ramp built from one authored table must agree
	// about where their edges are.
	//
	// @tparam Sequence Either sequence type; both expose `Keypoints` and `Count`.
	// @param sequence  The sequence to search. At least one keypoint, sorted.
	// @param time      Where to sample.
	// @return The index of the keypoint at or before `time`.
	template <class Sequence> constexpr uint32_t LowerKeypoint(const Sequence &sequence, float time) {
		uint32_t low = 0;
		for (uint32_t index = 1; index < sequence.Count; index++) {
			if (sequence.Keypoints[index].Time > time) {
				break;
			}
			low = index;
		}
		return low;
	}

	// One stop in a `NumberSequence`.
	//
	// @since v0.6
	struct NumberKeypoint {
		// Where this stop sits, 0 to 1.
		float Time = 0.0f;

		// The value at that point.
		float Value = 0.0f;

		// How far either side of `Value` a sampler may stray.
		//
		// Roblox's spelling for the randomised band a particle emitter samples
		// within. Nothing in this header consumes it — `Evaluate` returns the
		// centre — because picking a number inside the band needs a generator,
		// and which generator is the caller's decision. A sequence that reached
		// for a global one would be the determinism hazard `core::Random` exists
		// to avoid.
		float Envelope = 0.0f;

		// Constructs the keypoint at time zero with value zero.
		constexpr NumberKeypoint() = default;

		// Constructs a keypoint with no envelope.
		//
		// @param time  Where it sits, 0 to 1.
		// @param value The value there.
		constexpr NumberKeypoint(float time, float value) : Time(time), Value(value) {}

		// Constructs a keypoint with an envelope.
		//
		// @param time     Where it sits, 0 to 1.
		// @param value    The value there.
		// @param envelope How far either side a sampler may stray.
		constexpr NumberKeypoint(float time, float value, float envelope)
			: Time(time), Value(value), Envelope(envelope) {}

		// Reports whether every field is exactly equal.
		constexpr bool operator==(const NumberKeypoint &other) const {
			return Time == other.Time && Value == other.Value && Envelope == other.Envelope;
		}
	};

	// One stop in a `ColorSequence`.
	//
	// @since v0.6
	struct ColorKeypoint {
		// The colour at that point.
		Color3 Value{1.0f, 1.0f, 1.0f};

		// Where this stop sits, 0 to 1.
		float Time = 0.0f;

		// Explicit padding, so the object representation carries no
		// uninitialised bytes into a snapshot. `Color3` is three floats and
		// `Time` is a fourth, so this is the alignment tail rather than an
		// interior hole — named anyway, because the day somebody adds a field
		// is the day it stops being one.
		uint32_t Reserved = 0;

		// Constructs the keypoint at time zero, white.
		constexpr ColorKeypoint() = default;

		// Constructs a keypoint.
		//
		// @param time  Where it sits, 0 to 1.
		// @param value The colour there.
		constexpr ColorKeypoint(float time, const Color3 &value) : Value(value), Time(time) {}

		// Reports whether the time and the colour are exactly equal.
		constexpr bool operator==(const ColorKeypoint &other) const {
			return Time == other.Time && Value.R == other.Value.R && Value.G == other.Value.G &&
				   Value.B == other.Value.B;
		}
	};

	// A number that varies over a normalised span.
	//
	// @since v0.6
	struct NumberSequence {
		// The stops, the first `Count` of which are meaningful. Sorted by time;
		// nothing here sorts them, because a caller building one out of order
		// has a bug and quietly fixing it hides the bug.
		NumberKeypoint Keypoints[SEQUENCE_CAPACITY] = {};

		// How many of `Keypoints` are in use.
		uint32_t Count = 0;

		// Explicit padding, for the reason the file comment gives.
		uint32_t Reserved = 0;

		// Constructs an empty sequence, which evaluates to zero everywhere.
		constexpr NumberSequence() = default;

		// Constructs the constant sequence, which is Roblox's one-argument form.
		//
		// @param value The value at every point.
		explicit constexpr NumberSequence(float value) {
			Keypoints[0] = NumberKeypoint{0.0f, value};
			Keypoints[1] = NumberKeypoint{1.0f, value};
			Count = 2;
		}

		// Constructs the ramp from one value to another.
		//
		// @param from The value at time zero.
		// @param to   The value at time one.
		constexpr NumberSequence(float from, float to) {
			Keypoints[0] = NumberKeypoint{0.0f, from};
			Keypoints[1] = NumberKeypoint{1.0f, to};
			Count = 2;
		}

		// Appends a keypoint.
		//
		// @param keypoint The stop to add.
		// @return `false` when the sequence is already full.
		constexpr bool Add(const NumberKeypoint &keypoint) {
			if (Count >= SEQUENCE_CAPACITY) {
				return false;
			}
			Keypoints[Count++] = keypoint;
			return true;
		}

		// The value at a point on the span.
		//
		// Clamped to the first and last keypoint outside `[0, 1]`, and linearly
		// interpolated between neighbours inside it.
		//
		// @param time Where to sample, 0 to 1.
		// @return The value, or zero when the sequence is empty.
		constexpr float Evaluate(float time) const {
			if (Count == 0) {
				return 0.0f;
			}
			if (time <= Keypoints[0].Time) {
				return Keypoints[0].Value;
			}
			if (time >= Keypoints[Count - 1].Time) {
				return Keypoints[Count - 1].Value;
			}

			const uint32_t low = LowerKeypoint(*this, time);
			if (low + 1 >= Count) {
				return Keypoints[low].Value;
			}

			const NumberKeypoint &previous = Keypoints[low];
			const NumberKeypoint &next = Keypoints[low + 1];
			const float span = next.Time - previous.Time;

			// Unreachable for a sorted sequence — `LowerKeypoint` picks the
			// *last* stop at or before `time`, so the next one is strictly
			// later. Kept for the unsorted case the header refuses to fix
			// silently: a NaN would surface wherever the value was consumed
			// rather than where the sequence was built wrong.
			if (span <= 0.0f) {
				return next.Value;
			}

			const float alpha = (time - previous.Time) / span;
			return previous.Value + (next.Value - previous.Value) * alpha;
		}

		// Reports whether both sequences hold the same keypoints in the same
		// order. The unused tail is not compared.
		constexpr bool operator==(const NumberSequence &other) const {
			if (Count != other.Count) {
				return false;
			}
			for (uint32_t index = 0; index < Count; index++) {
				if (!(Keypoints[index] == other.Keypoints[index])) {
					return false;
				}
			}
			return true;
		}
	};

	// A colour that varies over a normalised span.
	//
	// @since v0.6
	struct ColorSequence {
		// The stops, the first `Count` of which are meaningful.
		ColorKeypoint Keypoints[SEQUENCE_CAPACITY] = {};

		// How many of `Keypoints` are in use.
		uint32_t Count = 0;

		// Explicit padding, for the reason the file comment gives.
		uint32_t Reserved = 0;

		// Constructs an empty sequence, which evaluates to black everywhere.
		constexpr ColorSequence() = default;

		// Constructs the constant sequence.
		//
		// @param value The colour at every point.
		explicit constexpr ColorSequence(const Color3 &value) {
			Keypoints[0] = ColorKeypoint{0.0f, value};
			Keypoints[1] = ColorKeypoint{1.0f, value};
			Count = 2;
		}

		// Constructs the ramp from one colour to another.
		//
		// @param from The colour at time zero.
		// @param to   The colour at time one.
		constexpr ColorSequence(const Color3 &from, const Color3 &to) {
			Keypoints[0] = ColorKeypoint{0.0f, from};
			Keypoints[1] = ColorKeypoint{1.0f, to};
			Count = 2;
		}

		// Appends a keypoint.
		//
		// @param keypoint The stop to add.
		// @return `false` when the sequence is already full.
		constexpr bool Add(const ColorKeypoint &keypoint) {
			if (Count >= SEQUENCE_CAPACITY) {
				return false;
			}
			Keypoints[Count++] = keypoint;
			return true;
		}

		// The colour at a point on the span.
		//
		// **Interpolated in whatever space `Color3` holds**, which is linear —
		// see `Color3.hpp`. That is the correct space to blend light in, and it
		// is why a red-to-green gradient here does not pass through the muddy
		// midpoint an sRGB blend produces.
		//
		// @param time Where to sample, 0 to 1.
		// @return The colour, or black when the sequence is empty.
		constexpr Color3 Evaluate(float time) const {
			if (Count == 0) {
				return Color3{0.0f, 0.0f, 0.0f};
			}
			if (time <= Keypoints[0].Time) {
				return Keypoints[0].Value;
			}
			if (time >= Keypoints[Count - 1].Time) {
				return Keypoints[Count - 1].Value;
			}

			const uint32_t low = LowerKeypoint(*this, time);
			if (low + 1 >= Count) {
				return Keypoints[low].Value;
			}

			const ColorKeypoint &previous = Keypoints[low];
			const ColorKeypoint &next = Keypoints[low + 1];
			const float span = next.Time - previous.Time;

			// See `NumberSequence::Evaluate`: unreachable when sorted, kept for
			// when it is not.
			if (span <= 0.0f) {
				return next.Value;
			}

			const float alpha = (time - previous.Time) / span;
			return Color3{
				previous.Value.R + (next.Value.R - previous.Value.R) * alpha,
				previous.Value.G + (next.Value.G - previous.Value.G) * alpha,
				previous.Value.B + (next.Value.B - previous.Value.B) * alpha,
			};
		}

		// Reports whether both sequences hold the same keypoints in the same
		// order. The unused tail is not compared.
		constexpr bool operator==(const ColorSequence &other) const {
			if (Count != other.Count) {
				return false;
			}
			for (uint32_t index = 0; index < Count; index++) {
				if (!(Keypoints[index] == other.Keypoints[index])) {
					return false;
				}
			}
			return true;
		}
	};
}
