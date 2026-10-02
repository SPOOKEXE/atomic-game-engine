#pragma once

#include <engine/imagegraph/Document.hpp>

#include <string>
#include <vector>

namespace engine::imagegraph {
	struct RandomReplayEntry {
		std::string NodeId;
		size_t ProcessorRow = 0;
		uint64_t Tick = 0;
		double Subframe = 0;
		bool Initialized = false;
		uint32_t StoredSeed = 0;
		double Accumulation = 0;
		double MovingAverage = 0;
		double PreviousOutput = 0;
		std::vector<double> Kernel;
		bool operator==(const RandomReplayEntry &) const = default;
	};
	struct RandomReplayState {
		std::vector<RandomReplayEntry> Entries;
		bool operator==(const RandomReplayState &) const = default;
	};
	// Recorded host entropy replaces source current_time and randomize without ambient core IO.
	struct RandomEntropyCapture {
		std::string NodeId;
		size_t ProcessorRow = 0;
		uint64_t Tick = 0;
		double Subframe = 0;
		uint64_t CurrentTimeMilliseconds = 0;
		uint32_t ReshuffleSeed = 0;
		bool operator==(const RandomEntropyCapture &) const = default;
	};
	uint64_t RetainedRandomEntryBytes(const RandomReplayEntry &entry);
	uint64_t RetainedRandomReplayBytes(const RandomReplayState &state);
	[[nodiscard]] Status
	ValidateRandomReplay(const RandomReplayState &state, uint64_t maximumBytes, Diagnostic &diagnostic);
}
