#include "Random.hpp"

#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

TEST_SUITE_ID("engine.procedural.random")

using engine::procedural::CounterWord;

TEST_CASE("named counter streams have fixed version-one vectors", "[procedural][random]") {
	CHECK(CounterWord(0, "forest.position.x", 0, 0) == 0xcfd5439b7d982424ull);
	CHECK(CounterWord(42, "house.variant", -3, 7) == 0x074807d09440e0c5ull);
	CHECK(
		CounterWord(0x0123456789abcdefull, "road.bend/market-west", -2147483648ll, 2147483647ll, 9) ==
		0xc93488febfcd7f9full
	);
	CHECK(CounterWord(42, "house.variant", -3, 7, 1) == 0xde54317b1850bb64ull);
}

TEST_CASE("stream, coordinate and counter identify independent choices", "[procedural][random]") {
	const auto word = CounterWord(42, "house.variant", -3, 7);
	CHECK(CounterWord(42, "house.material", -3, 7) != word);
	CHECK(CounterWord(42, "house.variant", -2, 7) != word);
	CHECK(CounterWord(42, "house.variant", -3, 7, 1) != word);
}
