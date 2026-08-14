#pragma once

// How an interpolation is shaped over time.
//
// **The easing curve is here and the tween is not.** `TweenInfo` describes a
// shape; running one needs a target instance, a property name and a place in the
// tick to advance from, none of which belong to a value type at L1. So this
// header answers one question - *given how far through I am, how far along am
// I?* - and `Evaluate` is a pure function of a normalised alpha.
//
// That split is what makes the curve testable at all. An easing function inside
// a tween service is checked by watching something move; a pure one is checked
// by asserting that `Quad`/`Out` at a half is three quarters.
//
// **The curves are the standard Penner set**, which is what Roblox's are, so an
// author's expectation transfers. `Evaluate` clamps its alpha: past the end a
// tween is finished, and an elastic curve extrapolated past one grows without
// bound.
//
// @tier L1 · shared

#include <cmath>
#include <cstdint>

namespace engine::core {

	// The shape of an easing curve.
	//
	// @since v0.6
	enum class EasingStyle : uint8_t {
		// Constant rate. The identity curve.
		Linear,

		// `t²`. The gentlest of the polynomial family.
		Quad,

		// `t³`.
		Cubic,

		// `t⁴`.
		Quart,

		// `t⁵`.
		Quint,

		// A quarter turn of a sine wave.
		Sine,

		// `2^(10(t-1))`. The sharpest of the standard set.
		Exponential,

		// A quarter of a circle, which starts almost flat and ends vertical.
		Circular,

		// Overshoots the target and comes back.
		Back,

		// Overshoots repeatedly with a decaying amplitude.
		Elastic,

		// Bounces off the target with decaying height.
		Bounce,
	};

	// Which end of a curve the easing applies to.
	//
	// @since v0.6
	enum class EasingDirection : uint8_t {
		// Slow at the start, full rate at the end.
		In,

		// Full rate at the start, slow at the end.
		Out,

		// Slow at both ends, fastest in the middle. The two halves are the `In`
		// curve and the `Out` curve, each compressed into half the span.
		InOut,
	};

	// How long an interpolation takes and what shape it follows.
	//
	// @since v0.6
	struct TweenInfo {
		// How long one pass takes, in **simulated** seconds. Never wall time:
		// see `ecs::WorldTime`, and rule 5 for what a wall clock does to a
		// recording.
		float Time = 1.0f;

		// How long to wait before the first pass, in simulated seconds.
		float DelayTime = 0.0f;

		// How many extra passes to run. `-1` repeats without end, matching
		// Roblox; anything below that is treated as endless too.
		int32_t RepeatCount = 0;

		// The curve's shape.
		EasingStyle Style = EasingStyle::Quad;

		// Which end the shape applies to.
		EasingDirection Direction = EasingDirection::Out;

		// Whether each pass runs backwards after running forwards.
		bool Reverses = false;

		// Explicit padding. A trivially copyable value is serialised as its
		// object representation and padding is never initialised, so without
		// this the byte after `Reverses` differs between two runs of one scene.
		uint8_t Reserved = 0;

		// Constructs a one-second `Quad`/`Out` tween, which is Roblox's default.
		constexpr TweenInfo() = default;

		// Constructs a tween of a given length with the default curve.
		//
		// @param time How long one pass takes, in simulated seconds.
		explicit constexpr TweenInfo(float time) : Time(time) {}

		// Constructs a fully specified tween.
		//
		// @param time        How long one pass takes.
		// @param style       The curve's shape.
		// @param direction   Which end it applies to.
		// @param repeatCount Extra passes; -1 for endless.
		// @param reverses    Whether each pass runs back afterwards.
		// @param delayTime   How long to wait before starting.
		constexpr TweenInfo(
			float time,
			EasingStyle style,
			EasingDirection direction,
			int32_t repeatCount = 0,
			bool reverses = false,
			float delayTime = 0.0f
		)
			: Time(time), DelayTime(delayTime), RepeatCount(repeatCount), Style(style), Direction(direction),
			  Reverses(reverses) {}

		// Reports whether every field is exactly equal.
		constexpr bool operator==(const TweenInfo &other) const {
			return Time == other.Time && DelayTime == other.DelayTime && RepeatCount == other.RepeatCount &&
				   Style == other.Style && Direction == other.Direction && Reverses == other.Reverses;
		}

		// Shapes a normalised progress into a normalised position.
		//
		// @param alpha How far through, 0 to 1. Clamped.
		// @return How far along, where 0 and 1 are always the endpoints - except
		//         for `Back` and `Elastic`, which overshoot by design.
		float Evaluate(float alpha) const {
			return Ease(alpha, Style, Direction);
		}

		// Shapes a normalised progress with an explicit curve.
		//
		// Static so a caller with a style and no `TweenInfo` - an editor
		// previewing a curve, a test sweeping the set - needs no throwaway
		// object.
		//
		// @param alpha     How far through, 0 to 1. Clamped.
		// @param style     The curve's shape.
		// @param direction Which end it applies to.
		// @return How far along.
		static float Ease(float alpha, EasingStyle style, EasingDirection direction) {
			// Clamped rather than extrapolated. Past the end a tween is
			// finished, and `Elastic` past one grows without bound - which would
			// surface as a part flung out of the world rather than as a bad
			// alpha.
			const float time = alpha < 0.0f ? 0.0f : (alpha > 1.0f ? 1.0f : alpha);

			switch (direction) {
			case EasingDirection::In:
				return EaseIn(time, style);
			case EasingDirection::Out:
				// The `Out` curve is the `In` curve run backwards through the
				// origin. Writing it this way rather than as eleven more cases
				// is what keeps the two halves from drifting apart.
				return 1.0f - EaseIn(1.0f - time, style);
			case EasingDirection::InOut:
				break;
			}

			// Each half is the matching curve compressed into half the span, so
			// the join at the midpoint is continuous.
			if (time < 0.5f) {
				return EaseIn(time * 2.0f, style) * 0.5f;
			}
			return 1.0f - EaseIn((1.0f - time) * 2.0f, style) * 0.5f;
		}

	  private:
		// The `In` form of every curve. Everything else is derived from it.
		static float EaseIn(float time, EasingStyle style) {
			constexpr float PI = 3.14159265358979323846f;

			switch (style) {
			case EasingStyle::Linear:
				return time;
			case EasingStyle::Quad:
				return time * time;
			case EasingStyle::Cubic:
				return time * time * time;
			case EasingStyle::Quart:
				return time * time * time * time;
			case EasingStyle::Quint:
				return time * time * time * time * time;
			case EasingStyle::Sine:
				return 1.0f - std::cos(time * PI * 0.5f);
			case EasingStyle::Exponential:
				// Zero is special-cased because `2^-10` is about a thousandth
				// rather than nothing, and a curve that does not start at its
				// start point is visible as a jump on the first frame.
				return time <= 0.0f ? 0.0f : std::pow(2.0f, 10.0f * (time - 1.0f));
			case EasingStyle::Circular:
				return 1.0f - std::sqrt(1.0f - time * time);
			case EasingStyle::Back: {
				// 1.70158 is Penner's constant: the overshoot that peaks at
				// about ten percent past the target.
				constexpr float OVERSHOOT = 1.70158f;
				return time * time * ((OVERSHOOT + 1.0f) * time - OVERSHOOT);
			}
			case EasingStyle::Elastic: {
				if (time <= 0.0f) {
					return 0.0f;
				}
				if (time >= 1.0f) {
					return 1.0f;
				}
				// A decaying sine, with the period Penner chose so that the
				// curve completes about three oscillations.
				constexpr float PERIOD = 0.3f;
				const float shift = PERIOD * 0.25f;
				return -std::pow(2.0f, 10.0f * (time - 1.0f)) *
					   std::sin((time - 1.0f - shift) * (2.0f * PI) / PERIOD);
			}
			case EasingStyle::Bounce:
				// Defined by its `Out` form everywhere it appears, so the `In`
				// form is that run backwards - the same relation `Ease` uses,
				// applied the other way round.
				return 1.0f - BounceOut(1.0f - time);
			}
			return time;
		}

		// The four-segment bounce, each segment a parabola with a smaller peak.
		static float BounceOut(float time) {
			constexpr float SCALE = 7.5625f;
			constexpr float SPLIT = 2.75f;

			if (time < 1.0f / SPLIT) {
				return SCALE * time * time;
			}
			if (time < 2.0f / SPLIT) {
				const float shifted = time - 1.5f / SPLIT;
				return SCALE * shifted * shifted + 0.75f;
			}
			if (time < 2.5f / SPLIT) {
				const float shifted = time - 2.25f / SPLIT;
				return SCALE * shifted * shifted + 0.9375f;
			}
			const float shifted = time - 2.625f / SPLIT;
			return SCALE * shifted * shifted + 0.984375f;
		}
	};
}
