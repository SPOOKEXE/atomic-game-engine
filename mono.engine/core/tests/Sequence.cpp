#include <engine/core/types/Sequence.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cstring>
#include <type_traits>

TEST_SUITE_ID("engine.core.types.sequence")

using Catch::Approx;
using engine::core::Color3;
using engine::core::ColorKeypoint;
using engine::core::ColorSequence;
using engine::core::NumberKeypoint;
using engine::core::NumberSequence;
using engine::core::SEQUENCE_CAPACITY;

TEST_CASE("a sequence is trivially copyable, which is what buys it a column", "[sequence]") {
	// The whole reason the keypoint array is fixed. A heap-allocated list could
	// not be a component, could not go into a snapshot as its object
	// representation, and could not cross a bus as bytes.
	STATIC_REQUIRE(std::is_trivially_copyable_v<NumberSequence>);
	STATIC_REQUIRE(std::is_trivially_copyable_v<ColorSequence>);
}

TEST_CASE("the unused tail is zeroed rather than left alone", "[sequence]") {
	// Uninitialised bytes past `Count` still reach a snapshot, and that is the
	// failure `ecs::WorldTime` found the expensive way - two runs of one scene
	// producing different files.
	NumberSequence first;
	NumberSequence second;
	first.Add(NumberKeypoint{0.0f, 1.0f});
	second.Add(NumberKeypoint{0.0f, 1.0f});

	REQUIRE(std::memcmp(&first, &second, sizeof(NumberSequence)) == 0);

	ColorSequence firstColour;
	ColorSequence secondColour;
	firstColour.Add(ColorKeypoint{0.0f, Color3{1.0f, 0.0f, 0.0f}});
	secondColour.Add(ColorKeypoint{0.0f, Color3{1.0f, 0.0f, 0.0f}});

	REQUIRE(std::memcmp(&firstColour, &secondColour, sizeof(ColorSequence)) == 0);
}

TEST_CASE("the constant form is flat everywhere", "[sequence]") {
	const NumberSequence flat{7.0f};

	REQUIRE(flat.Count == 2);
	REQUIRE(flat.Evaluate(0.0f) == Approx(7.0f));
	REQUIRE(flat.Evaluate(0.5f) == Approx(7.0f));
	REQUIRE(flat.Evaluate(1.0f) == Approx(7.0f));
}

TEST_CASE("a ramp interpolates between its ends", "[sequence]") {
	const NumberSequence ramp{0.0f, 10.0f};

	REQUIRE(ramp.Evaluate(0.0f) == Approx(0.0f));
	REQUIRE(ramp.Evaluate(0.25f) == Approx(2.5f));
	REQUIRE(ramp.Evaluate(0.5f) == Approx(5.0f));
	REQUIRE(ramp.Evaluate(1.0f) == Approx(10.0f));
}

TEST_CASE("evaluation clamps outside the span rather than extrapolating", "[sequence]") {
	// The opposite of `Lerp`, and deliberately. A gradient has a first and a
	// last stop; there is no value before the first one to extrapolate from.
	const NumberSequence ramp{2.0f, 8.0f};

	REQUIRE(ramp.Evaluate(-1.0f) == Approx(2.0f));
	REQUIRE(ramp.Evaluate(2.0f) == Approx(8.0f));
}

TEST_CASE("three keypoints interpolate within the right segment", "[sequence]") {
	NumberSequence sequence;
	sequence.Add(NumberKeypoint{0.0f, 0.0f});
	sequence.Add(NumberKeypoint{0.25f, 100.0f});
	sequence.Add(NumberKeypoint{1.0f, 0.0f});

	REQUIRE(sequence.Evaluate(0.125f) == Approx(50.0f));
	REQUIRE(sequence.Evaluate(0.25f) == Approx(100.0f));
	// Halfway through the second, longer segment.
	REQUIRE(sequence.Evaluate(0.625f) == Approx(50.0f));
}

TEST_CASE("two keypoints at one time are a step, not a divide by zero", "[sequence]") {
	NumberSequence sequence;
	sequence.Add(NumberKeypoint{0.0f, 0.0f});
	sequence.Add(NumberKeypoint{0.5f, 1.0f});
	sequence.Add(NumberKeypoint{0.5f, 9.0f});
	sequence.Add(NumberKeypoint{1.0f, 9.0f});

	// The value on the far side of the step wins. A NaN here would surface
	// wherever the sequence was consumed rather than where it was built.
	REQUIRE(sequence.Evaluate(0.5f) == Approx(9.0f));
	REQUIRE(sequence.Evaluate(0.25f) == Approx(0.5f));
	REQUIRE(sequence.Evaluate(0.75f) == Approx(9.0f));
}

TEST_CASE("an empty sequence evaluates to a default instead of reading past the end", "[sequence]") {
	REQUIRE(NumberSequence{}.Evaluate(0.5f) == Approx(0.0f));

	const Color3 black = ColorSequence{}.Evaluate(0.5f);
	REQUIRE(black.R == Approx(0.0f));
	REQUIRE(black.G == Approx(0.0f));
	REQUIRE(black.B == Approx(0.0f));
}

TEST_CASE("adding past the capacity is refused rather than silently dropped", "[sequence]") {
	NumberSequence sequence;
	for (uint32_t index = 0; index < SEQUENCE_CAPACITY; index++) {
		const float time = static_cast<float>(index) / static_cast<float>(SEQUENCE_CAPACITY - 1);
		REQUIRE(sequence.Add(NumberKeypoint{time, static_cast<float>(index)}));
	}

	REQUIRE(sequence.Count == SEQUENCE_CAPACITY);
	REQUIRE_FALSE(sequence.Add(NumberKeypoint{1.0f, 999.0f}));
	REQUIRE(sequence.Count == SEQUENCE_CAPACITY);
	REQUIRE(sequence.Evaluate(1.0f) == Approx(static_cast<float>(SEQUENCE_CAPACITY - 1)));
}

TEST_CASE("a colour ramp blends every channel", "[sequence]") {
	const ColorSequence ramp{Color3{1.0f, 0.0f, 0.0f}, Color3{0.0f, 0.0f, 1.0f}};
	const Color3 middle = ramp.Evaluate(0.5f);

	REQUIRE(middle.R == Approx(0.5f));
	REQUIRE(middle.G == Approx(0.0f));
	REQUIRE(middle.B == Approx(0.5f));
}

TEST_CASE("a colour step resolves the same way a number step does", "[sequence]") {
	// The rule has to be identical in both, because one authored table can
	// produce a number ramp and a colour ramp and they must agree about where
	// the edges are.
	ColorSequence sequence;
	sequence.Add(ColorKeypoint{0.0f, Color3{1.0f, 0.0f, 0.0f}});
	sequence.Add(ColorKeypoint{0.5f, Color3{1.0f, 0.0f, 0.0f}});
	sequence.Add(ColorKeypoint{0.5f, Color3{0.0f, 1.0f, 0.0f}});
	sequence.Add(ColorKeypoint{1.0f, Color3{0.0f, 1.0f, 0.0f}});

	const Color3 atSeam = sequence.Evaluate(0.5f);
	REQUIRE(atSeam.R == Approx(0.0f));
	REQUIRE(atSeam.G == Approx(1.0f));

	// And a hair either side reads the flat segment it belongs to.
	REQUIRE(sequence.Evaluate(0.49f).R == Approx(1.0f));
	REQUIRE(sequence.Evaluate(0.51f).G == Approx(1.0f));
}

TEST_CASE("equality compares the used keypoints and not the tail", "[sequence]") {
	NumberSequence first;
	first.Add(NumberKeypoint{0.0f, 1.0f});
	first.Add(NumberKeypoint{1.0f, 2.0f});

	NumberSequence second{1.0f, 2.0f};
	REQUIRE(first == second);

	second.Add(NumberKeypoint{1.0f, 3.0f});
	REQUIRE_FALSE(first == second);
}

TEST_CASE("the envelope is carried and not consumed by evaluation", "[sequence]") {
	// Sampling inside the band needs a generator, and which generator is the
	// caller's decision - a sequence reaching for a global one is exactly the
	// determinism hazard `core::Random` exists to avoid.
	NumberSequence sequence;
	sequence.Add(NumberKeypoint{0.0f, 5.0f, 2.0f});
	sequence.Add(NumberKeypoint{1.0f, 5.0f, 2.0f});

	REQUIRE(sequence.Keypoints[0].Envelope == Approx(2.0f));
	REQUIRE(sequence.Evaluate(0.5f) == Approx(5.0f));
}
