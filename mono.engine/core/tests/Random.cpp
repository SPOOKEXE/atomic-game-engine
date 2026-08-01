#include <engine/core/Random.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <set>
#include <vector>

TEST_SUITE_ID("engine.core.random")

using engine::core::Random;

// The property the whole thing exists for. A generator that drifts between runs
// or between machines does not fail loudly — a replay just diverges, and a
// server and client disagree about where something is. Pinning the actual
// numbers is the only check that catches it.
//
// These were not read back out of this implementation, which would only prove
// it agrees with itself. They are the first four bytes of the SHA-256 of the
// eight-byte big-endian (index, salt), computed with Python's hashlib:
//
//     python3 -c "import hashlib, struct
//     print(hashlib.sha256(struct.pack('>II', 1, 2)).hexdigest()[:8])"
//
// So this also checks the byte order the input is assembled in, which is the
// part a big-endian build would otherwise get silently wrong.
TEST_CASE("the values are pinned", "[random]") {
	REQUIRE(Random::Bits(0u, 0u) == 0xaf5570f5u);
	REQUIRE(Random::Bits(1u, 2u) == 0x0f585dd5u);
	REQUIRE(Random::Bits(20000u, 31u) == 0xcc355280u);
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
	// than that the distribution is good — SHA-256's is not in question.
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
