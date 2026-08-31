#include <engine/ecs/Determinism.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <string>

TEST_SUITE_ID("engine.ecs.determinism")

using namespace engine::ecs;

namespace determinism_test {
	struct Counter {
		int Value = 0;
	};

	struct ProcessLocal {
		std::string Value;
		ProcessLocal() = default;
	};
}

using namespace determinism_test;

TEST_CASE("whole-world fingerprints detect replay divergence", "[ecs][determinism]") {
	Store store("simulation");
	const Entity entity = store.Create();
	store.Set<Counter>(entity, Counter{7});
	DeterminismTracker tracker;

	const DeterminismObservation recorded = tracker.Observe(store, 20);
	REQUIRE(recorded.Result == DeterminismResult::Recorded);
	CHECK(recorded.Expected == recorded.Actual);
	CHECK(tracker.Observe(store, 20).Result == DeterminismResult::Match);

	store.Set<Counter>(entity, Counter{8});
	const DeterminismObservation diverged = tracker.Observe(store, 20);
	CHECK(diverged.Result == DeterminismResult::Diverged);
	CHECK(diverged.Expected != diverged.Actual);
}

TEST_CASE("fingerprint baselines follow rollback branches", "[ecs][determinism]") {
	Store store("simulation");
	DeterminismTracker tracker;
	CHECK(tracker.Observe(store, 1).Result == DeterminismResult::Recorded);
	CHECK(tracker.Observe(store, 2).Result == DeterminismResult::Recorded);
	CHECK(tracker.BaselineCount() == 2);

	tracker.ForgetAfter(1);
	CHECK(tracker.BaselineCount() == 1);
	CHECK(tracker.Observe(store, 2).Result == DeterminismResult::Recorded);
	tracker.Clear();
	CHECK(tracker.BaselineCount() == 0);
}

TEST_CASE("determinism checks can be disabled or refuse unsafe state", "[ecs][determinism]") {
	Store store("simulation");
	DeterminismTracker disabled(false);
	CHECK(disabled.Observe(store, 1).Result == DeterminismResult::Disabled);
	CHECK(disabled.BaselineCount() == 0);

	store.Set<ProcessLocal>(store.Create(), ProcessLocal{});
	DeterminismTracker enabled;
	CHECK(enabled.Observe(store, 1).Result == DeterminismResult::Unsnapshotable);
	CHECK(enabled.BaselineCount() == 0);
}
