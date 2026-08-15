#include <engine/core/Random.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <iterator>
#include <set>
#include <vector>

TEST_SUITE_ID("engine.core.random")

using engine::core::Random;

// The property the whole thing exists for. A generator that drifts between runs
// or between machines does not fail loudly - a replay just diverges, and a
// server and client disagree about where something is. Pinning the actual
// numbers is the only check that catches it, and `just determinism` and
// `just replay-check` cannot: both compare one run against another, so both
// stay green on a generator that changed and agrees with itself.
//
// **The first pin is a published constant, not a number read out of this file.**
// SplitMix64 seeded with zero emits 0xE220A8397B1DCDAF first - the value every
// reference implementation of it prints, and the one to check this against if
// the constants below are ever edited. `Bits(0, 0)` is its top half.
//
// The rest were computed from a transcription of Vigna's public-domain
// `splitmix64.c` into Python, which is a different language and a different
// author's arithmetic:
//
//     M = (1 << 64) - 1
//     def mix(z):
//         z = (z + 0x9E3779B97F4A7C15) & M
//         z = ((z ^ (z >> 30)) * 0xBF58476D1CE4E5B9) & M
//         z = ((z ^ (z >> 27)) * 0x94D049BB133111EB) & M
//         return z ^ (z >> 31)
//     def bits(index, salt):
//         return (mix(((salt << 32) | index) & M) >> 32) & 0xFFFFFFFF
//
// So this also checks how the pair is packed into the 64-bit input, which is the
// part a rewrite would otherwise be free to get wrong.
TEST_CASE("the values are pinned", "[random]") {
	REQUIRE(Random::Bits(0u, 0u) == 0xe220a839u);
	REQUIRE(Random::Bits(1u, 2u) == 0xc4858308u);
	REQUIRE(Random::Bits(20000u, 31u) == 0xb314ee8au);
}

TEST_CASE("a seed's whole stream is pinned", "[random]") {
	// The shape `Random.new(seed)` draws in both script VMs: one salt, a counter
	// for an index. One pinned value proves the mixer; a run of them proves the
	// *sequence*, which is what a game and a recording actually depend on.
	static constexpr uint32_t SEED = 12345u;
	static constexpr uint32_t EXPECTED[] = {
		0x8f190afcu,
		0x6804e9a3u,
		0x9cfe9968u,
		0x44878854u,
		0xee2c2c39u,
		0x19bcfc30u,
		0x90928b6bu,
		0x450396b8u,
	};

	for (uint32_t index = 0; index < std::size(EXPECTED); index++) {
		REQUIRE(Random::Bits(index, SEED) == EXPECTED[index]);
	}

	// And the float conversion, exactly rather than within a tolerance. The
	// result is a 24-bit integer scaled by a power of two, so it is representable
	// without rounding and an approximate comparison here would hide the one
	// failure this can have: a build whose last bit disagrees.
	REQUIRE(Random::Float(0u, SEED) == 0x1.1e3214p-1f);
	REQUIRE(Random::Float(1u, SEED) == 0x1.a013a4p-2f);
}

TEST_CASE("it is a pure function of its arguments", "[random]") {
	// Indexed rather than streamed: no call affects any other. This is what
	// makes it safe from a parallel spawn, or from one that skips entities.
	const float first = Random::Float(7u, 11u);
	for (uint32_t index = 0; index < 100u; index++) {
		Random::Float(index, 3u);
	}
	REQUIRE(Random::Float(7u, 11u) == first);
}

TEST_CASE("the range is half-open", "[random]") {
	// The mixer this replaced divided by 0xFFFFFFFF and so could return exactly
	// 1.0f, which a caller scaling into an array index would find eventually.
	for (uint32_t index = 0; index < 5000u; index++) {
		const float value = Random::Float(index, 1u);
		REQUIRE(value >= 0.0f);
		REQUIRE(value < 1.0f);
	}
}

TEST_CASE("different salts give different sequences", "[random]") {
	// Position wants three values from one index and a colour wants three more.
	// If salts collided, an entity's x and y would move together.
	std::set<uint32_t> seen;
	for (uint32_t salt = 0; salt < 64u; salt++) {
		seen.insert(Random::Bits(42u, salt));
	}
	REQUIRE(seen.size() == 64u);
}

TEST_CASE("consecutive indices do not repeat", "[random]") {
	// The failure a counter-based mixer has that a hash does not: a weak
	// finaliser leaves neighbouring inputs correlated, and a stream drawn by an
	// incrementing counter is nothing but neighbouring inputs. Four thousand in a
	// row, all distinct - at this count even a fair 32-bit generator is expected
	// to collide about once in five hundred runs, so a repeat here is structure
	// rather than luck.
	std::set<uint32_t> seen;
	for (uint32_t index = 0; index < 4096u; index++) {
		seen.insert(Random::Bits(index, 7u));
	}
	REQUIRE(seen.size() == 4096u);
}

TEST_CASE("Range spans the bounds it is given", "[random]") {
	float lowest = 1000.0f;
	float highest = -1000.0f;
	for (uint32_t index = 0; index < 5000u; index++) {
		const float value = Random::Range(index, 5u, -20.0f, 20.0f);
		REQUIRE(value >= -20.0f);
		REQUIRE(value < 20.0f);
		lowest = value < lowest ? value : lowest;
		highest = value > highest ? value : highest;
	}
	// Loose bounds on purpose. This checks the range is actually used rather
	// than that the distribution is good, which the two cases below do.
	REQUIRE(lowest < -19.0f);
	REQUIRE(highest > 19.0f);
}

TEST_CASE("the distribution is roughly flat", "[random]") {
	// Ten buckets over ten thousand values. A mixer that lost its high bits
	// would still pass every test above and fail this one.
	std::vector<int> buckets(10, 0);
	for (uint32_t index = 0; index < 10000u; index++) {
		buckets[static_cast<size_t>(Random::Float(index, 97u) * 10.0f)]++;
	}
	for (const int count : buckets) {
		REQUIRE(count > 850);
		REQUIRE(count < 1150);
	}
}

TEST_CASE("every bit is balanced", "[random]") {
	// The bucket case above only ever looks at the top eight bits, because that
	// is all `Float` keeps. This looks at all thirty-two, which is what `Bits`
	// hands out - a stuck or duplicated bit low down would be invisible to every
	// other case here and would show up in a caller as a `% 3` that never
	// returns two.
	int setCount[32] = {};
	static constexpr uint32_t SAMPLES = 20000u;
	for (uint32_t index = 0; index < SAMPLES; index++) {
		const uint32_t value = Random::Bits(index, 101u);
		for (uint32_t bit = 0; bit < 32u; bit++) {
			setCount[bit] += static_cast<int>((value >> bit) & 1u);
		}
	}
	for (const int count : setCount) {
		REQUIRE(count > 9700);
		REQUIRE(count < 10300);
	}
}
