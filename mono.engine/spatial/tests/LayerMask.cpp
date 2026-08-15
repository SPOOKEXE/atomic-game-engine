#include <engine/spatial/LayerMask.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

TEST_SUITE_ID("engine.spatial.layermask")

using engine::spatial::LayerMask;

TEST_CASE("a default mask is empty and matches nothing", "[layermask]") {
	// The deliberate half of the two possible failures. A proxy nothing can see
	// fails the first test written against it; a proxy everything can see is a
	// trigger that fires wrongly months later.
	const LayerMask empty;

	REQUIRE(empty.Bits == 0u);
	REQUIRE(empty == LayerMask::None());
	REQUIRE_FALSE(empty.Overlaps(LayerMask::All()));
	REQUIRE_FALSE(LayerMask::All().Overlaps(empty));
}

TEST_CASE("empty is the identity for union", "[layermask]") {
	const LayerMask two = LayerMask::Only(2);

	REQUIRE((two | LayerMask::None()) == two);
	REQUIRE((LayerMask::None() | two) == two);
}

TEST_CASE("Only names one layer and All names every one", "[layermask]") {
	REQUIRE(LayerMask::Only(0).Bits == 1u);
	REQUIRE(LayerMask::Only(1).Bits == 2u);
	REQUIRE(LayerMask::Only(31).Bits == 0x80000000u);
	REQUIRE(LayerMask::All().Bits == 0xFFFFFFFFu);
}

TEST_CASE("a layer index past the last one is empty rather than undefined", "[layermask]") {
	// Shifting a 32-bit value by 32 is undefined behaviour, and which answer a
	// compiler produces for it changes with the optimisation level - so the
	// dev build and the release build would disagree about what a bad index
	// means.
	REQUIRE(LayerMask::Only(LayerMask::LAYER_COUNT) == LayerMask::None());
	REQUIRE(LayerMask::Only(64) == LayerMask::None());
	REQUIRE(LayerMask::Only(0xFFFFFFFFu) == LayerMask::None());
}

TEST_CASE("overlapping is a shared bit and not equality", "[layermask]") {
	// The mistake this catches makes every query find only proxies whose layer
	// set is identical to the mask, which looks like a broad phase that has
	// stopped working rather than like a filter that is too strict.
	const LayerMask characters = LayerMask::Only(1);
	const LayerMask props = LayerMask::Only(2);
	const LayerMask both = characters | props;

	REQUIRE(both.Overlaps(characters));
	REQUIRE(characters.Overlaps(both));
	REQUIRE_FALSE(characters.Overlaps(props));
	REQUIRE_FALSE(both == characters);
}

TEST_CASE("union and intersection do what the bits do", "[layermask]") {
	const LayerMask low = LayerMask::Only(0) | LayerMask::Only(1);
	const LayerMask high = LayerMask::Only(1) | LayerMask::Only(2);

	REQUIRE((low | high) == (LayerMask::Only(0) | LayerMask::Only(1) | LayerMask::Only(2)));
	REQUIRE((low & high) == LayerMask::Only(1));
	REQUIRE((low & LayerMask::Only(5)) == LayerMask::None());
	REQUIRE((low & LayerMask::All()) == low);
}

TEST_CASE("every layer index up to the count is its own bit", "[layermask]") {
	// Nothing collides and nothing is skipped, which is the property a set of
	// 32 layers is entirely made of.
	LayerMask accumulated;
	for (uint32_t index = 0; index < LayerMask::LAYER_COUNT; index++) {
		const LayerMask one = LayerMask::Only(index);
		REQUIRE_FALSE(accumulated.Overlaps(one));
		accumulated = accumulated | one;
	}
	REQUIRE(accumulated == LayerMask::All());
}
