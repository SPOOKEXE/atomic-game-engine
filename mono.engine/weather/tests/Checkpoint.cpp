#include <engine/testing/Suite.hpp>
#include <engine/weather/Checkpoint.hpp>

#include <catch2/catch_test_macros.hpp>

TEST_SUITE_ID("engine.weather.checkpoint")
TEST_DEPENDS("engine.weather.simulation")

using namespace engine::weather;

namespace {
	SimulationState CheckpointFixture() {
		SimulationDefinition definition;
		definition.Grid.Cells = {4, 4, 4};
		definition.Grid.CellSizeMetres = 8;
		definition.LayerName = "late-join-storm";
		definition.WorldEpoch = 1234;
		SolverCell initial;
		initial.CondensateKilogramsPerCubicMetre = 0.004f;
		auto state = CreateSimulation(definition, initial);
		REQUIRE(state);
		SimulationForcing future;
		future.Tick = 3;
		future.CoolingCelsiusPerSecond = 0.5f;
		future.VapourSourcePerSecond = 0.0001f;
		REQUIRE(AddSimulationForcing(*state, future));
		return *state;
	}
}

TEST_CASE(
	"checkpoint roundtrip preserves current cells forcing and residual budgets", "[weather][checkpoint]"
) {
	auto state = CheckpointFixture();
	for (uint32_t tick = 0; tick < 8; ++tick)
		REQUIRE(StepSimulation(state) == SimulationStatus::Success);
	const auto encoded = EncodeCheckpoint(state);
	REQUIRE(encoded);
	SimulationState lateJoin;
	REQUIRE(DecodeCheckpoint(*encoded, lateJoin));
	CHECK(lateJoin == state);
	REQUIRE(StepSimulation(lateJoin) == SimulationStatus::Success);
	REQUIRE(StepSimulation(state) == SimulationStatus::Success);
	CHECK(lateJoin == state);
}

TEST_CASE(
	"bounded seek uses retained history rather than resetting at target tick", "[weather][checkpoint]"
) {
	auto initial = CheckpointFixture();
	auto continuous = initial;
	for (uint32_t tick = 0; tick < 12; ++tick)
		REQUIRE(StepSimulation(continuous) == SimulationStatus::Success);
	SimulationState replayed;
	REQUIRE(ReplaySimulation(initial, 12, 12, replayed) == SimulationStatus::Success);
	CHECK(replayed == continuous);
	const auto published = replayed;
	CHECK(ReplaySimulation(initial, 13, 12, replayed) == SimulationStatus::BudgetExceeded);
	CHECK(replayed == published);
	CHECK(ReplaySimulation(continuous, 11, 12, replayed) == SimulationStatus::Invalid);
	CHECK(replayed == published);
	const auto checkpoint = EncodeCheckpoint(continuous);
	REQUIRE(checkpoint);
	SimulationState restored;
	REQUIRE(DecodeCheckpoint(*checkpoint, restored));
	REQUIRE(ReplaySimulation(restored, 12, 0, replayed) == SimulationStatus::Success);
	CHECK(replayed == continuous);
}

TEST_CASE("malformed or overbudget checkpoint never partially publishes", "[weather][checkpoint]") {
	const auto initial = CheckpointFixture();
	const auto encoded = EncodeCheckpoint(initial);
	REQUIRE(encoded);
	auto destination = initial;
	REQUIRE(StepSimulation(destination) == SimulationStatus::Success);
	const auto published = destination;
	for (size_t length : {size_t{0}, size_t{7}, encoded->size() / 2, encoded->size() - 1}) {
		CHECK_FALSE(DecodeCheckpoint(std::span<const std::byte>(*encoded).first(length), destination));
		CHECK(destination == published);
	}
	auto corrupted = *encoded;
	corrupted[8] ^= std::byte{1};
	CHECK_FALSE(DecodeCheckpoint(corrupted, destination));
	CHECK(destination == published);
	CHECK_FALSE(DecodeCheckpoint(*encoded, destination, {.MaximumBytes = encoded->size() - 1}));
	CHECK_FALSE(DecodeCheckpoint(*encoded, destination, {.MaximumCells = 1}));
	CHECK_FALSE(DecodeCheckpoint(*encoded, destination, {.MaximumForcingRecords = 1}));
	CHECK(destination == published);
	CHECK_FALSE(EncodeCheckpoint(initial, {.MaximumBytes = 1}));
}

TEST_CASE("forcing edits are ordered bounded and cannot rewrite elapsed history", "[weather][checkpoint]") {
	auto state = CheckpointFixture();
	const auto published = state;
	SimulationForcing duplicate;
	duplicate.Tick = 3;
	CHECK_FALSE(AddSimulationForcing(state, duplicate));
	CHECK(state == published);
	for (uint32_t tick = 0; tick < 8; ++tick)
		REQUIRE(StepSimulation(state) == SimulationStatus::Success);
	SimulationForcing old;
	old.Tick = 7;
	CHECK_FALSE(AddSimulationForcing(state, old));
	SimulationForcing next;
	next.Tick = 8;
	CHECK(AddSimulationForcing(state, next));
	for (uint64_t tick = 9; tick < 80; ++tick) {
		next.Tick = tick;
		if (state.ForcingHistory.size() == 64)
			CHECK_FALSE(AddSimulationForcing(state, next));
		else
			CHECK(AddSimulationForcing(state, next));
	}
	CHECK(state.ForcingHistory.size() == 64);
}

TEST_CASE(
	"forcing compaction retains active state and supports replacement at capacity", "[weather][checkpoint]"
) {
	auto state = CheckpointFixture();
	for (uint32_t tick = 0; tick < 10; ++tick)
		REQUIRE(StepSimulation(state) == SimulationStatus::Success);
	for (uint64_t tick = 11; tick < 75; ++tick) {
		SimulationForcing forcing;
		forcing.Tick = tick;
		if (state.ForcingHistory.size() < 64) REQUIRE(AddSimulationForcing(state, forcing));
	}
	REQUIRE(state.ForcingHistory.size() == 64);
	const auto published = state;
	SimulationForcing overflow;
	overflow.Tick = 100;
	CHECK_FALSE(AddSimulationForcing(state, overflow));
	CHECK(state == published);
	auto replacement = state.ForcingHistory.back();
	replacement.CoolingCelsiusPerSecond = 0.25f;
	CHECK(AddSimulationForcing(state, replacement, true));
	CHECK(state.ForcingHistory.size() == 64);
	CHECK(state.ForcingHistory.back().CoolingCelsiusPerSecond == 0.25f);
}

TEST_CASE(
	"terrain geometry falling water and surface sources survive checkpoint replay", "[weather][checkpoint]"
) {
	SimulationDefinition definition;
	definition.Grid.Cells = {4, 4, 4};
	definition.Grid.CellSizeMetres = 8;
	definition.Grid.Boundaries[0] = SolverBoundary::Open;
	definition.TerrainRevision = 7;
	definition.GroundVapourFluxKilogramsPerSquareMetreSecond = 0.0002f;
	definition.BuoyancyMetresPerSecondSquaredPerCelsius = 0;
	definition.PhaseRelaxationPerSecond = 0;
	definition.PrecipitationPerSecond = 0;
	std::vector<float> heights(16, 8);
	heights[1] = NO_TERRAIN_HEIGHT;
	heights[2] = 16;
	auto state = CreateSimulation(definition, SolverCell{}, {}, heights);
	REQUIRE(state);
	state->FallingCondensateKilogramsPerCubicMetre[8] = 0.01f;
	state->Budget.InitialWaterKilograms = SimulationWaterKilograms(*state);
	for (uint32_t tick = 0; tick < 3; ++tick)
		REQUIRE(StepSimulation(*state) == SimulationStatus::Success);
	const auto encoded = EncodeCheckpoint(*state);
	REQUIRE(encoded);
	SimulationState restored;
	REQUIRE(DecodeCheckpoint(*encoded, restored));
	CHECK(restored == *state);
	CHECK(restored.GroundHeightMetres == heights);
	CHECK(restored.Definition.TerrainRevision == 7);
	SimulationState replayed;
	REQUIRE(ReplaySimulation(restored, 7, 4, replayed) == SimulationStatus::Success);
	for (uint32_t tick = 0; tick < 4; ++tick)
		REQUIRE(StepSimulation(*state) == SimulationStatus::Success);
	CHECK(replayed == *state);
	const auto published = restored;
	auto malformed = restored;
	malformed.SolidMask[0] ^= 1;
	CHECK_FALSE(EncodeCheckpoint(malformed));
	malformed = restored;
	malformed.GroundHeightMetres.pop_back();
	CHECK_FALSE(EncodeCheckpoint(malformed));
	malformed = restored;
	malformed.FallingCondensateKilogramsPerCubicMetre.pop_back();
	CHECK_FALSE(EncodeCheckpoint(malformed));
	for (size_t removed : {size_t{9}, size_t{100}, size_t{256}}) {
		CHECK_FALSE(
			DecodeCheckpoint(std::span<const std::byte>(*encoded).first(encoded->size() - removed), restored)
		);
		CHECK(restored == published);
	}
}
