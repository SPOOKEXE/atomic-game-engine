#include <engine/weather/Checkpoint.hpp>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string_view>

namespace {
	void Require(bool condition) {
		if (!condition) std::abort();
	}
	void RefreshDigest(std::vector<std::byte> &bytes) {
		uint64_t digest = 14695981039346656037ull;
		for (size_t index = 0; index + 8 < bytes.size(); ++index) {
			digest ^= std::to_integer<uint8_t>(bytes[index]);
			digest *= 1099511628211ull;
		}
		for (size_t lane = 0; lane < 8; ++lane)
			bytes[bytes.size() - 8 + lane] = static_cast<std::byte>(digest >> (8 * lane));
	}
	void WriteSeed(
		const std::filesystem::path &directory,
		const char *name,
		const engine::weather::SimulationState &state
	) {
		const auto encoded = engine::weather::EncodeCheckpoint(state);
		Require(encoded.has_value());
		engine::weather::SimulationState restored;
		Require(engine::weather::DecodeCheckpoint(*encoded, restored));
		Require(restored == state);
		std::ofstream file(directory / name, std::ios::binary);
		file.write(
			reinterpret_cast<const char *>(encoded->data()), static_cast<std::streamsize>(encoded->size())
		);
		file.close();
		Require(file.good());
	}
}

extern "C" int LLVMFuzzerInitialize(int *count, char ***arguments) {
	if (*count != 3 || std::string_view((*arguments)[1]) != "--write-seeds") return 0;
	const std::filesystem::path directory((*arguments)[2]);
	std::filesystem::create_directories(directory);
	engine::weather::SimulationDefinition definition;
	definition.Grid.Cells = {2, 2, 2};
	const auto state = engine::weather::CreateSimulation(definition, engine::weather::SolverCell{});
	Require(state.has_value());
	WriteSeed(directory, "weather.checkpoint", *state);

	definition.Grid.Cells = {4, 4, 4};
	definition.Grid.CellSizeMetres = 8;
	definition.LayerName = "terrain-rain";
	definition.TerrainRevision = 7;
	definition.GroundVapourFluxKilogramsPerSquareMetreSecond = 0.0002f;
	definition.MaximumVelocityMetresPerSecond = 4;
	definition.DiffusionSquareMetresPerSecond = 0;
	definition.PhaseRelaxationPerSecond = 0;
	definition.PrecipitationPerSecond = 0;
	definition.BuoyancyMetresPerSecondSquaredPerCelsius = 0;
	std::vector<float> heights(16, 8);
	heights[1] = engine::weather::NO_TERRAIN_HEIGHT;
	heights[2] = 16;
	auto terrain = engine::weather::CreateSimulation(definition, engine::weather::SolverCell{}, {}, heights);
	Require(terrain.has_value());
	terrain->FallingCondensateKilogramsPerCubicMetre[8] = 0.02f;
	terrain->Budget.InitialWaterKilograms = engine::weather::SimulationWaterKilograms(*terrain);
	Require(engine::weather::StepSimulation(*terrain) == engine::weather::SimulationStatus::Success);
	Require(terrain->Budget.SourceWaterKilograms > 0);
	WriteSeed(directory, "weather.terrain-rain.checkpoint", *terrain);

	definition.LayerName = "escaped-top";
	definition.GroundVapourFluxKilogramsPerSquareMetreSecond = 0;
	definition.RainFallSpeedMetresPerSecond = 0;
	definition.Grid.Boundaries = {
		engine::weather::SolverBoundary::Open,
		engine::weather::SolverBoundary::GroundOpen,
		engine::weather::SolverBoundary::Periodic
	};
	engine::weather::SimulationForcing forcing;
	forcing.OpenVelocityMetresPerSecond = {2, 1, 0};
	forcing.Inflow.VapourKilogramsPerKilogram = 0.001f;
	engine::weather::SolverCell initial;
	initial.VapourKilogramsPerKilogram = 0.03f;
	initial.VelocityMetresPerSecond = {2, 1, 0};
	auto escape = engine::weather::CreateSimulation(definition, initial, forcing);
	Require(escape.has_value());
	std::fill(
		escape->FallingCondensateKilogramsPerCubicMetre.begin(),
		escape->FallingCondensateKilogramsPerCubicMetre.end(),
		0.002f
	);
	escape->Budget.InitialWaterKilograms = engine::weather::SimulationWaterKilograms(*escape);
	Require(engine::weather::StepSimulation(*escape) == engine::weather::SimulationStatus::Success);
	Require(escape->Budget.BoundaryWaterKilograms < 0);
	WriteSeed(directory, "weather.escaped-top.checkpoint", *escape);
	std::exit(0);
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *input, size_t size) {
	constexpr engine::weather::CheckpointLimits limits{2 * 1024 * 1024, 4096, 64};
	if (size > limits.MaximumBytes) return 0;
	std::vector<std::byte> bytes(
		std::as_bytes(std::span(input, size)).begin(), std::as_bytes(std::span(input, size)).end()
	);
	// Refreshing the integrity word lets mutations reach the structural decoder rather than only its
	// checksum.
	if (size >= 16) RefreshDigest(bytes);
	engine::weather::SimulationState destination;
	destination.Definition.LayerName = "untouched";
	destination.Tick = 12345;
	const auto published = destination;
	if (!engine::weather::DecodeCheckpoint(bytes, destination, limits)) {
		Require(destination == published);
		return 0;
	}
	Require(engine::weather::ValidateSimulation(destination));
	const auto encoded = engine::weather::EncodeCheckpoint(destination, limits);
	Require(encoded.has_value());
	Require(*encoded == bytes);
	return 0;
}
