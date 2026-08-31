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
	// Outcome of comparing one tick with its recorded baseline.
	enum class DeterminismResult : uint8_t { Disabled, Recorded, Match, Diverged, Unsnapshotable };

	// One exact-tick determinism comparison.
	struct DeterminismObservation {
		// Result, tick, and fingerprints used by the comparison.
		//@{
		DeterminismResult Result = DeterminismResult::Disabled;
		uint64_t Tick = 0;
		uint64_t Expected = 0;
		uint64_t Actual = 0;
		//@}
	};

	// Records world fingerprints and compares repeated observations by tick.
	class DeterminismTracker {
	  public:
		// Creates an enabled or explicitly disabled tracker.
		explicit DeterminismTracker(bool enabled = true);

		// Records or compares the serialisable world at one tick.
		DeterminismObservation Observe(const Store &store, uint64_t tick);

		// Discards baselines newer than a rollback point.
		void ForgetAfter(uint64_t tick);

		// Discards every baseline.
		void Clear();

		// Whether observations record and compare fingerprints.
		bool Enabled() const;

		// Number of exact-tick baselines retained.
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
