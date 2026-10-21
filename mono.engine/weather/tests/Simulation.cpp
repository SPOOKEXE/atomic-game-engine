#include <engine/testing/Suite.hpp>
#include <engine/weather/Simulation.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <limits>

TEST_SUITE_ID("engine.weather.simulation")
TEST_DEPENDS("engine.weather.weather")
TEST_TIER("fast")

using namespace engine::weather;
using Catch::Approx;

namespace {
	SimulationDefinition PassiveDefinition() {
		SimulationDefinition definition;
		definition.Grid.Cells = {4, 4, 4};
		definition.Grid.CellSizeMetres = 8;
		definition.Grid.Boundaries.fill(SolverBoundary::Periodic);
		definition.DiffusionSquareMetresPerSecond = 0;
		definition.PhaseRelaxationPerSecond = 0;
		definition.PrecipitationPerSecond = 0;
		definition.BuoyancyMetresPerSecondSquaredPerCelsius = 0;
		definition.MaximumVelocityMetresPerSecond = 32;
		return definition;
	}

	void Rebaseline(SimulationState &state) {
		state.Budget.InitialWaterKilograms = SimulationWaterKilograms(state);
		state.Budget.InitialEnthalpyJoules = SimulationEnthalpyJoules(state);
	}
}

TEST_CASE("fixed grid conserves both water species under constant velocity", "[weather][simulation]") {
	auto definition = PassiveDefinition();
	SolverCell initial;
	initial.VelocityMetresPerSecond = {16, 0, 0};
	auto state = CreateSimulation(definition, initial);
	REQUIRE(state);
	for (size_t index = 0; index < state->Cells.size(); ++index) {
		state->Cells[index].VapourKilogramsPerKilogram = index % 4 == 0 ? 0.024f : 0.004f;
		state->Cells[index].CondensateKilogramsPerCubicMetre = index % 4 == 2 ? 0.002f : 0;
		state->Cells[index].TemperatureCelsius = static_cast<float>(index % 4) * 2;
	}
	Rebaseline(*state);
	const double originalWater = SimulationWaterKilograms(*state);
	const double originalHeat = SimulationEnthalpyJoules(*state);
	for (uint32_t tick = 0; tick < 20; ++tick)
		REQUIRE(StepSimulation(*state) == SimulationStatus::Success);
	CHECK(state->Tick == 20);
	CHECK(SimulationWaterKilograms(*state) == Approx(originalWater).epsilon(0.00001));
	CHECK(SimulationEnthalpyJoules(*state) == Approx(originalHeat).epsilon(0.00001));
	CHECK(state->Budget.SourceWaterKilograms == 0);
	CHECK(state->Budget.BoundaryWaterKilograms == 0);
	for (const auto &cell : state->Cells) {
		CHECK(cell.VapourKilogramsPerKilogram >= 0);
		CHECK(cell.CondensateKilogramsPerCubicMetre >= 0);
		CHECK(cell.VelocityMetresPerSecond[0] == Approx(16));
	}
}

TEST_CASE(
	"phase conversion conserves water and moist enthalpy with latent heating", "[weather][simulation]"
) {
	auto definition = PassiveDefinition();
	definition.PhaseRelaxationPerSecond = 20;
	SolverCell initial;
	initial.VapourKilogramsPerKilogram = 0.03f;
	auto state = CreateSimulation(definition, initial);
	REQUIRE(state);
	const double water = SimulationWaterKilograms(*state);
	const double heat = SimulationEnthalpyJoules(*state);
	REQUIRE(StepSimulation(*state) == SimulationStatus::Success);
	CHECK(state->Cells[0].VapourKilogramsPerKilogram < initial.VapourKilogramsPerKilogram);
	CHECK(state->Cells[0].CondensateKilogramsPerCubicMetre > 0);
	CHECK(state->Cells[0].TemperatureCelsius > initial.TemperatureCelsius);
	CHECK(SimulationWaterKilograms(*state) == Approx(water).epsilon(0.000001));
	CHECK(SimulationEnthalpyJoules(*state) == Approx(heat).epsilon(0.000001));
	initial.VapourKilogramsPerKilogram = 0.001f;
	initial.CondensateKilogramsPerCubicMetre = 0.00001f;
	auto dry = CreateSimulation(definition, initial);
	REQUIRE(dry);
	const double dryWater = SimulationWaterKilograms(*dry);
	const double dryHeat = SimulationEnthalpyJoules(*dry);
	REQUIRE(StepSimulation(*dry) == SimulationStatus::Success);
	CHECK(dry->Cells[0].CondensateKilogramsPerCubicMetre < initial.CondensateKilogramsPerCubicMetre);
	CHECK(dry->Cells[0].VapourKilogramsPerKilogram > initial.VapourKilogramsPerKilogram);
	CHECK(dry->Cells[0].TemperatureCelsius < initial.TemperatureCelsius);
	CHECK(SimulationWaterKilograms(*dry) == Approx(dryWater).epsilon(0.000001));
	CHECK(SimulationEnthalpyJoules(*dry) == Approx(dryHeat).epsilon(0.000001));
}

TEST_CASE("external sources cooling and precipitation have explicit budgets", "[weather][simulation]") {
	auto definition = PassiveDefinition();
	definition.PrecipitationPerSecond = 2;
	definition.Grid.Boundaries[1] = SolverBoundary::Closed;
	SimulationForcing forcing;
	forcing.VapourSourcePerSecond = 0.0001f;
	forcing.CondensateSourceKilogramsPerCubicMetrePerSecond = 0.0002f;
	forcing.CoolingCelsiusPerSecond = 0.5f;
	SolverCell initial;
	initial.CondensateKilogramsPerCubicMetre = 0.005f;
	auto state = CreateSimulation(definition, initial, forcing);
	REQUIRE(state);
	for (uint32_t tick = 0; tick < 20; ++tick)
		REQUIRE(StepSimulation(*state) == SimulationStatus::Success);
	CHECK(state->Budget.SourceWaterKilograms > 0);
	CHECK(state->Budget.PrecipitationWaterKilograms > 0);
	CHECK(state->Cells[0].TemperatureCelsius == Approx(14.5f).margin(0.0001));
	CHECK(std::abs(state->Budget.WaterResidualKilograms) < 0.01);
	CHECK(std::abs(state->Budget.EnthalpyResidualJoules) / state->Budget.InitialEnthalpyJoules < 0.00001);
}

TEST_CASE(
	"terrain blocks transport and ground flux enters only real contact faces", "[weather][simulation]"
) {
	auto definition = PassiveDefinition();
	definition.Grid.Boundaries[1] = SolverBoundary::Closed;
	definition.GroundVapourFluxKilogramsPerSquareMetreSecond = 0.001f;
	definition.DiffusionSquareMetresPerSecond = 2;
	std::vector<float> heights(16, 8);
	auto state = CreateSimulation(definition, SolverCell{}, {}, heights);
	REQUIRE(state);
	for (size_t index = 0; index < 64; ++index) {
		const auto y = (index / 4) % 4;
		CHECK(SimulationCellFlags(*state, index) == (y == 0 ? 1 : y == 1 ? 2 : 0));
	}
	const double water = SimulationWaterKilograms(*state);
	const double heat = SimulationEnthalpyJoules(*state);
	REQUIRE(StepSimulation(*state) == SimulationStatus::Success);
	const double source = 16.0 * 8 * 8 * 0.001 / definition.TickRate;
	CHECK(state->Budget.SourceWaterKilograms == Approx(source).epsilon(0.000001));
	CHECK(SimulationWaterKilograms(*state) == Approx(water + source).epsilon(0.000001));
	CHECK(
		state->Budget.ExternalEnthalpyJoules ==
		Approx(source * definition.LatentHeatJoulesPerKilogram).epsilon(0.000001)
	);
	CHECK(
		SimulationEnthalpyJoules(*state) ==
		Approx(heat + state->Budget.ExternalEnthalpyJoules).epsilon(0.000001)
	);
	for (size_t index = 0; index < 64; ++index) {
		if (!(SimulationCellFlags(*state, index) & 1)) continue;
		CHECK(state->Cells[index].VapourKilogramsPerKilogram == 0);
		CHECK(state->Cells[index].CondensateKilogramsPerCubicMetre == 0);
		CHECK(state->Cells[index].VelocityMetresPerSecond == std::array<float, 3>{});
	}
	definition.Grid.Origin[1] = 4;
	auto detached = CreateSimulation(definition, SolverCell{}, {}, heights);
	REQUIRE(detached);
	REQUIRE(StepSimulation(*detached) == SimulationStatus::Success);
	CHECK(detached->Budget.SourceWaterKilograms == 0);
	auto noGround = CreateSimulation(definition, SolverCell{});
	REQUIRE(noGround);
	REQUIRE(StepSimulation(*noGround) == SimulationStatus::Success);
	CHECK(noGround->Budget.SourceWaterKilograms == 0);
}

TEST_CASE("falling water moves downward before becoming a terrain sink", "[weather][simulation]") {
	auto definition = PassiveDefinition();
	definition.Grid.Boundaries[1] = SolverBoundary::Closed;
	definition.RainFallSpeedMetresPerSecond = 8;
	definition.MaximumVelocityMetresPerSecond = 1;
	std::vector<float> heights(16, 8);
	auto state = CreateSimulation(definition, SolverCell{}, {}, heights);
	REQUIRE(state);
	state->FallingCondensateKilogramsPerCubicMetre[8] = 0.01f;
	Rebaseline(*state);
	const double water = SimulationWaterKilograms(*state);
	REQUIRE(StepSimulation(*state) == SimulationStatus::Success);
	REQUIRE(state->LastSubsteps == 1);
	CHECK(state->FallingCondensateKilogramsPerCubicMetre[8] < 0.01f);
	CHECK(state->FallingCondensateKilogramsPerCubicMetre[4] > 0);
	CHECK(state->FallingCondensateKilogramsPerCubicMetre[0] == 0);
	CHECK(state->Budget.PrecipitationWaterKilograms == 0);
	CHECK(SimulationWaterKilograms(*state) == Approx(water).epsilon(0.000001));
	REQUIRE(StepSimulation(*state) == SimulationStatus::Success);
	CHECK(state->Budget.PrecipitationWaterKilograms > 0);
	CHECK(
		SimulationWaterKilograms(*state) + state->Budget.PrecipitationWaterKilograms ==
		Approx(water).epsilon(0.000001)
	);
	CHECK(state->Budget.BoundaryWaterKilograms == 0);
}

TEST_CASE(
	"precipitation conversion is an internal transfer until falling water exits", "[weather][simulation]"
) {
	auto definition = PassiveDefinition();
	definition.PrecipitationPerSecond = 2;
	SolverCell initial;
	initial.CondensateKilogramsPerCubicMetre = 0.005f;
	auto state = CreateSimulation(definition, initial);
	REQUIRE(state);
	const double water = SimulationWaterKilograms(*state);
	const double heat = SimulationEnthalpyJoules(*state);
	REQUIRE(StepSimulation(*state) == SimulationStatus::Success);
	CHECK(state->Cells[0].CondensateKilogramsPerCubicMetre < initial.CondensateKilogramsPerCubicMetre);
	CHECK(state->FallingCondensateKilogramsPerCubicMetre[0] > 0);
	CHECK(state->Budget.PrecipitationWaterKilograms == 0);
	CHECK(SimulationWaterKilograms(*state) == Approx(water).epsilon(0.000001));
	CHECK(SimulationEnthalpyJoules(*state) == Approx(heat).epsilon(0.000001));
}

TEST_CASE(
	"mixed open terrain boundaries conserve escaped rain and pressure transport", "[weather][simulation]"
) {
	auto definition = PassiveDefinition();
	definition.Grid.Boundaries = {SolverBoundary::Open, SolverBoundary::Closed, SolverBoundary::Periodic};
	definition.RainFallSpeedMetresPerSecond = 0;
	SimulationForcing forcing;
	forcing.OpenVelocityMetresPerSecond[0] = 2;
	SolverCell initial;
	initial.VelocityMetresPerSecond[0] = 2;
	std::vector<float> heights(16, 8);
	auto cold = CreateSimulation(definition, initial, forcing, heights);
	REQUIRE(cold);
	std::fill(
		cold->FallingCondensateKilogramsPerCubicMetre.begin(),
		cold->FallingCondensateKilogramsPerCubicMetre.end(),
		0.01f
	);
	for (size_t index = 0; index < cold->Cells.size(); ++index)
		if (SimulationCellFlags(*cold, index) & 1) cold->FallingCondensateKilogramsPerCubicMetre[index] = 0;
	Rebaseline(*cold);
	const double water = SimulationWaterKilograms(*cold);
	auto warm = *cold;
	SimulationWorkspace workspace;
	for (uint32_t tick = 0; tick < 4; ++tick) {
		REQUIRE(StepSimulation(*cold) == SimulationStatus::Success);
		REQUIRE(StepSimulation(warm, workspace) == SimulationStatus::Success);
		CHECK(warm == *cold);
	}
	CHECK(cold->Budget.BoundaryWaterKilograms < 0);
	CHECK(cold->Budget.PrecipitationWaterKilograms == 0);
	CHECK(
		SimulationWaterKilograms(*cold) - cold->Budget.BoundaryWaterKilograms ==
		Approx(water).epsilon(0.00001)
	);
	CHECK(cold->LastMaximumDivergencePerSecond <= definition.MaximumDivergencePerSecond);
}

TEST_CASE(
	"terrain and falling-water validation refuses malformed state transactionally", "[weather][simulation]"
) {
	auto definition = PassiveDefinition();
	CHECK_FALSE(CreateSimulation(definition, SolverCell{}, {}, std::vector<float>(15, 0)));
	std::vector<float> invalidHeight(16, std::numeric_limits<float>::quiet_NaN());
	CHECK_FALSE(CreateSimulation(definition, SolverCell{}, {}, invalidHeight));
	auto state = CreateSimulation(definition, SolverCell{});
	REQUIRE(state);
	state->SolidMask[0] = 1;
	const auto malformed = *state;
	CHECK(StepSimulation(*state) == SimulationStatus::Invalid);
	CHECK(*state == malformed);
	state->SolidMask[0] = 0;
	state->FallingCondensateKilogramsPerCubicMetre[0] = -1;
	const auto negativeRain = *state;
	CHECK(StepSimulation(*state) == SimulationStatus::Invalid);
	CHECK(*state == negativeRain);
	definition.RainFallSpeedMetresPerSecond = 64;
	definition.MaximumSubsteps = 1;
	definition.Grid.CellSizeMetres = 1;
	definition.TickRate = 1;
	auto unstable = CreateSimulation(definition, SolverCell{});
	REQUIRE(unstable);
	const auto previous = *unstable;
	CHECK(StepSimulation(*unstable) == SimulationStatus::Unstable);
	CHECK(*unstable == previous);
}

TEST_CASE("open and ground top boundaries account for moisture transfer", "[weather][simulation]") {
	for (SolverBoundary boundary : {SolverBoundary::Open, SolverBoundary::GroundOpen}) {
		auto definition = PassiveDefinition();
		definition.Grid.Boundaries[1] = boundary;
		definition.DiffusionSquareMetresPerSecond = 2;
		SimulationForcing forcing;
		forcing.Inflow.VapourKilogramsPerKilogram = 0.05f;
		if (boundary == SolverBoundary::Open) forcing.OpenVelocityMetresPerSecond[1] = 1;
		auto state = CreateSimulation(definition, SolverCell{}, forcing);
		REQUIRE(state);
		for (uint32_t tick = 0; tick < 10; ++tick)
			REQUIRE(StepSimulation(*state) == SimulationStatus::Success);
		CHECK(state->Budget.BoundaryWaterKilograms > 0);
		CHECK(std::abs(state->Budget.WaterResidualKilograms) < 0.01);
		CHECK(state->Cells.back().VapourKilogramsPerKilogram > 0.012f);
	}
}

TEST_CASE(
	"escape top transports projected air moisture and falling water while ground stays closed",
	"[weather][simulation]"
) {
	auto definition = PassiveDefinition();
	definition.Grid.Boundaries = {SolverBoundary::Open, SolverBoundary::GroundOpen, SolverBoundary::Periodic};
	definition.RainFallSpeedMetresPerSecond = 0;
	SimulationForcing forcing;
	forcing.OpenVelocityMetresPerSecond = {2, 1, 0};
	forcing.Inflow.VapourKilogramsPerKilogram = 0.001f;
	SolverCell initial;
	initial.VapourKilogramsPerKilogram = 0.03f;
	initial.VelocityMetresPerSecond = {2, 1, 0};
	auto escaping = CreateSimulation(definition, initial, forcing);
	REQUIRE(escaping);
	std::fill(
		escaping->FallingCondensateKilogramsPerCubicMetre.begin(),
		escaping->FallingCondensateKilogramsPerCubicMetre.end(),
		0.002f
	);
	Rebaseline(*escaping);
	const double water = SimulationWaterKilograms(*escaping);
	definition.Grid.Boundaries[1] = SolverBoundary::Closed;
	auto closed = CreateSimulation(definition, initial, forcing);
	REQUIRE(closed);
	REQUIRE(StepSimulation(*escaping) == SimulationStatus::Success);
	REQUIRE(StepSimulation(*closed) == SimulationStatus::Success);
	float topOutflow = 0;
	for (uint32_t z = 0; z < 4; ++z)
		for (uint32_t x = 0; x < 4; ++x) {
			const size_t top = x + 4 * (3 + 4 * z);
			topOutflow += std::max(0.0f, escaping->Cells[top].VelocityMetresPerSecond[1]);
			CHECK(closed->Cells[top].VelocityMetresPerSecond[1] == 0);
		}
	CHECK(topOutflow > 0);
	CHECK(escaping->Budget.BoundaryWaterKilograms < 0);
	CHECK(escaping->Budget.PrecipitationWaterKilograms == 0);
	CHECK(
		SimulationWaterKilograms(*escaping) - escaping->Budget.BoundaryWaterKilograms ==
		Approx(water).epsilon(0.00001)
	);
	CHECK(escaping->LastMaximumDivergencePerSecond <= escaping->Definition.MaximumDivergencePerSecond);
}

TEST_CASE("projection pressure remains distinct from atmospheric pressure", "[weather][simulation]") {
	auto definition = PassiveDefinition();
	definition.BuoyancyMetresPerSecondSquaredPerCelsius = 0.034f;
	definition.Grid.Boundaries[1] = SolverBoundary::Closed;
	auto state = CreateSimulation(definition, SolverCell{});
	REQUIRE(state);
	state->Cells[0].TemperatureCelsius = 25;
	state->Cells[0].AtmosphericPressurePascals += 50;
	Rebaseline(*state);
	REQUIRE(StepSimulation(*state) == SimulationStatus::Success);
	CHECK(state->Cells[0].AtmosphericPressurePascals == 101375);
	CHECK(state->Cells[0].ProjectionPressurePascals == 0);
	CHECK(state->Cells[1].ProjectionPressurePascals != 0);
	CHECK(state->LastMaximumDivergencePerSecond <= definition.MaximumDivergencePerSecond);
}

TEST_CASE("solver refusals never publish a partial tick", "[weather][simulation]") {
	auto definition = PassiveDefinition();
	CHECK_FALSE(
		CreateSimulation(SimulationDefinition{.Grid = SimulationGrid{.Cells = {128, 128, 128}}}, SolverCell{})
	);
	definition.MaximumStateBytes = 1;
	CHECK_FALSE(CreateSimulation(definition, SolverCell{}));
	definition = PassiveDefinition();
	auto state = CreateSimulation(definition, SolverCell{});
	REQUIRE(state);
	state->Tick = std::numeric_limits<uint64_t>::max();
	const auto overflow = *state;
	CHECK(StepSimulation(*state) == SimulationStatus::Invalid);
	CHECK(*state == overflow);
	state->Tick = 0;
	state->Definition.TickRate = 1;
	state->Definition.Grid.CellSizeMetres = 0.25f;
	state->Definition.MaximumSubsteps = 1;
	state->Definition.MaximumVelocityMetresPerSecond = 256;
	state->Cells[0].VelocityMetresPerSecond[0] = 256;
	const auto unstable = *state;
	CHECK(StepSimulation(*state) == SimulationStatus::Unstable);
	CHECK(*state == unstable);
	state->Cells[0].TemperatureCelsius = std::numeric_limits<float>::quiet_NaN();
	CHECK(StepSimulation(*state) == SimulationStatus::Invalid);
}

TEST_CASE("presentation packing retains authoritative physical state", "[weather][simulation]") {
	auto state = CreateSimulation(PassiveDefinition(), SolverCell{});
	REQUIRE(state);
	const auto physical = *state;
	const auto packed = PackSimulation(*state, EncodingProfile{});
	REQUIRE(packed);
	CHECK(packed->size() == state->Cells.size());
	CHECK(*state == physical);
	EncodingProfile invalidProfile;
	invalidProfile.VapourKilogramsPerKilogram = 0;
	CHECK_FALSE(PackSimulation(*state, invalidProfile));
}

TEST_CASE("projection refusal preserves the previous complete state", "[weather][simulation]") {
	auto definition = PassiveDefinition();
	definition.ProjectionIterations = 1;
	definition.MaximumDivergencePerSecond = 0.000001f;
	auto state = CreateSimulation(definition, SolverCell{});
	REQUIRE(state);
	state->Cells[0].VelocityMetresPerSecond[0] = 2;
	const auto published = *state;
	CHECK(StepSimulation(*state) == SimulationStatus::ProjectionFailed);
	CHECK(*state == published);
}

TEST_CASE(
	"bounded diffusion mixes neighboring water without creating or deleting it", "[weather][simulation]"
) {
	auto definition = PassiveDefinition();
	definition.DiffusionSquareMetresPerSecond = 10;
	auto state = CreateSimulation(definition, SolverCell{});
	REQUIRE(state);
	state->Cells[0].VapourKilogramsPerKilogram = 0.05f;
	state->Cells[0].CondensateKilogramsPerCubicMetre = 0.005f;
	Rebaseline(*state);
	const double water = SimulationWaterKilograms(*state);
	for (uint32_t tick = 0; tick < 10; ++tick)
		REQUIRE(StepSimulation(*state) == SimulationStatus::Success);
	CHECK(state->Cells[0].VapourKilogramsPerKilogram < 0.05f);
	CHECK(state->Cells[1].VapourKilogramsPerKilogram > 0.012f);
	CHECK(state->Cells[1].CondensateKilogramsPerCubicMetre > 0);
	CHECK(SimulationWaterKilograms(*state) == Approx(water).epsilon(0.00001));
}

TEST_CASE("reused solver workspace preserves exact reference steps and refusal", "[weather][simulation]") {
	auto definition = PassiveDefinition();
	definition.Grid.Boundaries[1] = SolverBoundary::Closed;
	definition.DiffusionSquareMetresPerSecond = 2;
	auto cold = CreateSimulation(definition, SolverCell{});
	REQUIRE(cold);
	cold->Cells[0].VapourKilogramsPerKilogram = 0.04f;
	cold->Cells[0].AtmosphericPressurePascals += 50;
	Rebaseline(*cold);
	auto warm = *cold;
	SimulationWorkspace workspace;
	for (uint32_t tick = 0; tick < 12; ++tick) {
		REQUIRE(StepSimulation(*cold) == SimulationStatus::Success);
		REQUIRE(StepSimulation(warm, workspace) == SimulationStatus::Success);
		CHECK(warm == *cold);
	}
	warm.Definition.ProjectionIterations = 1;
	warm.Definition.MaximumDivergencePerSecond = 0.000001f;
	warm.Cells[0].VelocityMetresPerSecond[0] += 2;
	const auto published = warm;
	CHECK(StepSimulation(warm, workspace) == SimulationStatus::ProjectionFailed);
	CHECK(warm == published);
}
