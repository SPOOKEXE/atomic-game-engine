#include <engine/script/DataLifecycleBridge.hpp>
#include <engine/world/DataFactory.hpp>

#include <deque>
#include <mutex>
#include <unordered_map>

namespace engine::script {
	namespace {
		constexpr size_t LIMIT = 64;

		bool SameRequest(const DataLifecycleBridgeRequest &left, const DataLifecycleBridgeRequest &right) {
			return left.InstanceId == right.InstanceId && left.Operation == right.Operation &&
				   left.OperationId == right.OperationId && left.ExpectedTick == right.ExpectedTick &&
				   left.ExpectedWorldEpoch == right.ExpectedWorldEpoch &&
				   left.ExpectedWorldVersion == right.ExpectedWorldVersion &&
				   left.DtNumeratorNanoseconds == right.DtNumeratorNanoseconds &&
				   left.DtDenominator == right.DtDenominator && left.Scope == right.Scope &&
				   left.CheckpointId == right.CheckpointId;
		}

		bool ValidText(std::string_view text, size_t limit, bool required = false) {
			return (!required || !text.empty()) && text.size() <= limit &&
				   text.find('\0') == std::string_view::npos;
		}

		bool KnownOperation(std::string_view operation) {
			return operation == "inspect" || operation == "pause" || operation == "resume" ||
				   operation == "step" || operation == "snapshot" || operation == "checkpoint" ||
				   operation == "restore";
		}

		DataLifecycleBridgeReply Reply(
			const world::DataFactoryReply &source, std::string snapshotId = {}, std::string checkpointId = {}
		) {
			return {
				.Status = world::Describe(source.Status),
				.Detail = source.Detail,
				.InstanceId = source.InstanceId,
				.SnapshotId = std::move(snapshotId),
				.CheckpointId = std::move(checkpointId),
				.Tick = std::to_string(source.Clock.Tick),
				.TimeNanoseconds = std::to_string(source.Clock.TimeNanoseconds),
				.Version = std::to_string(source.WorldVersion),
				.Epoch = std::to_string(source.WorldEpoch),
			};
		}
	}

	struct QueuedDataLifecycleBridge::State {
		struct Entry {
			DataLifecycleBridgeRequest Request;
			DataLifecycleBridgeReply Reply;
			bool Done = false;
		};
		struct Operation {
			DataLifecycleBridgeRequest Request;
			uint64_t Ticket = 0;
		};
		std::mutex Mutex;
		uint64_t Next = 1;
		std::unordered_map<uint64_t, Entry> Entries;
		std::unordered_map<uint64_t, DataLifecycleBridgeReply> Released;
		std::deque<uint64_t> Order;
		std::deque<uint64_t> ReleasedOrder;
		std::unordered_map<std::string, Operation> Operations;
	};

	QueuedDataLifecycleBridge::QueuedDataLifecycleBridge(world::DataFactorySession &session)
		: Session(session), QueueState(std::make_unique<State>()) {}

	QueuedDataLifecycleBridge::~QueuedDataLifecycleBridge() = default;

	DataLifecycleBridgeCapabilities QueuedDataLifecycleBridge::Capabilities() const {
		return {.Available = true, .Detail = "host pump required after the completed tick"};
	}

	bool QueuedDataLifecycleBridge::Queue(
		std::string_view instanceId,
		const DataLifecycleBridgeRequest &request,
		uint64_t &ticket,
		std::string &detail
	) {
		std::lock_guard lock(QueueState->Mutex);
		if (instanceId != request.InstanceId || !ValidText(request.InstanceId, 256, true) ||
			!ValidText(request.Operation, 32, true) || !KnownOperation(request.Operation) ||
			!ValidText(request.OperationId, 128) || !ValidText(request.Scope, 32) ||
			!ValidText(request.CheckpointId, 256) ||
			(!request.Scope.empty() && (request.Operation != "pause" || (request.Scope != "all_systems" &&
																		 request.Scope != "physics_only"))) ||
			(!request.CheckpointId.empty() && request.Operation != "restore") || QueueState->Next == 0) {
			detail = "invalid or full lifecycle queue";
			return false;
		}
		if (!request.OperationId.empty()) {
			if (const auto previous = QueueState->Operations.find(request.OperationId);
				previous != QueueState->Operations.end()) {
				if (!SameRequest(previous->second.Request, request)) {
					detail = "operation_id was already used with different arguments";
					return false;
				}
				ticket = previous->second.Ticket;
				return true;
			}
		}
		if (QueueState->Entries.size() >= LIMIT) {
			detail = "invalid or full lifecycle queue";
			return false;
		}
		ticket = QueueState->Next++;
		QueueState->Entries.emplace(ticket, State::Entry{.Request = request, .Reply = {}, .Done = false});
		QueueState->Order.push_back(ticket);
		if (!request.OperationId.empty())
			QueueState->Operations.emplace(
				request.OperationId, State::Operation{.Request = request, .Ticket = ticket}
			);
		return true;
	}

	bool QueuedDataLifecycleBridge::Poll(
		std::string_view instanceId, uint64_t ticket, DataLifecycleBridgeReply &reply, std::string &detail
	) {
		std::lock_guard lock(QueueState->Mutex);
		const auto found = QueueState->Entries.find(ticket);
		if (found != QueueState->Entries.end()) {
			if (found->second.Request.InstanceId != instanceId) {
				detail = "unknown lifecycle ticket";
				return false;
			}
			reply = found->second.Reply;
			if (!found->second.Done) reply.Status = "pending";
			return true;
		}
		const auto released = QueueState->Released.find(ticket);
		if (released == QueueState->Released.end() || released->second.InstanceId != instanceId) {
			detail = "unknown lifecycle ticket";
			return false;
		}
		reply = released->second;
		return true;
	}

	bool
	QueuedDataLifecycleBridge::Release(std::string_view instanceId, uint64_t ticket, std::string &detail) {
		std::lock_guard lock(QueueState->Mutex);
		const auto found = QueueState->Entries.find(ticket);
		if (found == QueueState->Entries.end() || found->second.Request.InstanceId != instanceId ||
			!found->second.Done) {
			detail = "unknown or pending lifecycle ticket";
			return false;
		}
		QueueState->Released.emplace(ticket, std::move(found->second.Reply));
		QueueState->ReleasedOrder.push_back(ticket);
		QueueState->Entries.erase(found);
		while (QueueState->ReleasedOrder.size() > LIMIT) {
			const uint64_t expired = QueueState->ReleasedOrder.front();
			QueueState->ReleasedOrder.pop_front();
			QueueState->Released.erase(expired);
			for (auto operation = QueueState->Operations.begin(); operation != QueueState->Operations.end();
				 ++operation) {
				if (operation->second.Ticket == expired) {
					QueueState->Operations.erase(operation);
					break;
				}
			}
		}
		// Retain a bounded copy of the completed reply so a retry after release
		// cannot repeat a lifecycle mutation.
		return true;
	}

	void QueuedDataLifecycleBridge::Pump() {
		std::deque<std::pair<uint64_t, DataLifecycleBridgeRequest>> pending;
		{
			std::lock_guard lock(QueueState->Mutex);
			while (!QueueState->Order.empty()) {
				const uint64_t ticket = QueueState->Order.front();
				QueueState->Order.pop_front();
				if (const auto entry = QueueState->Entries.find(ticket);
					entry != QueueState->Entries.end() && !entry->second.Done)
					pending.emplace_back(ticket, entry->second.Request);
			}
		}
		for (const auto &[ticket, request] : pending) {
			world::DataFactoryReply outcome;
			std::string snapshotId;
			std::string checkpointId;
			const world::DataFactoryReply observed = Session.Inspect(request.InstanceId);
			const bool preconditions =
				request.Operation == "inspect" || (observed.Status == world::DataFactoryStatus::Ok &&
												   observed.Clock.Tick == request.ExpectedTick &&
												   observed.WorldEpoch == request.ExpectedWorldEpoch &&
												   observed.WorldVersion == request.ExpectedWorldVersion);
			if (!preconditions) outcome = observed;
			if (outcome.Status == world::DataFactoryStatus::Ok) {
				outcome.Status = world::DataFactoryStatus::VersionConflict;
				outcome.Detail = "lifecycle preconditions do not match current world";
			} else if (request.Operation == "pause")
				outcome = Session.Pause(
					request.InstanceId,
					request.Scope == "physics_only" ? world::DataFactoryPauseScope::PhysicsOnly
													: world::DataFactoryPauseScope::AllSystems,
					request.ExpectedTick
				);
			else if (request.Operation == "resume")
				outcome = Session.Resume(request.InstanceId, request.ExpectedTick);
			else if (request.Operation == "snapshot")
				outcome = Session.Snapshot(request.InstanceId, snapshotId);
			else if (request.Operation == "inspect")
				outcome = Session.Inspect(request.InstanceId);
			else if (request.Operation == "checkpoint")
				outcome = Session.Checkpoint(request.InstanceId, checkpointId);
			else if (request.Operation == "restore")
				outcome = Session.Restore(request.InstanceId, request.CheckpointId);
			else if (request.Operation == "step") {
				const world::DataFactoryReply state = Session.Inspect(request.InstanceId);
				outcome = state.Status == world::DataFactoryStatus::Ok
							  ? Session.Step(
									request.InstanceId,
									{.NumeratorNanoseconds = request.DtNumeratorNanoseconds,
									 .Denominator = request.DtDenominator},
									request.ExpectedTick,
									request.ExpectedWorldVersion
								)
							  : state;
			} else
				outcome = {
					.Status = world::DataFactoryStatus::ValidationFailed,
					.Detail = "unknown lifecycle operation",
					.InstanceId = request.InstanceId,
					.Clock = {}
				};
			std::lock_guard lock(QueueState->Mutex);
			if (auto found = QueueState->Entries.find(ticket);
				found != QueueState->Entries.end() && !found->second.Done) {
				found->second.Reply = Reply(outcome, std::move(snapshotId), std::move(checkpointId));
				found->second.Done = true;
			}
		}
	}
}
