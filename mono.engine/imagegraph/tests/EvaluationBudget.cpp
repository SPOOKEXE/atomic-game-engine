#include "../src/EvaluationBudget.hpp"

#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <limits>
#include <memory_resource>
#include <vector>

TEST_SUITE_ID("engine.imagegraph.evaluation_budget")
using engine::imagegraph::detail::EvaluationBudget;

TEST_CASE(
	"live previous output and scratch reservations reject an overlapping clone",
	"[imagegraph][evaluation_budget]"
) {
	EvaluationBudget budget(96);
	auto previousCharge = budget.Reserve(32);
	REQUIRE(previousCharge);
	std::vector<double> previous{1, 2, 3, 4};
	auto publishedCharge = budget.Reserve(32);
	REQUIRE(publishedCharge);
	std::vector<double> published{9, 8, 7, 6};
	{
		auto scratchCharge = budget.Reserve(32);
		REQUIRE(scratchCharge);
		std::vector<double> scratch(previous);
		CHECK_FALSE(budget.Reserve(32));
		CHECK(published == std::vector<double>{9, 8, 7, 6});
		CHECK(previous == std::vector<double>{1, 2, 3, 4});
		CHECK(budget.Used() == 96);
	}
	CHECK(budget.Used() == 64);
	{
		auto nextCharge = budget.Reserve(32);
		REQUIRE(nextCharge);
		std::vector<double> next(previous);
		CHECK(next == previous);
		CHECK(budget.Used() == 96);
	}
	CHECK(budget.Peak() == 96);
}
TEST_CASE(
	"reservation growth overflow refusal and ownership transfer keep accounting balanced",
	"[imagegraph][evaluation_budget]"
) {
	EvaluationBudget budget(std::numeric_limits<uint64_t>::max());
	auto owned = budget.Reserve(std::numeric_limits<uint64_t>::max() - 1);
	REQUIRE(owned);
	CHECK_FALSE(budget.Reserve(2));
	CHECK(budget.Available() == 1);
	REQUIRE(owned->Resize(std::numeric_limits<uint64_t>::max()));
	CHECK_FALSE(budget.Reserve(1));
	auto split = owned->Split(17);
	REQUIRE(split);
	CHECK(budget.Used() == std::numeric_limits<uint64_t>::max());
	REQUIRE(owned->Merge(std::move(*split)));
	CHECK(split->Bytes() == 0);
	REQUIRE(owned->Resize(17));
	CHECK(budget.Used() == 17);
	EvaluationBudget other(17);
	auto foreign = other.Reserve(17);
	REQUIRE(foreign);
	CHECK_FALSE(owned->Merge(std::move(*foreign)));
	CHECK(other.Used() == 17);
	auto moved = std::move(*owned);
	CHECK(owned->Bytes() == 0);
	moved.Reset();
	CHECK(budget.Used() == 0);
	CHECK(other.Used() == 17);
}
TEST_CASE(
	"self transfer preserves a live buffer charge until its storage is freed",
	"[imagegraph][evaluation_budget]"
) {
	EvaluationBudget budget(32);
	auto charge = budget.Reserve(32);
	REQUIRE(charge);
	{
		std::vector<double> buffer{1, 2, 3, 4};
		REQUIRE(charge->Merge(std::move(*charge)));
		CHECK(charge->Bytes() == 32);
		CHECK(budget.Used() == 32);
		CHECK_FALSE(budget.Reserve(1));
		CHECK(buffer == std::vector<double>{1, 2, 3, 4});
	}
	charge->Reset();
	CHECK(budget.Used() == 0);
	CHECK(budget.Peak() == 32);
}
TEST_CASE(
	"real failed allocation unwinds its reservation and preserves a published buffer",
	"[imagegraph][evaluation_budget]"
) {
	EvaluationBudget budget(64);
	auto publishedCharge = budget.Reserve(32);
	REQUIRE(publishedCharge);
	std::vector<double> published{1, 2, 3, 4};
	try {
		auto candidateCharge = budget.Reserve(32);
		REQUIRE(candidateCharge);
		// No upstream memory is available: this is an actual allocator refusal, not a simulated copy flag.
		std::pmr::monotonic_buffer_resource storage(0, std::pmr::null_memory_resource());
		std::pmr::vector<double> candidate(published.begin(), published.end(), &storage);
		FAIL("the zero-capacity resource must refuse allocation");
	} catch (const std::bad_alloc &) {
		CHECK(budget.Used() == 32);
		CHECK(published == std::vector<double>{1, 2, 3, 4});
	}
	CHECK(budget.Peak() == 64);
}
