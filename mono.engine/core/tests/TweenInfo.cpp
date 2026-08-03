#include <engine/core/types/TweenInfo.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cstring>

TEST_SUITE_ID("engine.core.types.tweeninfo")

using Catch::Approx;
using engine::core::EasingDirection;
using engine::core::EasingStyle;
using engine::core::TweenInfo;

namespace {
	// Every style, so a case added to the enum and not to the switch shows up
	// as a curve that behaves like Linear rather than as a silent omission.
	constexpr EasingStyle EVERY_STYLE[] = {
		EasingStyle::Linear,
		EasingStyle::Quad,
		EasingStyle::Cubic,
		EasingStyle::Quart,
		EasingStyle::Quint,
		EasingStyle::Sine,
		EasingStyle::Exponential,
		EasingStyle::Circular,
		EasingStyle::Back,
		EasingStyle::Elastic,
		EasingStyle::Bounce,
	};

	constexpr EasingDirection EVERY_DIRECTION[] = {
		EasingDirection::In,
		EasingDirection::Out,
		EasingDirection::InOut,
	};
}

TEST_CASE("every curve starts at zero and ends at one", "[tweeninfo]") {
	// The property that makes a curve usable at all: a tween that does not
	// finish where it was told to lands the part somewhere nobody asked for.
	for (const EasingStyle style : EVERY_STYLE) {
		for (const EasingDirection direction : EVERY_DIRECTION) {
			REQUIRE(TweenInfo::Ease(0.0f, style, direction) == Approx(0.0f).margin(1.0e-5f));
			REQUIRE(TweenInfo::Ease(1.0f, style, direction) == Approx(1.0f).margin(1.0e-5f));
		}
	}
}

TEST_CASE("alpha is clamped rather than extrapolated", "[tweeninfo]") {
	// Elastic past one grows without bound, which would surface as a part flung
	// out of the world rather than as a bad alpha.
	for (const EasingStyle style : EVERY_STYLE) {
		for (const EasingDirection direction : EVERY_DIRECTION) {
			REQUIRE(TweenInfo::Ease(-5.0f, style, direction) == Approx(0.0f).margin(1.0e-5f));
			REQUIRE(TweenInfo::Ease(9.0f, style, direction) == Approx(1.0f).margin(1.0e-5f));
		}
	}
}

TEST_CASE("linear is the identity in every direction", "[tweeninfo]") {
	for (const EasingDirection direction : EVERY_DIRECTION) {
		REQUIRE(TweenInfo::Ease(0.25f, EasingStyle::Linear, direction) == Approx(0.25f));
		REQUIRE(TweenInfo::Ease(0.5f, EasingStyle::Linear, direction) == Approx(0.5f));
		REQUIRE(TweenInfo::Ease(0.75f, EasingStyle::Linear, direction) == Approx(0.75f));
	}
}

TEST_CASE("the polynomial curves are what they claim", "[tweeninfo]") {
	// Quad In at a half is a quarter; Quad Out at a half is three quarters.
	// These are the two numbers that catch a direction swapped anywhere.
	REQUIRE(TweenInfo::Ease(0.5f, EasingStyle::Quad, EasingDirection::In) == Approx(0.25f));
	REQUIRE(TweenInfo::Ease(0.5f, EasingStyle::Quad, EasingDirection::Out) == Approx(0.75f));

	REQUIRE(TweenInfo::Ease(0.5f, EasingStyle::Cubic, EasingDirection::In) == Approx(0.125f));
	REQUIRE(TweenInfo::Ease(0.5f, EasingStyle::Cubic, EasingDirection::Out) == Approx(0.875f));

	REQUIRE(TweenInfo::Ease(0.5f, EasingStyle::Quart, EasingDirection::In) == Approx(0.0625f));
	REQUIRE(TweenInfo::Ease(0.5f, EasingStyle::Quint, EasingDirection::In) == Approx(0.03125f));
}

TEST_CASE("InOut is symmetric about the midpoint", "[tweeninfo]") {
	// `f(t) + f(1-t) == 1` for a symmetric curve. Back and Elastic overshoot
	// but stay symmetric, so this holds for the whole set.
	for (const EasingStyle style : EVERY_STYLE) {
		for (float alpha = 0.0f; alpha <= 1.0f; alpha += 0.125f) {
			const float forward = TweenInfo::Ease(alpha, style, EasingDirection::InOut);
			const float backward = TweenInfo::Ease(1.0f - alpha, style, EasingDirection::InOut);
			REQUIRE(forward + backward == Approx(1.0f).margin(1.0e-5f));
		}
	}

	REQUIRE(TweenInfo::Ease(0.5f, EasingStyle::Quad, EasingDirection::InOut) == Approx(0.5f));
}

TEST_CASE("In and Out are reflections of one another", "[tweeninfo]") {
	// This is the relation `Ease` is written in terms of rather than as eleven
	// more cases, so the test is what keeps the two halves from drifting apart
	// if somebody ever writes them out.
	for (const EasingStyle style : EVERY_STYLE) {
		for (float alpha = 0.0f; alpha <= 1.0f; alpha += 0.0625f) {
			const float in = TweenInfo::Ease(alpha, style, EasingDirection::In);
			const float out = TweenInfo::Ease(1.0f - alpha, style, EasingDirection::Out);
			REQUIRE(in + out == Approx(1.0f).margin(1.0e-5f));
		}
	}
}

TEST_CASE("exponential starts exactly at zero", "[tweeninfo]") {
	// `2^-10` is about a thousandth, not nothing, and a curve that does not
	// start at its start point is a visible jump on the first frame.
	REQUIRE(TweenInfo::Ease(0.0f, EasingStyle::Exponential, EasingDirection::In) == Approx(0.0f));
}

TEST_CASE("back and elastic overshoot, and the others do not", "[tweeninfo]") {
	bool backOvershot = false;
	bool elasticOvershot = false;

	for (float alpha = 0.0f; alpha <= 1.0f; alpha += 0.01f) {
		if (TweenInfo::Ease(alpha, EasingStyle::Back, EasingDirection::In) < -1.0e-4f) {
			backOvershot = true;
		}
		if (TweenInfo::Ease(alpha, EasingStyle::Elastic, EasingDirection::In) < -1.0e-4f) {
			elasticOvershot = true;
		}

		// The monotone set stays inside [0, 1] the whole way, which is what
		// makes them safe for a colour or an alpha channel.
		for (const EasingStyle style :
			 {EasingStyle::Linear,
			  EasingStyle::Quad,
			  EasingStyle::Cubic,
			  EasingStyle::Sine,
			  EasingStyle::Circular,
			  EasingStyle::Bounce}) {
			const float value = TweenInfo::Ease(alpha, style, EasingDirection::InOut);
			REQUIRE(value >= -1.0e-4f);
			REQUIRE(value <= 1.0f + 1.0e-4f);
		}
	}

	REQUIRE(backOvershot);
	REQUIRE(elasticOvershot);
}

TEST_CASE("bounce lands on its own segment boundaries", "[tweeninfo]") {
	// The four-segment curve's peaks. A shifted constant reads as a curve that
	// is nearly right, so the boundaries are what pin it.
	REQUIRE(TweenInfo::Ease(1.0f, EasingStyle::Bounce, EasingDirection::Out) == Approx(1.0f));
	REQUIRE(
		TweenInfo::Ease(1.0f / 2.75f, EasingStyle::Bounce, EasingDirection::Out) ==
		Approx(1.0f).margin(1.0e-3f)
	);
}

TEST_CASE("the default is Roblox's default", "[tweeninfo]") {
	const TweenInfo info;

	REQUIRE(info.Time == Approx(1.0f));
	REQUIRE(info.Style == EasingStyle::Quad);
	REQUIRE(info.Direction == EasingDirection::Out);
	REQUIRE(info.RepeatCount == 0);
	REQUIRE_FALSE(info.Reverses);
	REQUIRE(info.DelayTime == Approx(0.0f));
}

TEST_CASE("Evaluate agrees with the static Ease", "[tweeninfo]") {
	const TweenInfo info{2.0f, EasingStyle::Sine, EasingDirection::InOut};

	for (float alpha = 0.0f; alpha <= 1.0f; alpha += 0.125f) {
		REQUIRE(
			info.Evaluate(alpha) == Approx(TweenInfo::Ease(alpha, EasingStyle::Sine, EasingDirection::InOut))
		);
	}
}

TEST_CASE("the padding byte is initialised", "[tweeninfo]") {
	// Named padding is only worth anything if it is actually written. Two
	// values built the same way must be byte-identical, because that is what a
	// snapshot compares.
	const TweenInfo first{1.5f, EasingStyle::Cubic, EasingDirection::In, 3, true, 0.25f};
	const TweenInfo second{1.5f, EasingStyle::Cubic, EasingDirection::In, 3, true, 0.25f};

	REQUIRE(std::memcmp(&first, &second, sizeof(TweenInfo)) == 0);
	REQUIRE(first == second);
}
