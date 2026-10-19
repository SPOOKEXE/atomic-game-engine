#include "PairSort.hpp"

#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <limits>
#include <vector>

TEST_SUITE_ID("engine.physics.pair-sort")
TEST_DEPENDS("engine.physics.broadphase")

TEST_CASE("pair radix order preserves full ids and stable collider sources", "[physics][pair-sort]") {
	using namespace engine;
	std::vector<std::vector<uint64_t>> cases{
		{}, {17}, {7, 7, 7}, {UINT64_MAX, 0, uint64_t{1} << 63, uint64_t{1} << 32, 1}
	};
	for (size_t bit = 0; bit < 64; bit++) {
		const uint64_t common = ~(uint64_t{1} << bit);
		cases.push_back({UINT64_MAX, common, common, UINT64_MAX});
	}
	uint64_t randomId = 0x9e3779b97f4a7c15ULL;
	std::vector<uint64_t> random;
	for (size_t index = 0; index < 4096; index++) {
		randomId ^= randomId << 13;
		randomId ^= randomId >> 7;
		randomId ^= randomId << 17;
		random.push_back(randomId);
	}
	cases.push_back(random);
	std::vector<physics::SourcedPair> scratch;
	for (const auto &ids : cases) {
		CAPTURE(ids.size());
		std::vector<physics::SourcedPair> pairs;
		for (size_t index = 0; index < ids.size(); index++) {
			for (uint32_t duplicate = 0; duplicate < 2; duplicate++) {
				pairs.push_back(
					{{ecs::Entity(ids[index]), ecs::Entity(ids[ids.size() - index - 1])},
					 {static_cast<uint32_t>(index), duplicate | physics::CandidateSource::STATIC}}
				);
			}
		}
		auto expected = pairs;
		std::stable_sort(expected.begin(), expected.end());
		physics::RadixSortPairMember(pairs, scratch, [](const physics::SourcedPair &pair) {
			return pair.Pair.B.Id;
		});
		physics::RadixSortPairMember(pairs, scratch, [](const physics::SourcedPair &pair) {
			return pair.Pair.A.Id;
		});
		REQUIRE(pairs.size() == expected.size());
		for (size_t index = 0; index < pairs.size(); index++) {
			CHECK(pairs[index].Pair == expected[index].Pair);
			CHECK(pairs[index].Source.First == expected[index].Source.First);
			CHECK(pairs[index].Source.Second == expected[index].Source.Second);
		}
	}
}

TEST_CASE("pair radix sorts secondary ids and accepts one row", "[physics][pair-sort]") {
	using namespace engine;
	std::vector<physics::SourcedPair> pairs{
		{{ecs::Entity(7), ecs::Entity(UINT64_MAX)}, {0, 0}},
		{{ecs::Entity(7), ecs::Entity(3)}, {1, 0}},
		{{ecs::Entity(2), ecs::Entity(uint64_t{1} << 63)}, {2, 0}},
		{{ecs::Entity(7), ecs::Entity(uint64_t{1} << 32)}, {3, 0}},
		{{ecs::Entity(7), ecs::Entity(3)}, {4, physics::CandidateSource::STATIC}},
		{{ecs::Entity(2), ecs::Entity(5)}, {5, 0}},
	};
	auto expected = pairs;
	std::stable_sort(expected.begin(), expected.end());
	std::vector<physics::SourcedPair> scratch;
	const auto sort = [&] {
		physics::RadixSortPairMember(pairs, scratch, [](const physics::SourcedPair &pair) {
			return pair.Pair.B.Id;
		});
		physics::RadixSortPairMember(pairs, scratch, [](const physics::SourcedPair &pair) {
			return pair.Pair.A.Id;
		});
	};
	sort();
	for (size_t index = 0; index < pairs.size(); index++) {
		CHECK(pairs[index].Pair == expected[index].Pair);
		CHECK(pairs[index].Source.First == expected[index].Source.First);
		CHECK(pairs[index].Source.Second == expected[index].Source.Second);
	}
	pairs = {pairs.front()};
	const auto singleton = pairs.front();
	sort();
	REQUIRE(pairs.size() == 1);
	CHECK(pairs.front().Pair == singleton.Pair);
	CHECK(pairs.front().Source.First == singleton.Source.First);
}
