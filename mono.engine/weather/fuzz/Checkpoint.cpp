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
}

extern "C" int LLVMFuzzerInitialize(int *count, char ***arguments) {
	if (*count != 3 || std::string_view((*arguments)[1]) != "--write-seeds") return 0;
	const std::filesystem::path directory((*arguments)[2]);
	std::filesystem::create_directories(directory);
	engine::weather::SimulationDefinition definition;
	definition.Grid.Cells = {2, 2, 2};
	const auto state = engine::weather::CreateSimulation(definition, engine::weather::SolverCell{});
	Require(state.has_value());
	const auto encoded = engine::weather::EncodeCheckpoint(*state);
	Require(encoded.has_value());
	std::ofstream file(directory / "weather.checkpoint", std::ios::binary);
	file.write(
		reinterpret_cast<const char *>(encoded->data()), static_cast<std::streamsize>(encoded->size())
	);
	Require(file.good());
	file.close();
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
