#include <engine/parallel/Jobs.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <IndexResidency.hpp>
#include <array>
#include <numeric>
#include <vector>

TEST_SUITE_ID("engine.render.indexresidency")

using engine::render::IndexResidency;

namespace {
	struct RunningJobs {
		RunningJobs() {
			engine::parallel::Jobs::Start(4);
		}
		~RunningJobs() {
			engine::parallel::Jobs::Stop();
		}
	};
}

TEST_CASE("an unchanged camera whitelist submits no resident index rows", "[render][residency]") {
	IndexResidency indices;
	const std::array<uint32_t, 5> whitelist{8, 3, 5, 1, 13};

	indices.Plan(0, whitelist, true);
	REQUIRE(indices.DirtyRanges().size() == 1);
	CHECK(indices.DirtyCount() == whitelist.size());
	indices.Acknowledge();

	indices.Plan(0, whitelist, false);
	CHECK(indices.DirtyRanges().empty());
	CHECK(indices.DirtyCount() == 0);
}

TEST_CASE("one camera whitelist edit submits only that resident index", "[render][residency]") {
	IndexResidency indices;
	std::array<uint32_t, 5> whitelist{8, 3, 5, 1, 13};

	indices.Plan(0, whitelist, true);
	indices.Acknowledge();
	whitelist[2] = 21;
	indices.Plan(0, whitelist, false);

	REQUIRE(indices.DirtyRanges().size() == 1);
	CHECK(indices.DirtyRanges()[0].First == 2);
	CHECK(indices.DirtyRanges()[0].Count == 1);
	CHECK(indices.DirtyCount() == 1);
}

TEST_CASE("in-flight camera whitelists are acknowledged independently", "[render][residency]") {
	IndexResidency indices;
	const std::array<uint32_t, 3> whitelist{4, 7, 9};

	for (uint32_t version = 0; version < IndexResidency::VERSIONS; version++) {
		indices.Plan(version, whitelist, false);
		CHECK(indices.DirtyCount() == whitelist.size());
		indices.Acknowledge();
	}

	for (uint32_t version = 0; version < IndexResidency::VERSIONS; version++) {
		indices.Plan(version, whitelist, false);
		CHECK(indices.DirtyCount() == 0);
		indices.Acknowledge();
	}
}

TEST_CASE("growth and shrink keep only live camera whitelist rows", "[render][residency]") {
	IndexResidency indices;
	std::array<uint32_t, 4> whitelist{2, 4, 6, 8};

	indices.Plan(0, std::span<const uint32_t>(whitelist).first(2), true);
	indices.Acknowledge();
	indices.Plan(0, whitelist, false);
	REQUIRE(indices.DirtyRanges().size() == 1);
	CHECK(indices.DirtyRanges()[0].First == 2);
	CHECK(indices.DirtyRanges()[0].Count == 2);
	indices.Acknowledge();

	indices.Plan(0, std::span<const uint32_t>(whitelist).first(1), false);
	CHECK(indices.DirtyCount() == 0);
	indices.Acknowledge();
	indices.Plan(0, std::span<const uint32_t>(whitelist).first(1), false);
	CHECK(indices.DirtyCount() == 0);
}

TEST_CASE("a replaced device whitelist is rebuilt in full", "[render][residency]") {
	IndexResidency indices;
	const std::array<uint32_t, 4> whitelist{2, 4, 6, 8};

	indices.Plan(0, whitelist, true);
	indices.Acknowledge();
	indices.Plan(0, whitelist, true);

	REQUIRE(indices.DirtyRanges().size() == 1);
	CHECK(indices.DirtyRanges()[0].First == 0);
	CHECK(indices.DirtyRanges()[0].Count == whitelist.size());
}

TEST_CASE("a large camera whitelist compares in chunks without widening its delta", "[render][residency]") {
	const RunningJobs jobs;
	IndexResidency indices;
	std::vector<uint32_t> whitelist(20'000);
	std::iota(whitelist.begin(), whitelist.end(), 0u);

	indices.Plan(0, whitelist, true);
	indices.Acknowledge();
	whitelist[4095] = 91'000;
	whitelist[4096] = 92'000;
	whitelist[18'321] = 93'000;
	indices.Plan(0, whitelist, false);

	REQUIRE(indices.DirtyRanges().size() == 2);
	CHECK(indices.DirtyRanges()[0].First == 4095);
	CHECK(indices.DirtyRanges()[0].Count == 2);
	CHECK(indices.DirtyRanges()[1].First == 18'321);
	CHECK(indices.DirtyRanges()[1].Count == 1);
	CHECK(indices.DirtyCount() == 3);
}
