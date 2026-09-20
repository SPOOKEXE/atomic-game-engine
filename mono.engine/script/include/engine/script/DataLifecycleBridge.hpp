#pragma once

// A runtime-local command queue for the host-owned data-factory session.
// Script only exchanges copied records and never touches a world lifecycle
// object while a VM call is active.
// @tier L9 · shared

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

namespace engine::world {
	class DataFactorySession;
}

namespace engine::script {
	// A lifecycle request exchanged with the host.
	struct DataLifecycleBridgeRequest {
		// Data-factory instance identifier.
		std::string InstanceId;
		// Requested lifecycle verb, such as step, checkpoint, or restore.
		std::string Operation;
		// Caller-generated identity used to poll and correlate this operation.
		std::string OperationId;
		// Tick that must still be current before the host applies the operation.
		uint64_t ExpectedTick = 0;
		// World incarnation that must still match before the host applies the operation.
		uint64_t ExpectedWorldEpoch = 0;
		// State version that must still match before the host applies the operation.
		uint64_t ExpectedWorldVersion = 0;
		// Step duration numerator in nanoseconds for rational fixed-tick advancement.
		uint64_t DtNumeratorNanoseconds = 0;
		// Divisor paired with DtNumeratorNanoseconds.
		uint32_t DtDenominator = 0;
		// Named lifecycle scope selected by the host's operation policy.
		std::string Scope;
		// Stable checkpoint identity used by checkpoint and restore operations.
		std::string CheckpointId;
		// Snapshot that produced this record.
		std::string SnapshotId;
		// Requested retained-history policy for operations that present a frame.
		std::string TemporalHistory;
	};

	// A lifecycle reply exchanged with the host.
	struct DataLifecycleBridgeReply {
		// Machine-readable operation status.
		std::string Status;
		// Human-readable diagnostic.
		std::string Detail;
		// Data-factory instance identifier.
		std::string InstanceId;
		// Snapshot that produced this record.
		std::string SnapshotId;
		// Stable checkpoint identity returned by a checkpoint operation.
		std::string CheckpointId;
		// World tick for this record.
		std::string Tick;
		// Elapsed world time in nanoseconds.
		std::string TimeNanoseconds;
		// Version of the world state observed after the operation.
		std::string Version;
		// Incarnation epoch of the world observed after the operation.
		std::string Epoch;
		// Caller-generated identity copied from the completed request.
		std::string OperationId;
		// Retained-history policy applied to any frame produced by the operation.
		std::string TemporalHistory;
	};

	// A lifecycle capabilities exchanged with the host.
	struct DataLifecycleBridgeCapabilities {
		// Whether this capability is available.
		bool Available = false;
		// Human-readable diagnostic.
		std::string Detail;
	};

	// One completed request boundary. Hosts that need an observation between
	// deterministic steps use this copied result without receiving a world or
	// store handle.
	struct DataLifecyclePumpResult {
		// True when PumpOne consumed a queued request or terminal reply.
		bool Processed = false;
		// True when the request waits only for its retained render result.
		bool PendingRenderOnly = false;
		// True when this operation changed the live world state.
		bool WorldMutated = false;
		// True when this operation completed one deterministic world tick.
		bool CompletedTick = false;
		// True when this operation restored a checkpoint into the live world.
		bool Restored = false;
		// Data-factory instance identifier.
		std::string InstanceId;
	};

	// Host boundary for copied lifecycle requests and ticketed replies.
	class DataLifecycleBridge {
	  public:
		virtual ~DataLifecycleBridge() = default;
		// Reports which lifecycle operations this host bridge can service.
		virtual DataLifecycleBridgeCapabilities Capabilities() const = 0;
		// Queues a lifecycle operation for the host.
		virtual bool Queue(
			std::string_view instanceId,
			const DataLifecycleBridgeRequest &request,
			uint64_t &ticket,
			std::string &detail
		) = 0;
		// Reads the current lifecycle operation result.
		virtual bool Poll(
			std::string_view instanceId, uint64_t ticket, DataLifecycleBridgeReply &reply, std::string &detail
		) = 0;
		// Releases a completed lifecycle operation.
		virtual bool Release(std::string_view instanceId, uint64_t ticket, std::string &detail) = 0;
	};

	// A host calls Pump at its completed-tick barrier. It is intentionally not a
	// runtime method: the owner decides when lifecycle mutations are safe.
	class QueuedDataLifecycleBridge final : public DataLifecycleBridge {
	  public:
		// Binds this bridge to the host session that owns lifecycle mutations.
		explicit QueuedDataLifecycleBridge(world::DataFactorySession &session);
		~QueuedDataLifecycleBridge() override;
		// Reports lifecycle operations supported by the bound host session.
		DataLifecycleBridgeCapabilities Capabilities() const override;
		// Queues a lifecycle operation for the host.
		bool Queue(std::string_view, const DataLifecycleBridgeRequest &, uint64_t &, std::string &) override;
		// Reads the current lifecycle operation result.
		bool Poll(std::string_view, uint64_t, DataLifecycleBridgeReply &, std::string &) override;
		// Releases a completed lifecycle operation.
		bool Release(std::string_view, uint64_t, std::string &) override;
		// Processes at most one queued request or terminal render-only reply. It
		// checks the bounded retained render order first, so a completed reply
		// cannot hide behind a newer pending request.
		DataLifecyclePumpResult PumpOne();

		// Drains the bridge for hosts that do not need an observer between
		// completed requests. Equivalent to repeatedly calling PumpOne().
		void Pump();

	  private:
		struct State;
		world::DataFactorySession &Session;
		std::unique_ptr<State> QueueState;
	};
}
