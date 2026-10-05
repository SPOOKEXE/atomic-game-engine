#pragma once

#include <engine/imagegraph/CacheGroupReplay.hpp>

#include <string>
#include <vector>

namespace engine::imagegraph {
	// A source value cache owns a cloned value at an integer timeline frame.
	struct DataReplayValueFrame {
		uint64_t Frame = 0;
		Value Data = double{0};
		bool operator==(const DataReplayValueFrame &) const = default;
	};
	// Each processor row owns its source scalar, trigger, or delayed value history.
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
		std::vector<DataReplayValueFrame> Values;
		// Exact authored cache text binds loaded source frame history to its saved payload.
		std::string LoadedCacheData;
		// A selected frame-cache Clear suppresses constructor loading for every processor row.
		bool FrameCacheConstructorCleared = false;
		bool operator==(const DataReplayEntry &) const = default;
	};
	struct DataReplayState {
		std::vector<DataReplayEntry> Entries;
		CacheGroupReplayState CacheGroups{};
		bool operator==(const DataReplayState &) const = default;
	};
	uint64_t RetainedDataReplayEntryBytes(const DataReplayEntry &entry);
	uint64_t RetainedDataReplayBytes(const DataReplayState &state);
	// Replay rows and group outputs validate in sequence; only the largest sorting table is live.
	uint64_t DataReplayValidationWorkspaceBytes(const DataReplayState &state, size_t additionalRows = 0);
	[[nodiscard]] Status
	ValidateDataReplay(const DataReplayState &state, uint64_t maximumBytes, Diagnostic &diagnostic);
}
