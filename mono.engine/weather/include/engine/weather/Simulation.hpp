#pragma once

// Fixed canonical constant-density moist-air solver. Full precision state survives cosmetic packing.
// Positive-face velocities form a MAC-style grid; projection pressure is separate from atmospheric pressure.
// @tier L8 shared

#include <engine/weather/Weather.hpp>

#include <array>
#include <optional>
#include <string>
#include <vector>

namespace engine::weather {

	// Each axis uses paired periodic, impermeable or prescribed open boundaries.
	enum class SolverBoundary : uint8_t {
		// Identical lattice values meet across opposite faces.
		Periodic,
		// Both exterior faces are impermeable.
		Closed,
		// Both exterior faces use prescribed balanced velocity and copied inflow scalars.
		Open,
		// The vertical ground is closed; the top exchanges scalar diffusion with zero net dry-air flow.
		GroundOpen
	};
	// The first solver version keeps a bounded uniform grid independent of all viewers.
	struct SimulationGrid {
		// Fixed lattice geometry in cells and metres, with per-axis boundary policy.
		//@{
		std::array<uint32_t, 3> Cells{8, 8, 8};
		std::array<int32_t, 3> Origin{};
		float CellSizeMetres = 64.0f;
		std::array<SolverBoundary, 3> Boundaries{
			SolverBoundary::Periodic, SolverBoundary::Closed, SolverBoundary::Periodic
		};
		// Compares every semantic field for checkpoint and replay verification.
		bool operator==(const SimulationGrid &) const = default;
		//@}
	};
	// Eight FP32 values per canonical cell. Velocities belong to the cell's positive axis faces.
	struct SolverCell {
		// Physical water and thermal values; face velocity and pressure retain full precision.
		//@{
		float VapourKilogramsPerKilogram = 0.012f;
		float CondensateKilogramsPerCubicMetre = 0.0f;
		float TemperatureCelsius = 15.0f;
		float AtmosphericPressurePascals = 101325.0f;
		std::array<float, 3> VelocityMetresPerSecond{};
		float ProjectionPressurePascals = 0.0f;
		// Compares every semantic field for checkpoint and replay verification.
		bool operator==(const SolverCell &) const = default;
		//@}
	};
	static_assert(sizeof(SolverCell) == 32);
	// Tick-stamped external sources and boundary conditions. Records are applied before their tick.
	struct SimulationForcing {
		// Accepted tick, physical forcing rates and exterior reference state.
		//@{
		uint64_t Tick = 0;
		std::array<float, 3> AccelerationMetresPerSecondSquared{};
		std::array<float, 3> OpenVelocityMetresPerSecond{};
		std::array<float, 3> WindTargetMetresPerSecond{};
		float WindRelaxationPerSecond = 0;
		float VapourSourcePerSecond = 0.0f;
		float CondensateSourceKilogramsPerCubicMetrePerSecond = 0.0f;
		float CoolingCelsiusPerSecond = 0.0f;
		SolverCell Inflow;
		// Compares every semantic field for checkpoint and replay verification.
		bool operator==(const SimulationForcing &) const = default;
		//@}
	};
	// Declared numerical model and resource ceilings, including transactional scratch storage.
	struct SimulationDefinition {
		// Versioned identity, canonical geometry, resource ceilings and SI model parameters.
		//@{
		std::string AlgorithmName = "constant-density-moist-air";
		std::string LayerName = "weather";
		uint32_t AlgorithmVersion = 1;
		uint32_t Revision = 1;
		uint32_t Seed = 1;
		// Exact initialization recipe words, retained so checkpoint compatibility includes authored physics.
		std::array<uint32_t, 16> InitializationRecipe{};
		uint64_t WorldEpoch = 1;
		SimulationGrid Grid;
		uint32_t TickRate = 20;
		uint32_t MaximumSubsteps = 128;
		uint32_t ProjectionIterations = 128;
		uint64_t MaximumStateBytes = 32 * 1024 * 1024;
		float DryAirKilogramsPerCubicMetre = 1.225f;
		float DiffusionSquareMetresPerSecond = 0.5f;
		float ReferenceTemperatureCelsius = 15.0f;
		float SaturationVapourRatio = 0.012f;
		float SaturationSlopePerCelsius = 0.06f;
		float PhaseRelaxationPerSecond = 1.0f;
		float LatentHeatJoulesPerKilogram = 2500000.0f;
		float HeatCapacityJoulesPerKilogramCelsius = 1005.0f;
		float BuoyancyMetresPerSecondSquaredPerCelsius = 0.034f;
		float PrecipitationThresholdKilogramsPerCubicMetre = 0.001f;
		float PrecipitationPerSecond = 0.1f;
		float MaximumVelocityMetresPerSecond = 256.0f;
		float MaximumPressureGradientPascalsPerMetre = 64.0f;
		float MaximumDivergencePerSecond = 0.002f;
		// Compares every semantic field for checkpoint and replay verification.
		bool operator==(const SimulationDefinition &) const = default;
		//@}
	};
	// Cumulative physical budgets in SI units. Residuals measure error rather than hiding it with a clamp.
	struct SimulationBudget {
		// Initial totals, external changes and measured numerical residuals in SI units.
		//@{
		double InitialWaterKilograms = 0;
		double SourceWaterKilograms = 0;
		double BoundaryWaterKilograms = 0;
		double PrecipitationWaterKilograms = 0;
		double WaterResidualKilograms = 0;
		double InitialEnthalpyJoules = 0;
		double ExternalEnthalpyJoules = 0;
		double EnthalpyResidualJoules = 0;
		// Compares every semantic field for checkpoint and replay verification.
		bool operator==(const SimulationBudget &) const = default;
		//@}
	};
	// Complete authoritative checkpoint state. Only this state, not encoded cells, is replayable.
	struct SimulationState {
		// Complete current grid, retained forcing history and completed-tick diagnostics.
		//@{
		SimulationDefinition Definition;
		uint64_t Tick = 0;
		std::vector<SolverCell> Cells;
		std::vector<SimulationForcing> ForcingHistory;
		SimulationBudget Budget;
		uint32_t LastSubsteps = 0;
		float LastMaximumDivergencePerSecond = 0;
		// Compares every semantic field for checkpoint and replay verification.
		bool operator==(const SimulationState &) const = default;
		//@}
	};
	// Reusable transactional scratch. Published physical authority remains in SimulationState.
	struct SimulationWorkspace {
		// Derived adjacency and reusable numerical buffers, reset rather than serialized.
		//@{
		SimulationGrid Grid;
		std::vector<std::array<uint32_t, 6>> Neighbors;
		std::vector<SolverCell> ForcedCells;
		std::vector<SolverCell> NextCells;
		std::vector<float> Pressure;
		std::vector<float> NextPressure;
		std::vector<float> Divergence;
		SimulationState Trial;
		//@}
	};
	// Detailed refusal leaves the input state unchanged, including tick and budgets.
	enum class SimulationStatus : uint8_t {
		// The entire tick or replay was published.
		Success,
		// Input geometry, state, history or tick is incompatible.
		Invalid,
		// An allocation or bounded replay limit refused the operation.
		BudgetExceeded,
		// The stability schedule or evolved physical limits were exceeded.
		Unstable,
		// The bounded pressure solve did not meet its divergence tolerance.
		ProjectionFailed
	};
	// Validates geometry, finite physics and all replay metadata without allocating.
	[[nodiscard]] bool ValidateSimulation(const SimulationState &state);
	// Builds a validated fixed-domain state with uniform initial conditions.
	[[nodiscard]] std::optional<SimulationState> CreateSimulation(
		const SimulationDefinition &definition,
		const SolverCell &initial,
		const SimulationForcing &forcing = {}
	);
	// Converts authored stateful controls into the declared canonical solver, never a camera LOD.
	[[nodiscard]] std::optional<SimulationDefinition>
	MakeSimulationDefinition(const scene::WeatherCompute &controls, std::string layerName = "weather");
	// Adds bounded, ordered forcing history. Changes before the current tick are refused.
	[[nodiscard]] bool AddSimulationForcing(
		SimulationState &state, const SimulationForcing &forcing, bool replacePending = false
	);
	// Canonical stability schedule depends only on declared physical bounds and accepted forcing.
	[[nodiscard]] std::optional<uint32_t>
	SimulationSubsteps(const SimulationDefinition &definition, const SimulationForcing &forcing);
	// Evolves one fixed weather tick with conservative finite-volume transport and pressure projection.
	[[nodiscard]] SimulationStatus StepSimulation(SimulationState &state);
	// Reuses bounded topology and buffers on an authoritative host without publishing failed trial steps.
	[[nodiscard]] SimulationStatus StepSimulation(SimulationState &state, SimulationWorkspace &workspace);
	// Restores and replays at most maximumTicks. Failure never partially publishes the destination.
	[[nodiscard]] SimulationStatus ReplaySimulation(
		const SimulationState &checkpoint,
		uint64_t targetTick,
		uint32_t maximumTicks,
		SimulationState &destination
	);
	// Total physical water and moist enthalpy use the model's declared constant dry-air density.
	[[nodiscard]] double SimulationWaterKilograms(const SimulationState &state);
	// Includes latent energy stored in vapour, allowing phase conversion to conserve moist enthalpy.
	[[nodiscard]] double SimulationEnthalpyJoules(const SimulationState &state);
	// Quantizes a derived presentation copy; saturation here cannot modify authoritative physics.
	[[nodiscard]] std::optional<std::vector<EncodedCell>>
	PackSimulation(const SimulationState &state, const EncodingProfile &profile);
}
