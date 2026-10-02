#include "../src/SourceRandom.hpp"

#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>
TEST_SUITE_ID("engine.imagegraph.source_random")
using engine::imagegraph::detail::SourceRandom;
TEST_CASE(
	"Source random matches official HTML5 seed and double draw integer vectors", "[imagegraph][source_random]"
) {
	struct Vector {
		uint32_t Seed;
		std::array<uint32_t, 6> Indices;
		std::array<double, 4> Units;
	};
	const Vector vectors[]{
		{0,
		 {4, 3, 5, 2, 5, 6},
		 {.6074330581386728, .9255384131919305, .5099332265136453, .29481496722195993}},
		{1,
		 {1, 5, 0, 3, 2, 4},
		 {.18299215574888147, .8061879765271153, .7674706847255447, .7811648402275354}},
		{uint32_t(-1),
		 {1, 5, 0, 3, 2, 2},
		 {.18069155382955984, .1853037314421049, .8567445170398543, .5886388847551489}},
		{12345,
		 {5, 0, 5, 2, 3, 1},
		 {.7990098701785364, .25161494698916326, .015874267097504002, .9563575926964905}},
		{0x80000000,
		 {6, 6, 1, 4, 4, 5},
		 {.9293240103541519, .6054015371973633, .992209569547423, .9154383767933764}}
	};
	for (const auto &vector : vectors) {
		SourceRandom unit(vector.Seed), index(vector.Seed);
		for (double expected : vector.Units)
			CHECK(unit.Unit() == expected);
		for (uint32_t expected : vector.Indices)
			CHECK(index.Index(7) == expected);
	}
}
TEST_CASE(
	"Source range preserves equal bounds no draw and unequal bounds two draws", "[imagegraph][source_random]"
) {
	SourceRandom random(12345);
	CHECK(random.Range(2, 2) == 2);
	CHECK(random.Range(20, 10) == 17.990098701785364);
	CHECK(random.Unit() == .015874267097504002);
}
