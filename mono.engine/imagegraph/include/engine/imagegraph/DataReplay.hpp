#pragma once

#include <engine/imagegraph/Document.hpp>

#include <string>
#include <vector>

namespace engine::imagegraph {
	// Each processor row owns its source differential or Boolean trigger history.
	struct DataReplayEntry {
		std::string NodeId;
		size_t ProcessorRow = 0;
		uint64_t Tick = 0;
		double Subframe = 0;
		bool NegativeFrame = false;
		bool Initialized = false;
		double PreviousValue = 0;
		double PreviousFrame = 0;
		bool Trigger = false;
		bool operator==(const DataReplayEntry &) const = default;
	};
	struct DataReplayState {
		std::vector<DataReplayEntry> Entries;
		bool operator==(const DataReplayState &) const = default;
	};
	uint64_t RetainedDataReplayEntryBytes(const DataReplayEntry &entry);
	uint64_t RetainedDataReplayBytes(const DataReplayState &state);
	[[nodiscard]] Status
	ValidateDataReplay(const DataReplayState &state, uint64_t maximumBytes, Diagnostic &diagnostic);
}
