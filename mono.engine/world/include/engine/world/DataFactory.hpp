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

	// Upper bound on actions accepted at one fixed-tick boundary.
	inline constexpr size_t MAXIMUM_DATA_FACTORY_ACTIONS = 32;

	// Outcomes returned by data-factory lifecycle operations.
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

	// Subsystems a lifecycle pause may freeze.
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
		// Whole nanoseconds represented by Denominator ticks.
		uint64_t NumeratorNanoseconds = 1'000'000'000;
		// Number of ticks represented by NumeratorNanoseconds.
		uint32_t Denominator = 60;
	};

	// Exact simulation time exposed with a lifecycle reply.
	struct DataFactoryClock {
		// Simulation tick at which the reply was produced.
		uint64_t Tick = 0;
		// Elapsed simulation time in nanoseconds.
		uint64_t TimeNanoseconds = 0;
		// Whether Interval describes the clock without floating-point rounding.
		bool RationalTimeAvailable = false;
		// Tick interval when RationalTimeAvailable is true.
		DataFactoryInterval Interval;
	};

	// Result of a lifecycle operation against one managed world.
	struct DataFactoryReply {
		// Outcome of the requested operation.
		DataFactoryStatus Status = DataFactoryStatus::Unsupported;
		// Human-readable refusal or failure reason.
		std::string Detail;
		// Stable identifier of the addressed instance.
		std::string InstanceId;
		// Lifecycle epoch observed by the operation.
		uint64_t WorldEpoch = 0;
		// State revision observed by the operation.
		uint64_t WorldVersion = 0;
		// Simulation clock captured with the reply.
		DataFactoryClock Clock;
		// Whether the addressed instance has been retired.
		bool Tombstone = false;
	};

	// Lifecycle changes a host may request for a managed world.
	enum class DataFactoryWorldOperation : uint8_t { Create, Reset, Retire };

	// Preconditions and metadata for a world lifecycle change.
	struct DataFactoryWorldRequest {
		// Lifecycle change to apply.
		DataFactoryWorldOperation Operation = DataFactoryWorldOperation::Create;
		// Stable identifier assigned to the managed world.
		std::string InstanceId;
		// Episode metadata is retained for a later package or manifest. WorldSettings
		// intentionally has no seed because it does not own product random sources.
		uint64_t Seed = 0;
		// Simulation ticks per second for a newly created or reset world.
		double TickRate = 60.0;
		// Lifecycle epoch required before applying the request.
		uint64_t ExpectedWorldEpoch = 0;
		// State revision required before applying the request.
		uint64_t ExpectedWorldVersion = 0;
		// Simulation tick required before applying the request.
		uint64_t ExpectedTick = 0;
		// Caller-provided idempotency key for this lifecycle request.
		std::string OperationId;
	};

	// A value-only request crosses from the shared world layer to the host that
	// owns presentation. No store, renderer, GPU handle, or callback-owned state
	// leaks through this boundary, so a headless host can refuse it honestly.
	struct DataFactoryRenderOnlyRequest {
		// Stable identifier of the paused world to present.
		std::string InstanceId;
		// Snapshot that was proven current at the render barrier.
		std::string SnapshotId;
		// Lifecycle epoch that must still match before submission.
		uint64_t ExpectedWorldEpoch = 0;
		// State revision that must still match before submission.
		uint64_t ExpectedWorldVersion = 0;
		// Paused tick that must still match before submission.
		uint64_t ExpectedTick = 0;
		// Unique operation ticket chosen by the session.
		uint64_t OperationId = 0;
		// Renderer-local temporal-history treatment requested by the host.
		DataFactoryTemporalHistory TemporalHistory = DataFactoryTemporalHistory::Preserve;
	};

	// Lifecycle reply augmented with render-only submission state.
	struct DataFactoryRenderOnlyReply : DataFactoryReply {
		// Unique ticket for polling this render-only operation.
		uint64_t OperationId = 0;
		// Temporal-history treatment accepted by the host.
		DataFactoryTemporalHistory TemporalHistory = DataFactoryTemporalHistory::Preserve;
		// Whether the host began presentation for the paused snapshot.
		bool Presented = false;
	};

	// Completion supplied by the host after a render-only submission.
	struct DataFactoryRenderOnlyCompletion {
		// Stable identifier of the world that was presented.
		std::string InstanceId;
		// Ticket returned by RenderOnly.
		uint64_t OperationId = 0;
		// Whether the renderer accepted the submission.
		bool Submitted = false;
		// Host-provided submission failure detail.
		std::string Detail;
	};

	// Retained serialized state used to restore or fork a world.
	struct DataFactoryCheckpoint {
		// Stable checkpoint identifier issued by the session.
		std::string Id;
		// Lifecycle epoch at which the checkpoint was written.
		uint64_t Epoch = 0;
		// World state revision at which the checkpoint was written.
		uint64_t Version = 0;
		// Simulation tick represented by the checkpoint.
		uint64_t Tick = 0;
		// Replay history generation compatible with this checkpoint.
		uint64_t ReplayGeneration = 0;
		// Whether the full scheduler was paused when captured.
		bool AllSystemsPaused = false;
		// Whether only physics was paused when captured.
		bool PhysicsOnlyPaused = false;
		// Owned serialized universe image.
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
	// Discards host state prepared for a rejected rehydration candidate.
	using DataFactoryRehydrateAbort = std::function<void()>;
	// Hooks that atomically prepare, publish, or discard restored host state.
	struct DataFactoryRehydrator {
		// Builds host state against the scratch universe.
		DataFactoryRehydrate Prepare;
		// Publishes the prepared host state after replacement commits.
		DataFactoryRehydrateCommit Commit;
		// Discards prepared host state when replacement fails.
		DataFactoryRehydrateAbort Abort;
	};

	// Rebuilds and publishes host state for an isolated fork. Fork preparation
	// cannot reuse the live-world rehydrator: its commit owns runtime state for
	// the parent universe. The branch id is the host's stable ownership key.
	using DataFactoryForkPrepare = std::function<bool(std::string_view, Universe &, WorldId, std::string &)>;
	// Publishes host state prepared for the named fork.
	using DataFactoryForkCommit = std::function<void(std::string_view)>;
	// Discards host state prepared for the named fork.
	using DataFactoryForkAbort = std::function<void(std::string_view)>;
	// Releases host state after the named fork is retired.
	using DataFactoryForkRetire = std::function<void(std::string_view)>;
	// Hooks that manage product-owned state for isolated forks.
	struct DataFactoryForkRehydrator {
		// Builds fork-owned host state against the scratch universe.
		DataFactoryForkPrepare Prepare;
		// Publishes the prepared fork host state.
		DataFactoryForkCommit Commit;
		// Discards a rejected fork candidate.
		DataFactoryForkAbort Abort;
		// Releases state held by a retired fork.
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
		// Supported representation alternatives for a property value.
		enum class Kind : uint8_t { Missing, Boolean, Integer, Number, String };

		// Active member of this tagged value.
		Kind Type = Kind::Missing;
		// Value when Type is Boolean.
		bool Boolean = false;
		// Value when Type is Integer.
		int64_t Integer = 0;
		// Value when Type is Number.
		double Number = 0.0;
		// Value when Type is String.
		std::string String;
	};

	// One optimistic property change applied as part of an intervention batch.
	struct DataFactoryIntervention {
		// Stable identifier of the target instance.
		std::string TargetId;
		// Product-defined property path on the target.
		std::string Path;
		// Value required before the change is applied.
		DataFactoryInterventionValue Expected;
		// Replacement value to write when Expected still matches.
		DataFactoryInterventionValue Value;
	};

	// The product owns scene-property vocabulary and must apply every row or no
	// rows. The session restores its encoded rollback point if this refuses.
	using DataFactoryInterventionExecutor =
		std::function<bool(Universe &, WorldId, std::span<const DataFactoryIntervention>, std::string &)>;

	// A named script-authored settings-menu action to deliver at one fixed-tick
	// boundary. The host validates its vocabulary before queuing any action.
	struct DataFactoryAction {
		// Script-authored action name accepted by the host executor.
		std::string Name;
	};

	// Forks one retained parent checkpoint into a separately owned universe.
	// BranchId names that isolated runtime. The encoded world keeps its source
	// name because Universe deliberately has no in-place rename operation.
	struct DataFactoryForkRequest {
		// Stable identifier of the parent world.
		std::string InstanceId;
		// Retained parent checkpoint used as the fork source.
		std::string CheckpointId;
		// Stable ownership name for the isolated runtime.
		std::string BranchId;
		// Parent lifecycle epoch required for the fork.
		uint64_t ExpectedWorldEpoch = 0;
		// Parent state revision required for the fork.
		uint64_t ExpectedWorldVersion = 0;
		// Parent simulation tick required for the fork.
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

	// Owns managed-world lifecycle state, checkpoints, and host integration hooks.
	class DataFactorySession final {
	  public:
		// Starts a lifecycle session with bounded checkpoint retention.
		explicit DataFactorySession(
			Universe &universe, size_t checkpointLimit = 16, size_t checkpointBytes = 64 * 1024 * 1024
		);

		// Installs the scratch-universe rehydration hook.
		void SetRehydrate(DataFactoryRehydrate rehydrate);
		// Installs atomic prepare, commit, and abort rehydration hooks.
		void SetRehydrator(DataFactoryRehydrator rehydrator);
		// Installs product hooks for isolated forks.
		void SetForkRehydrator(DataFactoryForkRehydrator rehydrator);
		// Installs the participant that freezes external subsystems.
		void SetPauseParticipant(DataFactoryPauseParticipant participant);
		// Installs the product lifecycle callback for world changes.
		void SetWorldLifecycle(DataFactoryWorldLifecycle lifecycle);
		// Installs the atomic scene-property intervention executor.
		void SetInterventionExecutor(DataFactoryInterventionExecutor executor);
		// Installs the live-world fixed-tick action executor.
		void SetActionExecutor(DataFactoryActionExecutor executor);
		// Installs the scratch-world replay action executor.
		void SetReplayActionExecutor(DataFactoryReplayActionExecutor executor);
		// Installs the host-owned render-only presenter.
		void SetRenderOnlyPresenter(DataFactoryRenderOnlyPresenter presenter);

		// Pauses the requested subsystem scope at the expected tick.
		DataFactoryReply
		Pause(std::string_view instanceId, DataFactoryPauseScope scope, uint64_t expectedTick);
		// Resumes a world paused at the expected tick.
		DataFactoryReply Resume(std::string_view instanceId, uint64_t expectedTick);
		// Advances one paused fixed tick after validating all action names.
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

		// Serializes a paused world and returns its retained snapshot id.
		DataFactoryReply Snapshot(std::string_view instanceId, std::string &snapshotId);
		// Verifies that a snapshot remains current for render-only presentation.
		DataFactoryReply RenderSnapshotBarrier(std::string_view instanceId, std::string_view snapshotId);
		// Begins one host-owned frame. A successful reply is Pending until the
		// host validates and completes the operation after renderer submission.
		DataFactoryRenderOnlyReply RenderOnly(DataFactoryRenderOnlyRequest request);
		// Confirms that a pending render-only ticket may be submitted.
		DataFactoryRenderOnlyReply
		ValidateRenderOnlySubmission(std::string_view instanceId, uint64_t operationId);
		// Stores the host result for a submitted render-only ticket.
		DataFactoryRenderOnlyReply CompleteRenderOnly(DataFactoryRenderOnlyCompletion completion);
		// Returns the current state of a render-only ticket.
		DataFactoryRenderOnlyReply PollRenderOnly(std::string_view instanceId, uint64_t operationId) const;
		// Stores a retained checkpoint for a paused world.
		DataFactoryReply Checkpoint(std::string_view instanceId, std::string &checkpointId);
		// Restores the retained checkpoint into a separately owned universe. The
		// parent remains byte-for-byte untouched if preparation or validation fails.
		DataFactoryReply Fork(const DataFactoryForkRequest &request);
		// Replaces a paused world with a retained checkpoint.
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
		// Creates a managed world after validating lifecycle preconditions.
		DataFactoryReply CreateWorld(const DataFactoryWorldRequest &request);
		// Recreates a managed world while preserving its stable name.
		DataFactoryReply ResetWorld(const DataFactoryWorldRequest &request);
		// Retires a managed world and its host-owned state.
		DataFactoryReply RetireWorld(const DataFactoryWorldRequest &request);
		// Applies an atomic optimistic property-change batch.
		DataFactoryReply ApplyIntervention(
			std::string_view instanceId,
			std::string_view baseSnapshotId,
			std::span<const DataFactoryIntervention> changes,
			uint64_t expectedTick,
			uint64_t expectedVersion
		);
		// Reports whether an intervention executor is installed.
		bool SupportsIntervention() const;
		// Reports whether a rehydrator enables scratch-world replay.
		bool SupportsBackwardSeek() const {
			return static_cast<bool>(Rehydrate);
		}

		// Reports whether a checkpoint remains retained.
		bool HasCheckpoint(std::string_view checkpointId) const;
		// Reports whether this session owns the named fork.
		bool OwnsFork(std::string_view branchId) const;
		// Returns the mutable isolated universe for a session-owned fork.
		Universe *ForkUniverse(std::string_view branchId);
		// Returns the isolated universe for a session-owned fork.
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

	// Returns the stable text name for a lifecycle status.
	const char *Describe(DataFactoryStatus status);
}
