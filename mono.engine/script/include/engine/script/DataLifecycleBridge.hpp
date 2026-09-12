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
	struct DataLifecycleBridgeRequest {
		std::string InstanceId;
		std::string Operation;
		std::string OperationId;
		uint64_t ExpectedTick = 0;
		uint64_t ExpectedWorldEpoch = 0;
		uint64_t ExpectedWorldVersion = 0;
		uint64_t DtNumeratorNanoseconds = 0;
		uint32_t DtDenominator = 0;
		std::string Scope;
		std::string CheckpointId;
	};

	struct DataLifecycleBridgeReply {
		std::string Status;
		std::string Detail;
		std::string InstanceId;
		std::string SnapshotId;
		std::string CheckpointId;
		std::string Tick;
		std::string TimeNanoseconds;
		std::string Version;
		std::string Epoch;
	};

	struct DataLifecycleBridgeCapabilities {
		bool Available = false;
		std::string Detail;
	};

	class DataLifecycleBridge {
	  public:
		virtual ~DataLifecycleBridge() = default;
		virtual DataLifecycleBridgeCapabilities Capabilities() const = 0;
		virtual bool Queue(
			std::string_view instanceId,
			const DataLifecycleBridgeRequest &request,
			uint64_t &ticket,
			std::string &detail
		) = 0;
		virtual bool Poll(
			std::string_view instanceId, uint64_t ticket, DataLifecycleBridgeReply &reply, std::string &detail
		) = 0;
		virtual bool Release(std::string_view instanceId, uint64_t ticket, std::string &detail) = 0;
	};

	// A host calls Pump at its completed-tick barrier. It is intentionally not a
	// runtime method: the owner decides when lifecycle mutations are safe.
	class QueuedDataLifecycleBridge final : public DataLifecycleBridge {
	  public:
		explicit QueuedDataLifecycleBridge(world::DataFactorySession &session);
		~QueuedDataLifecycleBridge() override;
		DataLifecycleBridgeCapabilities Capabilities() const override;
		bool Queue(std::string_view, const DataLifecycleBridgeRequest &, uint64_t &, std::string &) override;
		bool Poll(std::string_view, uint64_t, DataLifecycleBridgeReply &, std::string &) override;
		bool Release(std::string_view, uint64_t, std::string &) override;
		void Pump();

	  private:
		struct State;
		world::DataFactorySession &Session;
		std::unique_ptr<State> QueueState;
	};
}
