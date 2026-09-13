#include <engine/core/Bytes.hpp>
#include <engine/world/DataFactory.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace engine::world {

	namespace {
		bool TimeAt(uint64_t tick, DataFactoryInterval interval, uint64_t &time) {
			if (interval.Denominator == 0) return false;
			const uint64_t whole = tick / interval.Denominator;
			const uint64_t remainder = tick % interval.Denominator;
			if (whole > std::numeric_limits<uint64_t>::max() / interval.NumeratorNanoseconds) return false;
			const uint64_t quotient = interval.NumeratorNanoseconds / interval.Denominator;
			const uint64_t fraction = interval.NumeratorNanoseconds % interval.Denominator;
			if (quotient != 0 && remainder > std::numeric_limits<uint64_t>::max() / quotient) return false;
			const uint64_t wholePartial = remainder * quotient;
			const uint64_t fractionalPartial = (remainder * fraction) / interval.Denominator;
			if (wholePartial > std::numeric_limits<uint64_t>::max() - fractionalPartial) return false;
			const uint64_t partial = wholePartial + fractionalPartial;
			const uint64_t seconds = whole * interval.NumeratorNanoseconds;
			if (seconds > std::numeric_limits<uint64_t>::max() - partial) return false;
			time = seconds + partial;
			return true;
		}
	}

	DataFactorySession::DataFactorySession(Universe &universe, size_t checkpointLimit, size_t checkpointBytes)
		: Worlds(universe), Limit(checkpointLimit), MaximumCheckpointBytes(checkpointBytes) {}

	void DataFactorySession::SetRehydrate(DataFactoryRehydrate rehydrate) {
		Rehydrate = std::move(rehydrate);
	}

	void DataFactorySession::SetPauseParticipant(DataFactoryPauseParticipant participant) {
		Participant = std::move(participant);
	}

	void DataFactorySession::SetWorldLifecycle(DataFactoryWorldLifecycle lifecycle) {
		WorldLifecycle = std::move(lifecycle);
	}

	void DataFactorySession::SetInterventionExecutor(DataFactoryInterventionExecutor executor) {
		InterventionExecutor = std::move(executor);
	}

	void DataFactorySession::SetRenderOnlyPresenter(DataFactoryRenderOnlyPresenter presenter) {
		Presenter = std::move(presenter);
	}

	DataFactoryReply DataFactorySession::CreateWorld(const DataFactoryWorldRequest &request) {
		DataFactoryWorldRequest copy = request;
		copy.Operation = DataFactoryWorldOperation::Create;
		return WorldOperation(copy);
	}

	DataFactoryReply DataFactorySession::ResetWorld(const DataFactoryWorldRequest &request) {
		DataFactoryWorldRequest copy = request;
		copy.Operation = DataFactoryWorldOperation::Reset;
		return WorldOperation(copy);
	}

	DataFactoryReply DataFactorySession::RetireWorld(const DataFactoryWorldRequest &request) {
		DataFactoryWorldRequest copy = request;
		copy.Operation = DataFactoryWorldOperation::Retire;
		return WorldOperation(copy);
	}

	void DataFactorySession::InvalidateWorldState(std::string_view instanceId) {
		Paused.erase(std::string(instanceId));
		Checkpoints.clear();
		CheckpointOrder.clear();
		RetainedCheckpointBytes = 0;
		RenderOnlyTerminals.clear();
		RenderOnlyTerminalOrder.clear();
	}

	DataFactoryReply DataFactorySession::WorldOperation(const DataFactoryWorldRequest &request) {
		const auto replyFor = [this, &request](DataFactoryStatus status, std::string detail) {
			DataFactoryReply reply = Reply(WorldId{}, status, std::move(detail));
			reply.InstanceId = request.InstanceId;
			return reply;
		};
		const auto invalid = [&replyFor](std::string detail) {
			return replyFor(DataFactoryStatus::ValidationFailed, std::move(detail));
		};
		if (request.InstanceId.empty() || request.InstanceId.size() > 128 ||
			request.InstanceId.find('\0') != std::string::npos)
			return invalid("instance_id must contain 1 to 128 bytes");
		if (request.OperationId.empty() || request.OperationId.size() > 128 ||
			request.OperationId.find('\0') != std::string::npos)
			return invalid("operation_id must contain 1 to 128 bytes");
		if (!std::isfinite(request.TickRate) || request.TickRate <= 0.0 || request.TickRate > 1000.0)
			return invalid("tick_rate must be finite and between 0 and 1000");
		const auto prior = WorldOperations.find(request.OperationId);
		if (prior != WorldOperations.end()) {
			const auto &old = prior->second.Request;
			if (old.InstanceId != request.InstanceId || old.Seed != request.Seed ||
				old.TickRate != request.TickRate || old.Operation != request.Operation ||
				old.ExpectedWorldEpoch != request.ExpectedWorldEpoch ||
				old.ExpectedWorldVersion != request.ExpectedWorldVersion ||
				old.ExpectedTick != request.ExpectedTick)
				return replyFor(
					DataFactoryStatus::OperationIdConflict,
					"operation_id was already used with different arguments"
				);
			return prior->second.Reply;
		}
		const WorldId existing = Resolve(request.InstanceId);
		const bool create = request.Operation == DataFactoryWorldOperation::Create;
		const bool reset = request.Operation == DataFactoryWorldOperation::Reset;
		if (!create && !reset && request.Operation != DataFactoryWorldOperation::Retire)
			return invalid("unknown world lifecycle operation");
		if (create && (existing.IsValid() || OwnedWorlds.contains(request.InstanceId)))
			return replyFor(DataFactoryStatus::VersionConflict, "factory world already exists");
		if (create && Worlds.Count() != 0)
			return replyFor(
				DataFactoryStatus::ResourceLimit, "a compatibility world already occupies this universe"
			);
		// Epoch and version are session-wide, so one session deliberately owns one
		// factory world until per-world lifecycle revisions exist.
		if (create && !OwnedWorlds.empty())
			return replyFor(DataFactoryStatus::ResourceLimit, "this session already owns a factory world");
		if (!create && (!existing.IsValid() || !OwnedWorlds.contains(request.InstanceId) ||
						request.ExpectedWorldEpoch != Epoch || request.ExpectedWorldVersion != Version ||
						request.ExpectedTick != ClockOf(existing).Tick))
			return replyFor(
				DataFactoryStatus::VersionConflict, "world revision, tick, or ownership is stale"
			);
		if (!create && RenderOnlyInFlight(request.InstanceId))
			return replyFor(DataFactoryStatus::VersionConflict, "render-only presentation is in flight");
		if (!create && !AllSystemsPaused(request.InstanceId))
			return replyFor(DataFactoryStatus::NotPaused, "reset and retire require an all_systems pause");
		if (WorldOperations.size() >= 256)
			return replyFor(DataFactoryStatus::ResourceLimit, "world operation ledger is full");
		if (Epoch == UINT64_MAX || Version == UINT64_MAX)
			return replyFor(DataFactoryStatus::ResourceLimit, "world lifecycle revision exhausted");

		WorldId world = existing;
		if (request.Operation == DataFactoryWorldOperation::Retire) {
			std::string detail;
			if (!Participant || !Participant(world, DataFactoryPauseScope::AllSystems, false, detail))
				return replyFor(
					DataFactoryStatus::RestoreIncomplete,
					detail.empty() ? "cannot release the all_systems pause" : detail
				);
			if (WorldLifecycle && !WorldLifecycle(request.Operation, Worlds, world, false, detail)) {
				std::string restoreDetail;
				(void)Participant(world, DataFactoryPauseScope::AllSystems, true, restoreDetail);
				return replyFor(
					DataFactoryStatus::RestoreIncomplete,
					detail.empty() ? "client lifecycle retirement refused the world" : detail
				);
			}
			const DataFactoryClock finalClock = ClockOf(world);
			if (Worlds.Destroy(world) != WorldStatus::Ok || Resolve(request.InstanceId).IsValid())
				return replyFor(
					DataFactoryStatus::RestoreIncomplete, "world retirement did not remove the world"
				);
			OwnedWorlds.erase(request.InstanceId);
			InvalidateWorldState(request.InstanceId);
			if (WorldLifecycle) {
				std::string ignored;
				(void)WorldLifecycle(request.Operation, Worlds, WorldId{}, true, ignored);
			}
			++Version;
			DataFactoryReply reply = replyFor(DataFactoryStatus::Ok, "factory world retired");
			reply.Clock = finalClock;
			reply.Tombstone = true;
			WorldOperations.emplace(request.OperationId, WorldOperationRecord{request, reply});
			return reply;
		}

		WorldSettings settings;
		settings.Name = core::Name(request.InstanceId.c_str());
		settings.TickRate = request.TickRate;
		// Save into the candidate even for create: presentation and bus configuration
		// belong to the host universe and must survive a no-world lifecycle commit.
		core::ByteWriter staged(0, MaximumCheckpointBytes);
		try {
			if (!Worlds.Save(staged))
				return replyFor(DataFactoryStatus::Unsupported, "world lifecycle cannot stage a replacement");
		} catch (const std::length_error &) {
			return replyFor(
				DataFactoryStatus::ResourceLimit, "world lifecycle staging exceeds the byte limit"
			);
		}
		Universe candidate;
		core::ByteReader reader(staged.Bytes());
		if (!candidate.Load(reader) || reader.Remaining() != 0)
			return replyFor(
				DataFactoryStatus::RestoreIncomplete, "world lifecycle could not stage a replacement"
			);
		if (reset) {
			world = candidate.Find(core::Name(request.InstanceId));
			if (!world.IsValid() || candidate.Destroy(world) != WorldStatus::Ok)
				return replyFor(DataFactoryStatus::RestoreIncomplete, "world reset could not stage removal");
		}
		WorldStatus status = WorldStatus::Ok;
		world = candidate.Create(settings, &status);
		if (status != WorldStatus::Ok || !world.IsValid() ||
			candidate.NameOf(world).Text() != request.InstanceId ||
			candidate.SettingsOf(world).TickRate != request.TickRate)
			return replyFor(
				DataFactoryStatus::RestoreIncomplete, "world lifecycle did not create the requested world"
			);
		if (WorldLifecycle) {
			std::string detail;
			if (!WorldLifecycle(request.Operation, candidate, world, false, detail)) {
				return replyFor(
					DataFactoryStatus::RestoreIncomplete,
					detail.empty() ? "client lifecycle installation refused the world" : detail
				);
			}
		}
		if (candidate.SetState(world, WorldState::Suspended) != WorldStatus::Ok)
			return replyFor(
				DataFactoryStatus::RestoreIncomplete, "world lifecycle could not suspend the candidate"
			);
		if (!Participant)
			return replyFor(DataFactoryStatus::Unsupported, "the host has not installed pause participants");
		std::string pauseDetail;
		if (!Participant(world, DataFactoryPauseScope::AllSystems, true, pauseDetail))
			return replyFor(
				DataFactoryStatus::RestoreIncomplete,
				pauseDetail.empty() ? "cannot establish the all_systems pause" : pauseDetail
			);
		Worlds.ReplaceWith(candidate);
		world = Worlds.Find(core::Name(request.InstanceId));
		if (!world.IsValid())
			return replyFor(DataFactoryStatus::RestoreIncomplete, "world replacement did not commit");
		if (WorldLifecycle) {
			// Replacement is the atomic commit. The product hook may now publish its
			// prepared state, but it cannot turn that completed mutation into a refusal.
			std::string ignored;
			(void)WorldLifecycle(request.Operation, Worlds, world, true, ignored);
		}
		OwnedWorlds[request.InstanceId] = OwnedWorld{request.Seed, request.TickRate};
		InvalidateWorldState(request.InstanceId);
		Paused[request.InstanceId].AllSystems = true;
		++Epoch;
		++Version;
		DataFactoryReply reply =
			Reply(world, DataFactoryStatus::Ok, reset ? "factory world reset" : "factory world created");
		WorldOperations.emplace(request.OperationId, WorldOperationRecord{request, reply});
		return reply;
	}

	bool DataFactorySession::OwnsWorld(std::string_view instanceId) const {
		return OwnedWorlds.contains(std::string(instanceId));
	}

	WorldId DataFactorySession::Resolve(std::string_view instanceId) const {
		return Worlds.Find(core::Name(instanceId));
	}

	DataFactoryClock DataFactorySession::ClockOf(WorldId world) const {
		DataFactoryClock clock;
		clock.Tick = Worlds.StatisticsOf(world).Ticks;
		const double rate = Worlds.SettingsOf(world).TickRate;
		const double rounded = std::round(rate);
		if (rate > 0.0 && rounded >= 1.0 && rounded <= static_cast<double>(UINT32_MAX) && rate == rounded) {
			clock.Interval.Denominator = static_cast<uint32_t>(rounded);
			clock.RationalTimeAvailable = true;
		}
		if (clock.RationalTimeAvailable && !TimeAt(clock.Tick, clock.Interval, clock.TimeNanoseconds))
			clock.RationalTimeAvailable = false;
		return clock;
	}

	DataFactoryReply
	DataFactorySession::Reply(WorldId world, DataFactoryStatus status, std::string detail) const {
		DataFactoryReply reply;
		reply.Status = status;
		reply.Detail = std::move(detail);
		reply.WorldEpoch = Epoch;
		reply.WorldVersion = Version;
		if (world.IsValid()) {
			reply.InstanceId = std::string(Worlds.NameOf(world).Text());
			reply.Clock = ClockOf(world);
		}
		return reply;
	}

	DataFactoryRenderOnlyReply DataFactorySession::RenderReply(
		WorldId world,
		DataFactoryStatus status,
		DataFactoryTemporalHistory temporalHistory,
		std::string detail,
		bool presented,
		uint64_t operationId
	) const {
		DataFactoryRenderOnlyReply reply;
		static_cast<DataFactoryReply &>(reply) = Reply(world, status, std::move(detail));
		reply.TemporalHistory = temporalHistory;
		reply.Presented = presented;
		reply.OperationId = operationId;
		return reply;
	}

	bool DataFactorySession::IsCanonicalInterval(WorldId world, DataFactoryInterval interval) const {
		const double rate = Worlds.SettingsOf(world).TickRate;
		if (!(rate > 0.0) || rate != std::round(rate) || rate > static_cast<double>(UINT32_MAX)) return false;
		const DataFactoryClock clock = ClockOf(world);
		return interval.NumeratorNanoseconds == clock.Interval.NumeratorNanoseconds &&
			   interval.Denominator == clock.Interval.Denominator;
	}

	bool DataFactorySession::RenderOnlyInFlight(std::string_view instanceId) const {
		const auto paused = Paused.find(std::string(instanceId));
		return paused != Paused.end() && paused->second.RenderOnly.has_value();
	}

	DataFactoryReply DataFactorySession::Pause(
		std::string_view instanceId, DataFactoryPauseScope scope, uint64_t expectedTick
	) {
		const WorldId world = Resolve(instanceId);
		if (!world.IsValid()) return Reply(world, DataFactoryStatus::ValidationFailed, "unknown instance_id");
		if (RenderOnlyInFlight(instanceId))
			return Reply(world, DataFactoryStatus::VersionConflict, "render-only presentation is in flight");
		if (ClockOf(world).Tick != expectedTick)
			return Reply(
				world, DataFactoryStatus::VersionConflict, "expected_tick does not match the completed tick"
			);
		if (scope == DataFactoryPauseScope::AllSystems && AllSystemsPaused(instanceId))
			return Reply(world, DataFactoryStatus::Ok, {});
		if (!Participant)
			return Reply(
				world, DataFactoryStatus::Unsupported, "the host has not installed pause participants"
			);

		std::string detail;
		if (!Participant(world, scope, true, detail))
			return Reply(
				world, DataFactoryStatus::Unsupported, detail.empty() ? "pause scope is unavailable" : detail
			);

		PauseState &paused = Paused[std::string(instanceId)];
		if (scope == DataFactoryPauseScope::AllSystems) {
			if (Worlds.SetState(world, WorldState::Suspended) != WorldStatus::Ok) {
				(void)Participant(world, scope, false, detail);
				return Reply(
					world, DataFactoryStatus::ValidationFailed, "world cannot pause at this boundary"
				);
			}
			paused.AllSystems = true;
		} else {
			paused.PhysicsOnly = true;
		}
		Version++;
		return Reply(world, DataFactoryStatus::Ok, {});
	}

	DataFactoryReply DataFactorySession::Resume(std::string_view instanceId, uint64_t expectedTick) {
		const WorldId world = Resolve(instanceId);
		if (!world.IsValid()) return Reply(world, DataFactoryStatus::ValidationFailed, "unknown instance_id");
		if (RenderOnlyInFlight(instanceId))
			return Reply(world, DataFactoryStatus::VersionConflict, "render-only presentation is in flight");
		if (ClockOf(world).Tick != expectedTick)
			return Reply(
				world, DataFactoryStatus::VersionConflict, "expected_tick does not match the completed tick"
			);
		const auto found = Paused.find(std::string(instanceId));
		if (found == Paused.end())
			return Reply(world, DataFactoryStatus::NotPaused, "instance is not paused by this session");

		std::string detail;
		bool resumedAllSystems = false;
		bool activatedWorld = false;
		const auto rollbackAllSystems = [&] {
			bool restored = true;
			if (activatedWorld && Worlds.SetState(world, WorldState::Suspended) != WorldStatus::Ok)
				restored = false;
			if (resumedAllSystems) {
				std::string rollbackDetail;
				if (!Participant(world, DataFactoryPauseScope::AllSystems, true, rollbackDetail))
					restored = false;
			}
			if (!restored) {
				// The host could not prove the all-system pause was restored, so no
				// later step or capture may rely on the stale session claim.
				found->second.AllSystems = false;
				Version++;
			}
			return restored;
		};
		if (found->second.AllSystems) {
			if (!Participant || !Participant(world, DataFactoryPauseScope::AllSystems, false, detail))
				return Reply(
					world,
					DataFactoryStatus::RestoreIncomplete,
					detail.empty() ? "cannot resume all systems" : detail
				);
			resumedAllSystems = true;
			if (Worlds.SetState(world, WorldState::Active) != WorldStatus::Ok) {
				(void)rollbackAllSystems();
				return Reply(
					world, DataFactoryStatus::RestoreIncomplete, "world cannot resume at this boundary"
				);
			}
			activatedWorld = true;
		}
		if (found->second.PhysicsOnly &&
			(!Participant || !Participant(world, DataFactoryPauseScope::PhysicsOnly, false, detail))) {
			(void)rollbackAllSystems();
			return Reply(
				world, DataFactoryStatus::RestoreIncomplete, detail.empty() ? "cannot resume physics" : detail
			);
		}
		Paused.erase(found);
		Version++;
		return Reply(world, DataFactoryStatus::Ok, {});
	}

	DataFactoryReply DataFactorySession::Step(
		std::string_view instanceId,
		DataFactoryInterval interval,
		uint64_t expectedTick,
		uint64_t expectedVersion
	) {
		const WorldId world = Resolve(instanceId);
		if (!world.IsValid()) return Reply(world, DataFactoryStatus::ValidationFailed, "unknown instance_id");
		if (RenderOnlyInFlight(instanceId))
			return Reply(world, DataFactoryStatus::VersionConflict, "render-only presentation is in flight");
		if (expectedVersion != Version)
			return Reply(world, DataFactoryStatus::VersionConflict, "expected_world_version does not match");
		if (ClockOf(world).Tick != expectedTick)
			return Reply(
				world, DataFactoryStatus::VersionConflict, "expected_tick does not match the completed tick"
			);
		if (!IsCanonicalInterval(world, interval))
			return Reply(
				world,
				DataFactoryStatus::ValidationFailed,
				"dt is not this instance's canonical fixed interval"
			);
		const auto paused = Paused.find(std::string(instanceId));
		if (paused == Paused.end() || !paused->second.AllSystems)
			return Reply(world, DataFactoryStatus::NotPaused, "step requires an all_systems pause");
		if (Worlds.StepPaused(world) != WorldStatus::Ok) {
			// A scheduler fault can occur after the world committed its tick, so
			// callers must not reuse the revision they observed before the call.
			Version++;
			return Reply(
				world,
				DataFactoryStatus::RestoreIncomplete,
				"the completed step faulted or the world is not suspended"
			);
		}
		Version++;
		return Reply(world, DataFactoryStatus::Ok, "simulation uses the engine's declared float fixed delta");
	}

	DataFactoryReply DataFactorySession::Inspect(std::string_view instanceId) const {
		const WorldId world = Resolve(instanceId);
		if (!world.IsValid()) return Reply(world, DataFactoryStatus::ValidationFailed, "unknown instance_id");
		return Reply(world, DataFactoryStatus::Ok, {});
	}

	bool DataFactorySession::AllSystemsPaused(std::string_view instanceId) const {
		const auto paused = Paused.find(std::string(instanceId));
		return paused != Paused.end() && paused->second.AllSystems;
	}

	void DataFactorySession::Store(DataFactoryCheckpoint checkpoint) {
		RetainedCheckpointBytes += checkpoint.Bytes.size();
		CheckpointOrder.push_back(checkpoint.Id);
		Checkpoints.emplace(checkpoint.Id, std::move(checkpoint));
		while (CheckpointOrder.size() > Limit || RetainedCheckpointBytes > MaximumCheckpointBytes) {
			const auto found = Checkpoints.find(CheckpointOrder.front());
			RetainedCheckpointBytes -= found->second.Bytes.size();
			Checkpoints.erase(found);
			CheckpointOrder.erase(CheckpointOrder.begin());
		}
	}

	DataFactoryReply DataFactorySession::Snapshot(std::string_view instanceId, std::string &snapshotId) {
		const WorldId world = Resolve(instanceId);
		if (!world.IsValid()) return Reply(world, DataFactoryStatus::ValidationFailed, "unknown instance_id");
		if (RenderOnlyInFlight(instanceId))
			return Reply(world, DataFactoryStatus::VersionConflict, "render-only presentation is in flight");
		if (Limit == 0)
			return Reply(world, DataFactoryStatus::ResourceLimit, "checkpoint retention limit is zero");
		if (Worlds.Count() != 1)
			return Reply(
				world, DataFactoryStatus::Unsupported, "world checkpoints require a single-world universe"
			);
		core::ByteWriter writer(0, MaximumCheckpointBytes);
		try {
			if (!Worlds.Save(writer))
				return Reply(
					world,
					DataFactoryStatus::Unsupported,
					"universe state is not serializable at this boundary"
				);
		} catch (const std::length_error &) {
			return Reply(
				world, DataFactoryStatus::ResourceLimit, "checkpoint exceeds the configured byte limit"
			);
		}
		if (writer.Size() > MaximumCheckpointBytes)
			return Reply(
				world, DataFactoryStatus::ResourceLimit, "checkpoint exceeds the configured byte limit"
			);
		DataFactoryCheckpoint checkpoint;
		checkpoint.Id = "snapshot-" + std::to_string(Epoch) + "-" + std::to_string(NextCheckpoint++);
		checkpoint.Epoch = Epoch;
		checkpoint.Version = Version;
		if (const auto paused = Paused.find(std::string(instanceId)); paused != Paused.end()) {
			checkpoint.AllSystemsPaused = paused->second.AllSystems;
			checkpoint.PhysicsOnlyPaused = paused->second.PhysicsOnly;
		}
		checkpoint.Bytes.assign(writer.Bytes().begin(), writer.Bytes().end());
		snapshotId = checkpoint.Id;
		Store(std::move(checkpoint));
		return Reply(world, DataFactoryStatus::Ok, "immutable ecs and bus snapshot");
	}

	DataFactoryReply DataFactorySession::Checkpoint(std::string_view instanceId, std::string &checkpointId) {
		if (!Rehydrate)
			return Reply(
				Resolve(instanceId),
				DataFactoryStatus::Unsupported,
				"checkpoint restore needs a scheduler rehydrate callback"
			);
		DataFactoryReply reply = Snapshot(instanceId, checkpointId);
		if (reply.Status == DataFactoryStatus::Ok)
			reply.Detail = "checkpoint requires the host rehydrate callback on restore";
		return reply;
	}

	DataFactoryReply
	DataFactorySession::RenderSnapshotBarrier(std::string_view instanceId, std::string_view snapshotId) {
		const WorldId world = Resolve(instanceId);
		if (!world.IsValid()) return Reply(world, DataFactoryStatus::ValidationFailed, "unknown instance_id");
		const auto paused = Paused.find(std::string(instanceId));
		if (paused == Paused.end() || !paused->second.AllSystems)
			return Reply(world, DataFactoryStatus::NotPaused, "render capture requires an all_systems pause");
		const auto saved = Checkpoints.find(std::string(snapshotId));
		if (saved == Checkpoints.end() || saved->second.Epoch != Epoch)
			return Reply(
				world, DataFactoryStatus::StaleSnapshot, "snapshot is not in the current world epoch"
			);
		core::ByteWriter current(0, MaximumCheckpointBytes);
		try {
			if (!Worlds.Save(current))
				return Reply(
					world,
					DataFactoryStatus::Unsupported,
					"live world cannot be checked at the render barrier"
				);
		} catch (const std::length_error &) {
			return Reply(
				world, DataFactoryStatus::ResourceLimit, "render barrier exceeds the configured byte limit"
			);
		}
		const std::span<const std::byte> bytes = current.Bytes();
		if (bytes.size() != saved->second.Bytes.size() ||
			!std::equal(bytes.begin(), bytes.end(), saved->second.Bytes.begin()))
			return Reply(world, DataFactoryStatus::StaleSnapshot, "world changed after the snapshot");
		return Reply(world, DataFactoryStatus::Ok, "retained immutable snapshot matches the paused world");
	}

	void DataFactorySession::StoreRenderOnlyTerminal(DataFactoryRenderOnlyReply reply) {
		// The control ledger retains 256 idempotency keys. Keep terminal results for
		// the same window so a retained retry can always refresh its pending reply.
		constexpr size_t LIMIT = 256;
		RenderOnlyTerminalOrder.push_back(reply.OperationId);
		RenderOnlyTerminals.emplace(reply.OperationId, std::move(reply));
		while (RenderOnlyTerminalOrder.size() > LIMIT) {
			RenderOnlyTerminals.erase(RenderOnlyTerminalOrder.front());
			RenderOnlyTerminalOrder.pop_front();
		}
	}

	void DataFactorySession::FinishRenderOnly(PauseState &state, DataFactoryRenderOnlyReply reply) {
		state.RenderOnly.reset();
		StoreRenderOnlyTerminal(std::move(reply));
	}

	DataFactoryRenderOnlyReply DataFactorySession::ValidateRenderOnlySubmission(
		WorldId world, const DataFactoryRenderOnlyRequest &request
	) {
		const auto reply = [&](DataFactoryStatus status, std::string detail) {
			return RenderReply(
				world, status, request.TemporalHistory, std::move(detail), false, request.OperationId
			);
		};
		if (!world.IsValid()) return reply(DataFactoryStatus::ValidationFailed, "unknown instance_id");
		if (request.ExpectedWorldEpoch != Epoch)
			return reply(DataFactoryStatus::StaleSnapshot, "expected_world_epoch does not match");
		if (request.ExpectedWorldVersion != Version)
			return reply(DataFactoryStatus::VersionConflict, "expected_world_version does not match");
		if (ClockOf(world).Tick != request.ExpectedTick)
			return reply(
				DataFactoryStatus::VersionConflict, "expected_tick does not match the completed tick"
			);
		if (!AllSystemsPaused(request.InstanceId))
			return reply(
				DataFactoryStatus::NotPaused, "render-only presentation requires an all_systems pause"
			);
		const auto snapshot = Checkpoints.find(request.SnapshotId);
		if (snapshot == Checkpoints.end() || snapshot->second.Epoch != Epoch)
			return reply(DataFactoryStatus::StaleSnapshot, "snapshot is not in the current world epoch");
		if (snapshot->second.Version != Version)
			return reply(
				DataFactoryStatus::StaleSnapshot, "snapshot does not match the current world version"
			);
		const DataFactoryReply barrier = RenderSnapshotBarrier(request.InstanceId, request.SnapshotId);
		if (barrier.Status != DataFactoryStatus::Ok) return reply(barrier.Status, barrier.Detail);
		return reply(DataFactoryStatus::Pending, "render-only presentation is ready for submission");
	}

	DataFactoryRenderOnlyReply DataFactorySession::RenderOnly(DataFactoryRenderOnlyRequest request) {
		const WorldId world = Resolve(request.InstanceId);
		const auto reply = [&](DataFactoryStatus status, std::string detail) {
			return RenderReply(
				world, status, request.TemporalHistory, std::move(detail), false, request.OperationId
			);
		};
		if (!world.IsValid()) return reply(DataFactoryStatus::ValidationFailed, "unknown instance_id");
		if (request.TemporalHistory != DataFactoryTemporalHistory::Preserve)
			return reply(
				DataFactoryStatus::Unsupported,
				"reset and disable temporal history require renderer-local history support"
			);
		if (!Presenter)
			return reply(
				DataFactoryStatus::Unsupported, "the host has not installed a render-only presenter"
			);
		auto paused = Paused.find(request.InstanceId);
		if (paused == Paused.end() || !paused->second.AllSystems)
			return reply(
				DataFactoryStatus::NotPaused, "render-only presentation requires an all_systems pause"
			);
		if (paused->second.RenderOnly)
			return reply(DataFactoryStatus::VersionConflict, "render-only presentation is already in flight");
		const DataFactoryRenderOnlyReply ready = ValidateRenderOnlySubmission(world, request);
		if (ready.Status != DataFactoryStatus::Pending) return ready;
		if (NextRenderOnly == 0)
			return reply(DataFactoryStatus::ResourceLimit, "render-only operation id space exhausted");
		request.OperationId = NextRenderOnly++;
		paused->second.RenderOnly = request;

		std::string detail;
		bool queued = false;
		try {
			queued = Presenter(request, detail);
		} catch (const std::exception &exception) {
			detail = std::string("render-only presenter threw: ") + exception.what();
		} catch (...) {
			detail = "render-only presenter threw an unknown exception";
		}
		paused = Paused.find(request.InstanceId);
		if (paused == Paused.end() || !paused->second.RenderOnly ||
			paused->second.RenderOnly->OperationId != request.OperationId)
			return RenderReply(
				world,
				DataFactoryStatus::PresentationFailed,
				request.TemporalHistory,
				"host changed render-only lifecycle state while queueing",
				false,
				request.OperationId
			);
		if (!queued) {
			DataFactoryRenderOnlyReply failed = RenderReply(
				world,
				DataFactoryStatus::PresentationFailed,
				request.TemporalHistory,
				detail.empty() ? "host refused render-only presentation" : std::move(detail),
				false,
				request.OperationId
			);
			FinishRenderOnly(paused->second, failed);
			return failed;
		}
		return RenderReply(
			world,
			DataFactoryStatus::Pending,
			request.TemporalHistory,
			std::move(detail),
			false,
			request.OperationId
		);
	}

	DataFactoryRenderOnlyReply
	DataFactorySession::ValidateRenderOnlySubmission(std::string_view instanceId, uint64_t operationId) {
		const WorldId world = Resolve(instanceId);
		const auto paused = Paused.find(std::string(instanceId));
		if (paused == Paused.end() || !paused->second.RenderOnly ||
			paused->second.RenderOnly->OperationId != operationId)
			return RenderReply(
				world,
				DataFactoryStatus::ValidationFailed,
				DataFactoryTemporalHistory::Preserve,
				"render-only operation is not pending",
				false,
				operationId
			);
		DataFactoryRenderOnlyReply reply = ValidateRenderOnlySubmission(world, *paused->second.RenderOnly);
		if (reply.Status != DataFactoryStatus::Pending) FinishRenderOnly(paused->second, reply);
		return reply;
	}

	DataFactoryRenderOnlyReply
	DataFactorySession::CompleteRenderOnly(DataFactoryRenderOnlyCompletion completion) {
		const WorldId world = Resolve(completion.InstanceId);
		const auto paused = Paused.find(completion.InstanceId);
		if (paused == Paused.end() || !paused->second.RenderOnly ||
			paused->second.RenderOnly->OperationId != completion.OperationId)
			return RenderReply(
				world,
				DataFactoryStatus::ValidationFailed,
				DataFactoryTemporalHistory::Preserve,
				"render-only operation is not pending",
				false,
				completion.OperationId
			);
		const DataFactoryRenderOnlyRequest request = *paused->second.RenderOnly;
		if (completion.Submitted) {
			DataFactoryRenderOnlyReply valid = ValidateRenderOnlySubmission(world, request);
			if (valid.Status != DataFactoryStatus::Pending) {
				FinishRenderOnly(paused->second, valid);
				return valid;
			}
		}
		DataFactoryRenderOnlyReply reply =
			completion.Submitted
				? RenderReply(
					  world,
					  DataFactoryStatus::Ok,
					  request.TemporalHistory,
					  std::move(completion.Detail),
					  true,
					  request.OperationId
				  )
				: RenderReply(
					  world,
					  DataFactoryStatus::PresentationFailed,
					  request.TemporalHistory,
					  completion.Detail.empty() ? "renderer did not submit the render-only frame"
												: std::move(completion.Detail),
					  false,
					  request.OperationId
				  );
		FinishRenderOnly(paused->second, reply);
		return reply;
	}

	DataFactoryRenderOnlyReply
	DataFactorySession::PollRenderOnly(std::string_view instanceId, uint64_t operationId) const {
		const WorldId world = Resolve(instanceId);
		const auto paused = Paused.find(std::string(instanceId));
		if (paused != Paused.end() && paused->second.RenderOnly &&
			paused->second.RenderOnly->OperationId == operationId)
			return RenderReply(
				world,
				DataFactoryStatus::Pending,
				paused->second.RenderOnly->TemporalHistory,
				"render-only presentation is pending host submission",
				false,
				operationId
			);
		if (const auto terminal = RenderOnlyTerminals.find(operationId);
			terminal != RenderOnlyTerminals.end() && terminal->second.InstanceId == instanceId)
			return terminal->second;
		return RenderReply(
			world,
			DataFactoryStatus::ValidationFailed,
			DataFactoryTemporalHistory::Preserve,
			"unknown render-only operation",
			false,
			operationId
		);
	}

	DataFactoryReply DataFactorySession::Restore(std::string_view instanceId, std::string_view checkpointId) {
		const WorldId live = Resolve(instanceId);
		if (!live.IsValid()) return Reply(live, DataFactoryStatus::ValidationFailed, "unknown instance_id");
		if (RenderOnlyInFlight(instanceId))
			return Reply(live, DataFactoryStatus::VersionConflict, "render-only presentation is in flight");
		if (Worlds.Count() != 1)
			return Reply(
				live, DataFactoryStatus::Unsupported, "world checkpoints require a single-world universe"
			);
		if (!Rehydrate)
			return Reply(
				live,
				DataFactoryStatus::Unsupported,
				"checkpoint restore needs a scheduler rehydrate callback"
			);
		const auto found = Checkpoints.find(std::string(checkpointId));
		if (found == Checkpoints.end())
			return Reply(live, DataFactoryStatus::StaleSnapshot, "checkpoint is not retained");

		core::ByteReader reader(found->second.Bytes);
		Universe candidate;
		if (!candidate.Load(reader) || reader.Remaining() != 0)
			return Reply(
				live, DataFactoryStatus::RestoreIncomplete, "checkpoint is incompatible with this engine"
			);
		for (const WorldId world : candidate.Worlds()) {
			std::string detail;
			if (!Rehydrate(candidate, world, detail))
				return Reply(
					live, DataFactoryStatus::RestoreIncomplete, detail.empty() ? "rehydration failed" : detail
				);
		}

		const PauseState previous = [&] {
			const auto paused = Paused.find(std::string(instanceId));
			return paused == Paused.end() ? PauseState{} : paused->second;
		}();
		const PauseState restoredPause{
			.AllSystems = found->second.AllSystemsPaused,
			.PhysicsOnly = found->second.PhysicsOnlyPaused,
			.RenderOnly = {},
		};
		std::vector<std::pair<DataFactoryPauseScope, bool>> changed;
		const auto reconcile =
			[&](DataFactoryPauseScope scope, bool before, bool after, std::string &detail) {
				if (before == after) return true;
				if (!Participant || !Participant(live, scope, after, detail)) return false;
				changed.emplace_back(scope, before);
				return true;
			};
		std::string detail;
		if (!reconcile(
				DataFactoryPauseScope::AllSystems, previous.AllSystems, restoredPause.AllSystems, detail
			) ||
			!reconcile(
				DataFactoryPauseScope::PhysicsOnly, previous.PhysicsOnly, restoredPause.PhysicsOnly, detail
			)) {
			for (auto change = changed.rbegin(); change != changed.rend(); ++change) {
				std::string ignored;
				(void)Participant(live, change->first, change->second, ignored);
			}
			return Reply(
				live,
				DataFactoryStatus::RestoreIncomplete,
				detail.empty() ? "could not reconcile paused subsystems" : detail
			);
		}

		Worlds.ReplaceWith(candidate);
		Epoch++;
		Version++;
		Paused.clear();
		if (restoredPause.AllSystems || restoredPause.PhysicsOnly)
			Paused.emplace(std::string(instanceId), restoredPause);
		const WorldId restored = Resolve(instanceId);
		return Reply(restored, DataFactoryStatus::Ok, "restored through a scratch universe and fresh epoch");
	}

	DataFactoryReply DataFactorySession::CommitExternalMutation(
		std::string_view instanceId, uint64_t expectedTick, uint64_t expectedVersion
	) {
		const WorldId world = Resolve(instanceId);
		if (!world.IsValid()) return Reply(world, DataFactoryStatus::ValidationFailed, "unknown instance_id");
		if (RenderOnlyInFlight(instanceId))
			return Reply(world, DataFactoryStatus::VersionConflict, "render-only presentation is in flight");
		if (!AllSystemsPaused(instanceId))
			return Reply(
				world, DataFactoryStatus::NotPaused, "external mutation requires an all_systems pause"
			);
		if (expectedVersion != Version)
			return Reply(world, DataFactoryStatus::VersionConflict, "expected_world_version does not match");
		if (ClockOf(world).Tick != expectedTick)
			return Reply(
				world, DataFactoryStatus::VersionConflict, "expected_tick does not match the completed tick"
			);
		Version++;
		return Reply(world, DataFactoryStatus::Ok, "external atomic mutation committed at a fresh revision");
	}

	DataFactoryReply DataFactorySession::ApplyIntervention(
		std::string_view instanceId,
		std::string_view baseSnapshotId,
		std::span<const DataFactoryIntervention> changes,
		uint64_t expectedTick,
		uint64_t expectedVersion
	) {
		const WorldId world = Resolve(instanceId);
		if (!world.IsValid()) return Reply(world, DataFactoryStatus::ValidationFailed, "unknown instance_id");
		if (RenderOnlyInFlight(instanceId))
			return Reply(world, DataFactoryStatus::VersionConflict, "render-only presentation is in flight");
		if (!InterventionExecutor)
			return Reply(world, DataFactoryStatus::Unsupported, "no host intervention executor is installed");
		if (!Rehydrate)
			return Reply(
				world,
				DataFactoryStatus::Unsupported,
				"intervention rollback needs a scheduler rehydrate callback"
			);
		if (changes.empty())
			return Reply(world, DataFactoryStatus::ValidationFailed, "changed_causes is empty");
		if (expectedVersion != Version || ClockOf(world).Tick != expectedTick)
			return Reply(
				world,
				DataFactoryStatus::VersionConflict,
				"intervention precondition does not match the world"
			);
		if (RenderSnapshotBarrier(instanceId, baseSnapshotId).Status != DataFactoryStatus::Ok)
			return Reply(
				world, DataFactoryStatus::StaleSnapshot, "base_snapshot_id is not the current paused world"
			);

		core::ByteWriter writer(0, MaximumCheckpointBytes);
		try {
			if (!Worlds.Save(writer))
				return Reply(
					world,
					DataFactoryStatus::RestoreIncomplete,
					"world cannot be saved for intervention rollback"
				);
		} catch (const std::length_error &) {
			return Reply(
				world,
				DataFactoryStatus::ResourceLimit,
				"intervention rollback exceeds the configured byte limit"
			);
		}
		std::string detail;
		bool applied = false;
		try {
			applied = InterventionExecutor(Worlds, world, changes, detail);
		} catch (const std::exception &exception) {
			detail = std::string("intervention executor threw: ") + exception.what();
		} catch (...) {
			detail = "intervention executor threw an unknown exception";
		}
		if (applied) {
			Version++;
			return Reply(world, DataFactoryStatus::Ok, {});
		}

		core::ByteReader reader(writer.Bytes());
		Universe rollback;
		if (!rollback.Load(reader) || reader.Remaining() != 0)
			return Reply(
				world, DataFactoryStatus::RestoreIncomplete, "intervention rollback checkpoint is invalid"
			);
		for (const WorldId restored : rollback.Worlds()) {
			std::string rehydrate;
			if (!Rehydrate(rollback, restored, rehydrate))
				return Reply(
					world, DataFactoryStatus::RestoreIncomplete, "intervention rollback rehydration failed"
				);
		}
		Worlds.ReplaceWith(rollback);
		return Reply(
			Resolve(instanceId),
			DataFactoryStatus::ValidationFailed,
			detail.empty() ? "intervention executor refused the change" : detail
		);
	}

	bool DataFactorySession::SupportsIntervention() const {
		return static_cast<bool>(InterventionExecutor) && static_cast<bool>(Rehydrate);
	}

	bool DataFactorySession::HasCheckpoint(std::string_view checkpointId) const {
		return Checkpoints.contains(std::string(checkpointId));
	}

	const char *Describe(DataFactoryStatus status) {
		switch (status) {
		case DataFactoryStatus::Ok:
			return "ok";
		case DataFactoryStatus::Unsupported:
			return "capability_unsupported";
		case DataFactoryStatus::ValidationFailed:
			return "validation_failed";
		case DataFactoryStatus::OperationIdConflict:
			return "operation_id_conflict";
		case DataFactoryStatus::VersionConflict:
			return "version_conflict";
		case DataFactoryStatus::StaleSnapshot:
			return "stale_snapshot";
		case DataFactoryStatus::NotPaused:
			return "not_paused";
		case DataFactoryStatus::RestoreIncomplete:
			return "restore_incomplete";
		case DataFactoryStatus::ResourceLimit:
			return "resource_limit";
		case DataFactoryStatus::PresentationFailed:
			return "presentation_failed";
		case DataFactoryStatus::Pending:
			return "pending";
		}
		return "?";
	}
}
