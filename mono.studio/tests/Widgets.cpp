// The searching the class picker and both filter boxes do.
//
// **The only part of this program a headless test can reach**, and it happens to
// be the part with a real algorithm in it. Everything else here needs a window,
// a device and an imgui frame — which is why `mono.studio/AGENTS.md` carries the
// invariants that a test cannot, and why the run cycle is tested one layer down
// in `mono.client/tests/Presentation.cpp` where no GPU is involved.

#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <studio/Widgets.hpp>

TEST_SUITE_ID("studio.widgets")

using studio::FuzzyMatch;

TEST_CASE("a subsequence matches and a missing letter does not", "[studio][widgets]") {
	int score = 0;

	// The case a substring match gets wrong, and the reason this is not
	// `find()`: typing "bp" should find "BasePart". An editor whose search only
	// matches what you already spelled correctly is an editor you have to know
	// the answer to use.
	CHECK(FuzzyMatch("bp", "BasePart", score));
	CHECK(FuzzyMatch("part", "BasePart", score));
	CHECK(FuzzyMatch("BASEPART", "basepart", score));

	CHECK_FALSE(FuzzyMatch("bpz", "BasePart", score));
	CHECK_FALSE(FuzzyMatch("pb", "BasePart", score));
}

TEST_CASE("an empty query matches everything", "[studio][widgets]") {
	// What an unfiltered list is. A picker that showed nothing until somebody
	// typed would hide the whole palette behind knowing a name.
	int score = 0;
	CHECK(FuzzyMatch("", "Part", score));
	CHECK(FuzzyMatch("", "", score));
}

TEST_CASE("an exact name outranks everything that merely contains it", "[studio][widgets]") {
	// Typing a full class name has to put that class first, however many other
	// classes happen to contain those letters in order.
	int exact = 0;
	int scattered = 0;

	REQUIRE(FuzzyMatch("Part", "Part", exact));
	REQUIRE(FuzzyMatch("Part", "ParticleEmitterAttachment", scattered));
	CHECK(exact > scattered);
}

TEST_CASE("a prefix outranks a match that starts later", "[studio][widgets]") {
	int prefix = 0;
	int later = 0;

	REQUIRE(FuzzyMatch("Loc", "LocalScript", prefix));
	REQUIRE(FuzzyMatch("Loc", "BlockLocator", later));
	CHECK(prefix > later);
}

TEST_CASE("consecutive letters score above scattered ones", "[studio][widgets]") {
	// What makes the ordering feel right rather than merely correct: "spa"
	// should prefer a name where those three letters are together.
	int together = 0;
	int apart = 0;

	REQUIRE(FuzzyMatch("spa", "SpawnLocation", together));
	REQUIRE(FuzzyMatch("spa", "SurfaceParticleAnchor", apart));
	CHECK(together > apart);
}
