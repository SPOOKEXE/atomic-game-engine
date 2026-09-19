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
#include <deque>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace engine::world {

	inline constexpr size_t MAXIMUM_DATA_FACTORY_ACTIONS = 32;

	enum class DataFactoryStatus : uint8_t {
		Ok,
		Unsupported,
		ValidationFailed,
		OperationIdConflict,
		VersionConflict,
		StaleSnapshot,
		NotPaused,
		RestoreIncomplete,
		ResourceLimit,
		PresentationFailed,
		Pending,
	};

	enum class DataFactoryPauseScope : uint8_t { AllSystems, PhysicsOnly };

	// This belongs to the data-factory lifecycle rather than the renderer. The
	// host converts it to its renderer-specific policy at the client boundary.
	// Preserve is the only policy currently offered because Reset and Disable
	// require an actual renderer-local history pass.
	enum class DataFactoryTemporalHistory : uint8_t { Preserve, Reset, Disable };

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
		bool Tombstone = false;
	};

	enum class DataFactoryWorldOperation : uint8_t { Create, Reset, Retire };

	struct DataFactoryWorldRequest {
		DataFactoryWorldOperation Operation = DataFactoryWorldOperation::Create;
		std::string InstanceId;
		// Episode metadata is retained for a later package or manifest. WorldSettings
		// intentionally has no seed because it does not own product random sources.
		uint64_t Seed = 0;
		double TickRate = 60.0;
		uint64_t ExpectedWorldEpoch = 0;
		uint64_t ExpectedWorldVersion = 0;
		uint64_t ExpectedTick = 0;
		std::string OperationId;
	};

	// A value-only request crosses from the shared world layer to the host that
	// owns presentation. No store, renderer, GPU handle, or callback-owned state
	// leaks through this boundary, so a headless host can refuse it honestly.
	struct DataFactoryRenderOnlyRequest {
		std::string InstanceId;
		std::string SnapshotId;
		uint64_t ExpectedWorldEpoch = 0;
		uint64_t ExpectedWorldVersion = 0;
		uint64_t ExpectedTick = 0;
		uint64_t OperationId = 0;
		DataFactoryTemporalHistory TemporalHistory = DataFactoryTemporalHistory::Preserve;
	};

	struct DataFactoryRenderOnlyReply : DataFactoryReply {
		uint64_t OperationId = 0;
		DataFactoryTemporalHistory TemporalHistory = DataFactoryTemporalHistory::Preserve;
		bool Presented = false;
	};

	struct DataFactoryRenderOnlyCompletion {
		std::string InstanceId;
		uint64_t OperationId = 0;
		bool Submitted = false;
		std::string Detail;
	};

	struct DataFactoryCheckpoint {
		std::string Id;
		uint64_t Epoch = 0;
		uint64_t Version = 0;
		uint64_t Tick = 0;
		uint64_t ReplayGeneration = 0;
		bool AllSystemsPaused = false;
		bool PhysicsOnlyPaused = false;
		std::vector<std::byte> Bytes;
	};

	// Rebuilds scheduler and product-owned state after `Universe::Load`. A
	// Universe snapshot deliberately carries storage and buses, not scheduler
	// callbacks. Without this hook a checkpoint is not restorable state.
	using DataFactoryRehydrate = std::function<bool(Universe &, WorldId, std::string &)>;

	// Publishes or discards host state prepared alongside a scratch universe.
	// A runtime binds to the scratch store, so it cannot become visible until
	// `Universe::ReplaceWith` commits. Abort runs for every failed candidate.
	using DataFactoryRehydrateCommit = std::function<void()>;
	using DataFactoryRehydrateAbort = std::function<void()>;
	struct DataFactoryRehydrator {
		DataFactoryRehydrate Prepare;
		DataFactoryRehydrateCommit Commit;
		DataFactoryRehydrateAbort Abort;
	};

	// Rebuilds and publishes host state for an isolated fork. Fork preparation
	// cannot reuse the live-world rehydrator: its commit owns runtime state for
	// the parent universe. The branch id is the host's stable ownership key.
	using DataFactoryForkPrepare = std::function<bool(std::string_view, Universe &, WorldId, std::string &)>;
	using DataFactoryForkCommit = std::function<void(std::string_view)>;
	using DataFactoryForkAbort = std::function<void(std::string_view)>;
	using DataFactoryForkRetire = std::function<void(std::string_view)>;
	struct DataFactoryForkRehydrator {
		DataFactoryForkPrepare Prepare;
		DataFactoryForkCommit Commit;
		DataFactoryForkAbort Abort;
		DataFactoryForkRetire Retire;
	};

	// Freezes an external subsystem. `paused` is true for pause and false for
	// resume. A host advertises a scope only after every relevant participant is
	// installed through this hook.
	using DataFactoryPauseParticipant =
		std::function<bool(WorldId world, DataFactoryPauseScope scope, bool paused, std::string &)>;

	// Product-owned state is prepared against the candidate before replacement.
	// A false pre-commit result refuses the operation with the live universe
	// intact. The committed call is notification only and must not fail.
	using DataFactoryWorldLifecycle = std::function<bool(
		DataFactoryWorldOperation operation,
		Universe &universe,
		WorldId world,
		bool committed,
		std::string &detail
	)>;

	// Presentation is owned by the product. The callback starts exactly one
	// render-only frame for a request already proven to name the live paused
	// snapshot. Deferred image readback remains owned by the renderer ticket.
	using DataFactoryRenderOnlyPresenter =
		std::function<bool(const DataFactoryRenderOnlyRequest &, std::string &)>;

	// Copied control values cross into the host executor. The world service does
	// not interpret a scene property path or retain a JSON representation.
	struct DataFactoryInterventionValue {
		enum class Kind : uint8_t { Missing, Boolean, Integer, Number, String };

		Kind Type = Kind::Missing;
		bool Boolean = false;
		int64_t Integer = 0;
		double Number = 0.0;
		std::string String;
	};

	struct DataFactoryIntervention {
		std::string TargetId;
		std::string Path;
		DataFactoryInterventionValue Expected;
		DataFactoryInterventionValue Value;
	};

	// The product owns scene-property vocabulary and must apply every row or no
	// rows. The session restores its encoded rollback point if this refuses.
	using DataFactoryInterventionExecutor =
		std::function<bool(Universe &, WorldId, std::span<const DataFactoryIntervention>, std::string &)>;

	// A named script-authored settings-menu action to deliver at one fixed-tick
	// boundary. The host validates its vocabulary before queuing any action.
	struct DataFactoryAction {
		std::string Name;
	};

	// Forks one retained parent checkpoint into a separately owned universe.
	// BranchId names that isolated runtime. The encoded world keeps its source
	// name because Universe deliberately has no in-place rename operation.
	struct DataFactoryForkRequest {
		std::string InstanceId;
		std::string CheckpointId;
		std::string BranchId;
		uint64_t ExpectedWorldEpoch = 0;
		uint64_t ExpectedWorldVersion = 0;
		uint64_t ExpectedTick = 0;
	};

	// An infallible host commit that runs at the exact paused tick boundary.
	using DataFactoryActionCommit = std::function<void()>;

	// The product owns which named actions its current script runtime exposes.
	// It must either refuse the complete batch or return one infallible deferred
	// commit after validating every action.
	using DataFactoryActionExecutor = std::function<bool(
		Universe &, WorldId, std::span<const DataFactoryAction>, DataFactoryActionCommit &, std::string &
	)>;

	// Replays an accepted action batch against a scratch universe. It must not
	// inspect or mutate the live host runtime, because a later candidate check
	// may refuse the seek and discard this universe.
	using DataFactoryReplayActionExecutor = std::function<bool(
		Universe &, WorldId, std::span<const DataFactoryAction>, DataFactoryActionCommit &, std::string &
	)>;

	class DataFactorySession final {
	  public:
		explicit DataFactorySession(
			Universe &universe, size_t checkpointLimit = 16, size_t checkpointBytes = 64 * 1024 * 1024
		);

		void SetRehydrate(DataFactoryRehydrate rehydrate);
		void SetRehydrator(DataFactoryRehydrator rehydrator);
		void SetForkRehydrator(DataFactoryForkRehydrator rehydrator);
		void SetPauseParticipant(DataFactoryPauseParticipant participant);
		void SetWorldLifecycle(DataFactoryWorldLifecycle lifecycle);
		void SetInterventionExecutor(DataFactoryInterventionExecutor executor);
		void SetActionExecutor(DataFactoryActionExecutor executor);
		void SetReplayActionExecutor(DataFactoryReplayActionExecutor executor);
		void SetRenderOnlyPresenter(DataFactoryRenderOnlyPresenter presenter);

		DataFactoryReply
		Pause(std::string_view instanceId, DataFactoryPauseScope scope, uint64_t expectedTick);
		DataFactoryReply Resume(std::string_view instanceId, uint64_t expectedTick);
		DataFactoryReply Step(
			std::string_view instanceId,
			DataFactoryInterval interval,
			uint64_t expectedTick,
			uint64_t expectedVersion,
			std::span<const DataFactoryAction> actions = {}
		);
		// Returns the lifecycle revision without retaining a snapshot or changing
		// the world, so adapters can reject stale requests before mutation.
		DataFactoryReply Inspect(std::string_view instanceId) const;

		// The host reads this before presentation or input work so it does not
		// keep a second pause map beside the lifecycle session.
		bool AllSystemsPaused(std::string_view instanceId) const;

		// The maximum serialized checkpoint size accepted by this session. Host
		// extensions use the same bound when preparing an atomic candidate.
		size_t CheckpointByteLimit() const {
			return MaximumCheckpointBytes;
		}

		DataFactoryReply Snapshot(std::string_view instanceId, std::string &snapshotId);
		DataFactoryReply RenderSnapshotBarrier(std::string_view instanceId, std::string_view snapshotId);
		// Begins one host-owned frame. A successful reply is Pending until the
		// host validates and completes the operation after renderer submission.
		DataFactoryRenderOnlyReply RenderOnly(DataFactoryRenderOnlyRequest request);
		DataFactoryRenderOnlyReply
		ValidateRenderOnlySubmission(std::string_view instanceId, uint64_t operationId);
		DataFactoryRenderOnlyReply CompleteRenderOnly(DataFactoryRenderOnlyCompletion completion);
		DataFactoryRenderOnlyReply PollRenderOnly(std::string_view instanceId, uint64_t operationId) const;
		DataFactoryReply Checkpoint(std::string_view instanceId, std::string &checkpointId);
		// Restores the retained checkpoint into a separately owned universe. The
		// parent remains byte-for-byte untouched if preparation or validation fails.
		DataFactoryReply Fork(const DataFactoryForkRequest &request);
		DataFactoryReply Restore(std::string_view instanceId, std::string_view checkpointId);
		// Rebuilds an earlier paused state in scratch, then replays contiguous
		// fixed steps. Action-bearing ticks require the candidate-safe executor.
		DataFactoryReply SeekBackward(
			std::string_view instanceId, uint64_t targetTick, uint64_t expectedTick, uint64_t expectedVersion
		);
		// Advances the lifecycle revision after an external executor completed an
		// atomic mutation against the live, all-systems-paused world.
		DataFactoryReply
		CommitExternalMutation(std::string_view instanceId, uint64_t expectedTick, uint64_t expectedVersion);
		DataFactoryReply CreateWorld(const DataFactoryWorldRequest &request);
		DataFactoryReply ResetWorld(const DataFactoryWorldRequest &request);
		DataFactoryReply RetireWorld(const DataFactoryWorldRequest &request);
		DataFactoryReply ApplyIntervention(
			std::string_view instanceId,
			std::string_view baseSnapshotId,
			std::span<const DataFactoryIntervention> changes,
			uint64_t expectedTick,
			uint64_t expectedVersion
		);
		bool SupportsIntervention() const;
		bool SupportsBackwardSeek() const {
			return static_cast<bool>(Rehydrate);
		}

		bool HasCheckpoint(std::string_view checkpointId) const;
		bool OwnsFork(std::string_view branchId) const;
		Universe *ForkUniverse(std::string_view branchId);
		const Universe *ForkUniverse(std::string_view branchId) const;
		// Product hosts use this to distinguish an MCP-owned world from a compatibility world.
		bool OwnsWorld(std::string_view instanceId) const;
		// Control adapters enter a verified lifecycle world through this one owner.
		Universe &UniverseOf() const {
			return Worlds;
		}

	  private:
		struct PauseState {
			bool AllSystems = false;
			bool PhysicsOnly = false;
			std::optional<DataFactoryRenderOnlyRequest> RenderOnly;
		};

		WorldId Resolve(std::string_view instanceId) const;
		DataFactoryClock ClockOf(WorldId world) const;
		DataFactoryClock ClockOf(const Universe &universe, WorldId world) const;
		DataFactoryReply Reply(WorldId world, DataFactoryStatus status, std::string detail) const;
		DataFactoryRenderOnlyReply RenderReply(
			WorldId world,
			DataFactoryStatus status,
			DataFactoryTemporalHistory temporalHistory,
			std::string detail,
			bool presented = false,
			uint64_t operationId = 0
		) const;
		bool IsCanonicalInterval(WorldId world, DataFactoryInterval interval) const;
		bool RenderOnlyInFlight(std::string_view instanceId) const;
		DataFactoryRenderOnlyReply
		ValidateRenderOnlySubmission(WorldId world, const DataFactoryRenderOnlyRequest &request);
		void FinishRenderOnly(PauseState &state, DataFactoryRenderOnlyReply reply);
		void StoreRenderOnlyTerminal(DataFactoryRenderOnlyReply reply);
		void Store(DataFactoryCheckpoint checkpoint);
		void BeginReplayGeneration();
		DataFactoryReply WorldOperation(const DataFactoryWorldRequest &request);
		void InvalidateWorldState(std::string_view instanceId);

		Universe &Worlds;
		size_t Limit = 0;
		size_t MaximumCheckpointBytes = 0;
		uint64_t Epoch = 1;
		uint64_t Version = 0;
		uint64_t NextCheckpoint = 1;
		uint64_t NextRenderOnly = 1;
		uint64_t LastForkEpoch = 1;
		uint64_t ReplayGeneration = 1;
		DataFactoryRehydrate Rehydrate;
		DataFactoryRehydrateCommit RehydrateCommit;
		DataFactoryRehydrateAbort RehydrateAbort;
		DataFactoryForkPrepare ForkPrepare;
		DataFactoryForkCommit ForkCommit;
		DataFactoryForkAbort ForkAbort;
		DataFactoryForkRetire ForkRetire;
		DataFactoryPauseParticipant Participant;
		DataFactoryWorldLifecycle WorldLifecycle;
		DataFactoryInterventionExecutor InterventionExecutor;
		DataFactoryActionExecutor ActionExecutor;
		DataFactoryReplayActionExecutor ReplayActionExecutor;
		DataFactoryRenderOnlyPresenter Presenter;
		struct OwnedWorld {
			// Retain episode metadata beside ownership until package manifests carry it.
			uint64_t Seed = 0;
			double TickRate = 60.0;
		};
		struct WorldOperationRecord {
			DataFactoryWorldRequest Request;
			DataFactoryReply Reply;
		};
		std::unordered_map<std::string, WorldOperationRecord> WorldOperations;
		std::unordered_map<std::string, OwnedWorld> OwnedWorlds;
		std::unordered_map<std::string, PauseState> Paused;
		std::unordered_map<uint64_t, DataFactoryRenderOnlyReply> RenderOnlyTerminals;
		std::deque<uint64_t> RenderOnlyTerminalOrder;
		std::unordered_map<std::string, DataFactoryCheckpoint> Checkpoints;
		std::vector<std::string> CheckpointOrder;
		struct ForkedWorld {
			std::unique_ptr<Universe> Realm;
			WorldId World;
			uint64_t Epoch = 0;
			uint64_t Version = 0;
		};
		std::unordered_map<std::string, ForkedWorld> Forks;
		struct ReplayStep {
			uint64_t Generation = 0;
			uint64_t BeforeTick = 0;
			DataFactoryInterval Interval;
			std::vector<DataFactoryAction> Actions;
		};
		std::deque<ReplayStep> ReplaySteps;
		size_t RetainedCheckpointBytes = 0;
	};

	const char *Describe(DataFactoryStatus status);
}
