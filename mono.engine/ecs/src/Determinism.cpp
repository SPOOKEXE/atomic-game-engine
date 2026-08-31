#include <engine/ecs/Determinism.hpp>

#include <algorithm>

namespace engine::ecs {
	namespace {
		uint64_t Fingerprint(std::span<const std::byte> bytes) {
			uint64_t hash = 14695981039346656037ull;
			for (const std::byte value : bytes) {
				hash ^= std::to_integer<uint8_t>(value);
				hash *= 1099511628211ull;
			}
			return hash;
		}
	}

	DeterminismTracker::DeterminismTracker(bool enabled) : TrackingEnabled(enabled) {}

	DeterminismObservation DeterminismTracker::Observe(const Store &store, uint64_t tick) {
		if (!TrackingEnabled) {
			return {.Result = DeterminismResult::Disabled, .Tick = tick};
		}

		core::ByteWriter writer;
		if (!store.Save(writer)) {
			return {.Result = DeterminismResult::Unsnapshotable, .Tick = tick};
		}
		const uint64_t actual = Fingerprint(writer.Bytes());
		const auto found = std::lower_bound(
			Baselines.begin(), Baselines.end(), tick, [](const Baseline &baseline, uint64_t value) {
				return baseline.Tick < value;
			}
		);
		if (found == Baselines.end() || found->Tick != tick) {
			Baselines.insert(found, Baseline{tick, actual});
			return {
				.Result = DeterminismResult::Recorded,
				.Tick = tick,
				.Expected = actual,
				.Actual = actual,
			};
		}
		return {
			.Result = found->Fingerprint == actual ? DeterminismResult::Match : DeterminismResult::Diverged,
			.Tick = tick,
			.Expected = found->Fingerprint,
			.Actual = actual,
		};
	}

	void DeterminismTracker::ForgetAfter(uint64_t tick) {
		const auto firstForgotten = std::upper_bound(
			Baselines.begin(), Baselines.end(), tick, [](uint64_t value, const Baseline &baseline) {
				return value < baseline.Tick;
			}
		);
		Baselines.erase(firstForgotten, Baselines.end());
	}

	void DeterminismTracker::Clear() {
		Baselines.clear();
	}

	bool DeterminismTracker::Enabled() const {
		return TrackingEnabled;
	}

	size_t DeterminismTracker::BaselineCount() const {
		return Baselines.size();
	}
}
