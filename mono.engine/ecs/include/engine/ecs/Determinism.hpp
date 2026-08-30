#pragma once

// Snapshot fingerprints for detecting replay divergence at exact ticks.
//
// The first observation records a baseline. Later observations of the same
// tick compare the whole serialisable world, including entity generations,
// resources, and clock state.
//
// @tier L3 shared

#include <engine/ecs/Store.hpp>

#include <cstdint>
#include <vector>

namespace engine::ecs {
	enum class DeterminismResult : uint8_t { Disabled, Recorded, Match, Diverged, Unsnapshotable };

	struct DeterminismObservation {
		DeterminismResult Result = DeterminismResult::Disabled;
		uint64_t Tick = 0;
		uint64_t Expected = 0;
		uint64_t Actual = 0;
	};

	class DeterminismTracker {
	  public:
		explicit DeterminismTracker(bool enabled = true);

		DeterminismObservation Observe(const Store &store, uint64_t tick);
		void ForgetAfter(uint64_t tick);
		void Clear();

		bool Enabled() const;
		size_t BaselineCount() const;

	  private:
		struct Baseline {
			uint64_t Tick = 0;
			uint64_t Fingerprint = 0;
		};

		bool TrackingEnabled = true;
		std::vector<Baseline> Baselines;
	};
}
