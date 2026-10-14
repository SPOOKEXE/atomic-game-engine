#include "PackingComparison.hpp"

#include <engine/testing/Bench.hpp>
#include <engine/weather/Weather.hpp>

#include <array>
#include <stdexcept>

TEST_SUITE_ID("engine.weather.bench.weather")

namespace {
	uint32_t Checksum(std::span<const engine::weather::EncodedCell> cells) {
		uint32_t checksum = 2166136261u;
		for (const auto &cell : cells)
			for (const uint8_t code : cell.Codes)
				checksum = (checksum ^ code) * 16777619u;
		return checksum;
	}

	struct ReferenceBatch {
		engine::weather::WeatherDefinition Definition;
		engine::weather::WeatherTime Time;
		std::array<engine::weather::CellCoordinate, 4096> Coordinates{};
		std::array<engine::weather::EncodedCell, 4096> Input{};
		std::array<engine::weather::EncodedCell, 4096> Output{};
		std::array<engine::weather::comparison::CellWords, 4096> Words{};
		std::array<uint8_t, 4096 * 8> Planes{};
		std::array<engine::weather::comparison::Fp8Cell, 4096> Fp8{};
	};

	ReferenceBatch &Batch() {
		static ReferenceBatch batch = [] {
			ReferenceBatch created;
			created.Time.PhaseTick = UINT64_MAX;
			for (int32_t index = 0; index < 4096; index++) {
				created.Coordinates[static_cast<size_t>(index)] = {
					index % 64 - 32, (index / 64) % 25, index / 1024
				};
			}
			for (size_t index = 0; index < created.Coordinates.size(); index++) {
				created.Input[index] =
					engine::weather::Evaluate(created.Definition, created.Time, created.Coordinates[index]);
			}
			const uint32_t inputChecksum = Checksum(created.Input);
			const auto validateExact = [&] {
				const uint32_t outputChecksum = Checksum(created.Output);
				engine::testing::Consume(outputChecksum);
				if (outputChecksum != inputChecksum || created.Output != created.Input)
					throw std::runtime_error("weather packing benchmark exact round trip differs");
			};
			if (!engine::weather::comparison::PackWords(created.Input, created.Words))
				throw std::runtime_error("weather word benchmark encode refused");
			engine::testing::Consume(created.Words);
			if (!engine::weather::comparison::UnpackWords(created.Words, created.Output))
				throw std::runtime_error("weather word benchmark decode refused");
			validateExact();
			if (!engine::weather::comparison::PackPlanes(created.Input, created.Planes))
				throw std::runtime_error("weather plane benchmark encode refused");
			engine::testing::Consume(created.Planes);
			if (!engine::weather::comparison::UnpackPlanes(created.Planes, created.Output))
				throw std::runtime_error("weather plane benchmark decode refused");
			validateExact();
			return created;
		}();
		return batch;
	}
}

BENCH_PER_ITEM("Weather scalar canonical samples per cell", 4096) {
	auto &batch = Batch();
	for (size_t index = 0; index < batch.Coordinates.size(); index++) {
		batch.Output[index] =
			engine::weather::Evaluate(batch.Definition, batch.Time, batch.Coordinates[index]);
	}
	engine::testing::Consume(batch.Output);
}

BENCH_PER_ITEM("Weather batch canonical samples per cell", 4096) {
	auto &batch = Batch();
	engine::testing::Consume(
		engine::weather::EvaluateBatch(batch.Definition, batch.Time, batch.Coordinates, batch.Output)
	);
	engine::testing::Consume(batch.Output);
}

BENCH_PER_ITEM("Weather direct seek maximum tick per anchor", 4096) {
	auto anchor = engine::weather::InitialAnchor(Batch().Definition);
	for (uint64_t index = 0; index < 4096; index++) {
		engine::testing::Consume(engine::weather::ResolveTime(anchor, UINT64_MAX - index));
	}
}

BENCH_PER_ITEM("Weather uint32 words encode and decode exact per cell", 4096) {
	auto &batch = Batch();
	engine::testing::Consume(engine::weather::comparison::PackWords(batch.Input, batch.Words));
	// Materialize the representation before decode so the round trip cannot fold into a copy.
	engine::testing::Consume(batch.Words);
	engine::testing::Consume(engine::weather::comparison::UnpackWords(batch.Words, batch.Output));
	engine::testing::Consume(batch.Output);
}

BENCH_PER_ITEM("Weather byte planes encode and decode exact per cell", 4096) {
	auto &batch = Batch();
	engine::testing::Consume(engine::weather::comparison::PackPlanes(batch.Input, batch.Planes));
	engine::testing::Consume(batch.Planes);
	engine::testing::Consume(engine::weather::comparison::UnpackPlanes(batch.Planes, batch.Output));
	engine::testing::Consume(batch.Output);
}

BENCH_PER_ITEM("Weather software E4M3 normalized encode and decode lossy per cell", 4096) {
	auto &batch = Batch();
	engine::testing::Consume(engine::weather::comparison::PackFp8(batch.Input, batch.Fp8));
	engine::testing::Consume(batch.Fp8);
	engine::testing::Consume(engine::weather::comparison::UnpackFp8(batch.Fp8, batch.Output));
	engine::testing::Consume(batch.Output);
}
