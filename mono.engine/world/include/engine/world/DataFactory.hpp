#pragma once

// The bounded lifecycle an external data factory drives through a world.
//
// The service owns only control metadata and immutable encoded checkpoints.
// Universe remains the owner of simulation state. Script and MCP adapters use
// this same service so their pause, step, and restore semantics cannot drift.
//
// @tier L4 · shared

#include <engine/world/Universe.hpp>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace engine::world {

	enum class DataFactoryStatus : uint8_t {
		Ok,
		Unsupported,
		ValidationFailed,
		VersionConflict,
		StaleSnapshot,
		NotPaused,
		RestoreIncomplete,
		ResourceLimit,
	};

	enum class DataFactoryPauseScope : uint8_t { AllSystems, PhysicsOnly };

	// The interval is represented as a rational number of nanoseconds. This is
	// the exported clock contract, separate from the engine's current float
	// simulation delta, so callers never accumulate rounded integer nanoseconds.
	struct DataFactoryInterval {
		uint64_t NumeratorNanoseconds = 1'000'000'000;
		uint32_t Denominator = 60;
	};

	struct DataFactoryClock {
		uint64_t Tick = 0;
		uint64_t TimeNanoseconds = 0;
		bool RationalTimeAvailable = false;
		DataFactoryInterval Interval;
	};

	struct DataFactoryReply {
		DataFactoryStatus Status = DataFactoryStatus::Unsupported;
		std::string Detail;
		std::string InstanceId;
		uint64_t WorldEpoch = 0;
		uint64_t WorldVersion = 0;
		DataFactoryClock Clock;
	};

	struct DataFactoryCheckpoint {
		std::string Id;
		uint64_t Epoch = 0;
		uint64_t Version = 0;
		bool AllSystemsPaused = false;
		bool PhysicsOnlyPaused = false;
		std::vector<std::byte> Bytes;
	};

	// Rebuilds scheduler and product-owned state after `Universe::Load`. A
	// Universe snapshot deliberately carries storage and buses, not scheduler
	// callbacks. Without this hook a checkpoint is not restorable state.
	using DataFactoryRehydrate = std::function<bool(Universe &, WorldId, std::string &)>;

	// Freezes an external subsystem. `paused` is true for pause and false for
	// resume. A host advertises a scope only after every relevant participant is
	// installed through this hook.
	using DataFactoryPauseParticipant =
		std::function<bool(WorldId world, DataFactoryPauseScope scope, bool paused, std::string &)>;

	class DataFactorySession final {
	  public:
		explicit DataFactorySession(
			Universe &universe, size_t checkpointLimit = 16, size_t checkpointBytes = 64 * 1024 * 1024
		);

		void SetRehydrate(DataFactoryRehydrate rehydrate);
		void SetPauseParticipant(DataFactoryPauseParticipant participant);

		DataFactoryReply
		Pause(std::string_view instanceId, DataFactoryPauseScope scope, uint64_t expectedTick);
		DataFactoryReply Resume(std::string_view instanceId, uint64_t expectedTick);
		DataFactoryReply Step(
			std::string_view instanceId,
			DataFactoryInterval interval,
			uint64_t expectedTick,
			uint64_t expectedVersion
		);
		// Returns the lifecycle revision without retaining a snapshot or changing
		// the world, so adapters can reject stale requests before mutation.
		DataFactoryReply Inspect(std::string_view instanceId) const;

		// The host reads this before presentation or input work so it does not
		// keep a second pause map beside the lifecycle session.
		bool AllSystemsPaused(std::string_view instanceId) const;

		DataFactoryReply Snapshot(std::string_view instanceId, std::string &snapshotId);
		DataFactoryReply RenderSnapshotBarrier(std::string_view instanceId, std::string_view snapshotId);
		DataFactoryReply Checkpoint(std::string_view instanceId, std::string &checkpointId);
		DataFactoryReply Restore(std::string_view instanceId, std::string_view checkpointId);

		bool HasCheckpoint(std::string_view checkpointId) const;

	  private:
		struct PauseState {
			bool AllSystems = false;
			bool PhysicsOnly = false;
		};

		WorldId Resolve(std::string_view instanceId) const;
		DataFactoryClock ClockOf(WorldId world) const;
		DataFactoryReply Reply(WorldId world, DataFactoryStatus status, std::string detail) const;
		bool IsCanonicalInterval(WorldId world, DataFactoryInterval interval) const;
		void Store(DataFactoryCheckpoint checkpoint);

		Universe &Worlds;
		size_t Limit = 0;
		size_t MaximumCheckpointBytes = 0;
		uint64_t Epoch = 1;
		uint64_t Version = 0;
		uint64_t NextCheckpoint = 1;
		DataFactoryRehydrate Rehydrate;
		DataFactoryPauseParticipant Participant;
		std::unordered_map<std::string, PauseState> Paused;
		std::unordered_map<std::string, DataFactoryCheckpoint> Checkpoints;
		std::vector<std::string> CheckpointOrder;
		size_t RetainedCheckpointBytes = 0;
	};

	const char *Describe(DataFactoryStatus status);
}
