#include "../src/EvaluationAllocator.hpp"

#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

TEST_SUITE_ID("engine.imagegraph.evaluation_allocator")
using namespace engine::imagegraph::detail;

TEST_CASE(
	"scratch allocator retains old hash buckets until replacement succeeds", "[imagegraph][evaluation_budget]"
) {
	std::array<std::string, 32> names;
	for (size_t index = 0; index < names.size(); ++index)
		names[index] = "key" + std::to_string(index);
	using Entry = std::pair<const std::string_view, size_t>;
	using Map = std::unordered_map<
		std::string_view,
		size_t,
		std::hash<std::string_view>,
		std::equal_to<std::string_view>,
		EvaluationAllocator<Entry>>;
	uint64_t peak = 0;
	{
		EvaluationBudget budget(1'000'000);
		auto previousCharge = budget.Reserve(32);
		REQUIRE(previousCharge);
		std::vector<double> previous{1, 2, 3, 4};
		Map map(
			0,
			std::hash<std::string_view>{},
			std::equal_to<std::string_view>{},
			EvaluationAllocator<Entry>(budget)
		);
		map.reserve(8);
		for (size_t index = 0; index < names.size(); ++index)
			map.emplace(names[index], index);
		const uint64_t oldBytes = budget.Used();
		map.rehash(map.bucket_count() * 8);
		peak = budget.Peak();
		CHECK(peak > budget.Used());
		CHECK(budget.Used() > oldBytes);
	}
	for (const uint64_t limit : {peak - 1, peak}) {
		EvaluationBudget budget(limit);
		auto previousCharge = budget.Reserve(32);
		REQUIRE(previousCharge);
		std::vector<double> previous{1, 2, 3, 4};
		{
			Map map(
				0,
				std::hash<std::string_view>{},
				std::equal_to<std::string_view>{},
				EvaluationAllocator<Entry>(budget)
			);
			map.reserve(8);
			for (size_t index = 0; index < names.size(); ++index)
				map.emplace(names[index], index);
			const uint64_t oldBytes = budget.Used();
			const size_t buckets = map.bucket_count();
			if (limit < peak) {
				CHECK_THROWS_AS(map.rehash(buckets * 8), std::bad_alloc);
				CHECK(map.bucket_count() == buckets);
				CHECK(budget.Used() == oldBytes);
			} else {
				map.rehash(buckets * 8);
				CHECK(budget.Peak() == peak);
			}
			REQUIRE(map.size() == names.size());
			for (size_t index = 0; index < names.size(); ++index)
				CHECK(map.at(names[index]) == index);
			CHECK(previous == std::vector<double>{1, 2, 3, 4});
		}
		CHECK(budget.Used() == 32);
	}
}

TEST_CASE(
	"scratch allocator rebind identity and allocation throw preserve the ledger",
	"[imagegraph][evaluation_budget]"
) {
	EvaluationBudget first(std::numeric_limits<uint64_t>::max()), second(1024);
	EvaluationAllocator<double> allocator(first);
	EvaluationAllocator<int> rebound(allocator), other(second);
	CHECK(rebound == allocator);
	CHECK_FALSE(other == allocator);
	CHECK_THROWS_AS(
		allocator.allocate(std::numeric_limits<size_t>::max() / sizeof(double) + 1), std::bad_array_new_length
	);
	CHECK(first.Used() == 0);
	// This request fits the ledger multiplication but cannot fit the platform address space.
	CHECK_THROWS_AS(allocator.allocate(std::numeric_limits<size_t>::max() / sizeof(double)), std::bad_alloc);
	CHECK(first.Used() == 0);
}
