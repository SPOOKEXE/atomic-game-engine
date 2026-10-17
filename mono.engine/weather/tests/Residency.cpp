#include <engine/testing/Suite.hpp>
#include <engine/weather/Residency.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <limits>

TEST_SUITE_ID("engine.weather.residency")

namespace {
	using namespace engine::weather;

	std::array<BrickDemand, 4> Demands() {
		return {
			{{{{0, 0, 0}, 2}, 8.0f, 0.01f, true},
			 {{{0, 0, 0}, 0}, 128.0f, 0.2f, false},
			 {{{8, 0, 0}, 0}, 64.0f, 0.1f, false},
			 {{{32, 0, 0}, 2}, 4.0f, 0.01f, true}}
		};
	}

	std::array<BrickKey, 2> Pages() {
		return {{{{0, 0, 0}, 0}, {{8, 0, 0}, 0}}};
	}

	PageGeneration Generate(
		ResidencyState &state,
		uint64_t tick,
		uint32_t revision,
		uint64_t generationSubmission,
		uint64_t completedSubmission
	) {
		const auto generation = ReserveGeneration(state, Pages(), tick, revision, completedSubmission);
		REQUIRE(generation);
		REQUIRE(MarkGenerationSubmitted(state, generation->Generation, generationSubmission));
		REQUIRE(PublishGeneration(state, generation->Generation, generationSubmission));
		return *generation;
	}
}

TEST_CASE("sparse budgets retain coarse coverage before optional refinement", "[weather][residency]") {
	const auto selection = SelectResidency(Demands(), 2);
	REQUIRE(selection);
	CHECK(selection->Pages.size() == 2);
	CHECK(selection->Pages[0].Lod == 2);
	CHECK(selection->Pages[1].Lod == 2);
	CHECK(selection->RefusedRefinements == 2);
	CHECK_FALSE(selection->RequiresProceduralFallback);
	const auto overflow = SelectResidency(Demands(), 1);
	REQUIRE(overflow);
	CHECK(overflow->CoarseFallbackRegions == 1);
	CHECK(overflow->RequiresProceduralFallback);
	const auto emptyBudget = SelectResidency(Demands(), 0);
	REQUIRE(emptyBudget);
	CHECK(emptyBudget->Pages.empty());
	CHECK(emptyBudget->CoarseFallbackRegions == 2);
	CHECK(SelectResidency({}, 0)->Pages.empty());
	CHECK_FALSE(SelectResidency({}, MAXIMUM_RESIDENCY_PAGES + 1));
}

TEST_CASE("multiple view demands combine by key and retain stable page order", "[weather][residency]") {
	auto demands = Demands();
	demands[3] = demands[1];
	demands[3].ProjectedPixels = 512;
	demands[3].RequiredCoarse = true;
	const auto forward = SelectResidency(demands, 3);
	std::reverse(demands.begin(), demands.end());
	const auto reverse = SelectResidency(demands, 3);
	REQUIRE(forward);
	REQUIRE(reverse);
	CHECK(forward->Pages.size() == 3);
	CHECK(reverse->Pages == forward->Pages);
	demands[0].ProjectedPixels = std::numeric_limits<float>::quiet_NaN();
	CHECK_FALSE(SelectResidency(demands, 3));
	CHECK_FALSE(ValidBrickKey({{1, 0, 0}, 0}));
	CHECK(ValidBrickKey({{-8, -8, -8}, 0}));
	CHECK_FALSE(ValidBrickKey({{}, 28}));
}

TEST_CASE(
	"optical LOD bounds override residence while visual thresholds have hysteresis", "[weather][residency]"
) {
	LodPolicy policy;
	policy.MaximumLod = 4;
	CHECK(ChooseLod(16, 0.001f, 0, 30, policy) == 4);
	CHECK(ChooseLod(16, 0.001f, 0, 0, policy) == 0);
	CHECK(ChooseLod(40, 0.001f, 0, 30, policy) == 0);
	CHECK(ChooseLod(40, 0.001f, 1, 30, policy) == 4);
	CHECK(ChooseLod(1024, 0.001f, 4, 30, policy) == 4);
	CHECK(ChooseLod(2048, 0.001f, 4, 30, policy) == 0);
	CHECK(ChooseLod(16, 0.03f, 4, 0, policy) == 0);
	CHECK(ChooseLod(16, 0.005f, 0, 30, policy) == 3);
	CHECK_FALSE(ChooseLod(-1, 0, 0, 30, policy));
	policy.RefineAbovePixels = 16;
	CHECK_FALSE(ChooseLod(1, 0, 0, 30, policy));
}

TEST_CASE("lazy envelopes include growth acceleration camera motion and actual age", "[weather][residency]") {
	const MotionEnvelope envelope{10, 2, 4, 3, 1};
	CHECK(MotionMargin(envelope, 5) == Catch::Approx(111.0f));
	CHECK(MotionMargin(envelope, 8) == Catch::Approx(201.0f));
	CHECK(MotionMargin(envelope, 0) == 1.0f);
	CHECK_FALSE(MotionMargin(envelope, -1));
	MotionEnvelope invalid = envelope;
	invalid.MaximumSpeedMetresPerSecond = std::numeric_limits<float>::infinity();
	CHECK_FALSE(MotionMargin(invalid, 5));
	invalid.MaximumSpeedMetresPerSecond = std::numeric_limits<float>::max();
	CHECK_FALSE(MotionMargin(invalid, 5));
}

TEST_CASE(
	"maintenance invalidates teleports portals and revisions before five seconds", "[weather][residency]"
) {
	std::array<ResidencyView, 2> views{{{1, {0, 0, 0}, 0}, {2, {100, 0, 0}, 3}}};
	const auto stamp = RecordMaintenance(views, 100, 7, 2);
	REQUIRE(stamp);
	CHECK_FALSE(MaintenanceRequired(*stamp, views, 199, 7, 2));
	CHECK(MaintenanceRequired(*stamp, views, 200, 7, 2));
	CHECK(MaintenanceRequired(*stamp, views, 101, 8, 2));
	CHECK(MaintenanceRequired(*stamp, views, 101, 7, 3));
	CHECK(MaintenanceRequired(*stamp, views, 99, 7, 2));
	views[0].CameraPositionMetres.X = 300;
	CHECK(MaintenanceRequired(*stamp, views, 101, 7, 2));
	views[0].CameraPositionMetres.X = 0;
	++views[1].PortalRevision;
	CHECK(MaintenanceRequired(*stamp, views, 101, 7, 2));
	views[1].PortalRevision = 3;
	std::reverse(views.begin(), views.end());
	CHECK_FALSE(MaintenanceRequired(*stamp, views, 101, 7, 2));
	views[0].Key = 9;
	CHECK(MaintenanceRequired(*stamp, views, 101, 7, 2));
	views[0].Key = views[1].Key;
	CHECK_FALSE(RecordMaintenance(views, 101, 7, 2));
	CHECK(MaintenanceRequired({}, {}, 0, 0, 0));
}

TEST_CASE("generation publishes the exact revision only after GPU completion", "[weather][residency]") {
	ResidencyState state;
	state.Slots.resize(4);
	const auto generation = ReserveGeneration(state, Pages(), 80, 7, 0);
	REQUIRE(generation);
	CHECK(state.PublishedRevision == 0);
	CHECK_FALSE(PublishGeneration(state, generation->Generation, 100));
	CHECK_FALSE(ReserveGeneration(state, Pages(), 81, 8, 0));
	CHECK_FALSE(MarkGenerationSubmitted(state, generation->Generation + 1, 4));
	CHECK_FALSE(MarkGenerationSubmitted(state, generation->Generation, 0));
	REQUIRE(MarkGenerationSubmitted(state, generation->Generation, 4));
	CHECK_FALSE(MarkGenerationSubmitted(state, generation->Generation, 5));
	CHECK_FALSE(PublishGeneration(state, generation->Generation, 3));
	CHECK(state.PublishedRevision == 0);
	REQUIRE(PublishGeneration(state, generation->Generation, 4));
	CHECK(state.PublishedRevision == 7);
	CHECK(state.PublishedTick == 80);
	CHECK(state.PendingGeneration == 0);
	CHECK_FALSE(PublishGeneration(state, generation->Generation, 4));
	const auto memory = MemoryOf(state);
	CHECK(memory.PayloadBytes == 4 * 4096);
	CHECK(memory.PublishedBytes == 2 * 4096);
	CHECK(memory.GeneratingBytes == 0);
}

TEST_CASE("inflight generations keep old pages until their last referencing fence", "[weather][residency]") {
	ResidencyState state;
	state.Slots.resize(4);
	const auto first = Generate(state, 80, 7, 4, 0);
	REQUIRE(ReferencePublished(state, 10));
	const auto second = Generate(state, 81, 7, 8, 4);
	CHECK(first.Slots != second.Slots);
	CHECK(MemoryOf(state).RetiredBytes == 2 * 4096);
	const auto nextGeneration = state.NextGeneration;
	CHECK_FALSE(ReserveGeneration(state, Pages(), 82, 7, 9));
	CHECK(state.NextGeneration == nextGeneration);
	RetireCompleted(state, 9);
	CHECK(MemoryOf(state).RetiredBytes == 2 * 4096);
	RetireCompleted(state, 10);
	CHECK(MemoryOf(state).RetiredBytes == 0);
	const auto third = ReserveGeneration(state, Pages(), 82, 9, 10);
	REQUIRE(third);
	CHECK(third->Slots == first.Slots);
	CHECK(MemoryOf(state).GeneratingBytes == 2 * 4096);
	REQUIRE(CancelGeneration(state, third->Generation));
	CHECK(MemoryOf(state).GeneratingBytes == 0);
	CHECK(state.PublishedRevision == 7);
}

TEST_CASE(
	"cancelled submitted pages wait for completion and invalid requests are atomic", "[weather][residency]"
) {
	ResidencyState state;
	state.Slots.resize(2);
	CHECK_FALSE(ReferencePublished(state, 1));
	const auto generation = ReserveGeneration(state, Pages(), 2, 3, 0);
	REQUIRE(generation);
	REQUIRE(MarkGenerationSubmitted(state, generation->Generation, 9));
	REQUIRE(CancelGeneration(state, generation->Generation));
	CHECK_FALSE(ReserveGeneration(state, Pages(), 3, 3, 8));
	CHECK(MemoryOf(state).RetiredBytes == 8192);
	CHECK_FALSE(CancelGeneration(state, generation->Generation));
	const auto next = ReserveGeneration(state, Pages(), 3, 3, 9);
	REQUIRE(next);
	REQUIRE(CancelGeneration(state, next->Generation));
	auto duplicates = Pages();
	duplicates[1] = duplicates[0];
	CHECK_FALSE(ReserveGeneration(state, duplicates, 3, 3, 9));
	CHECK(state.PendingGeneration == 0);
	state.NextGeneration = std::numeric_limits<uint64_t>::max();
	CHECK_FALSE(ReserveGeneration(state, Pages(), 3, 3, 9));
}
