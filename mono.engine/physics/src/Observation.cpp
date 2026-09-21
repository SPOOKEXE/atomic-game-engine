#include "ObservationInternal.hpp"
#include "WorldResource.hpp"

#include <engine/core/Bytes.hpp>
#include <engine/ecs/Components.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/physics/Clock.hpp>
#include <engine/physics/Observation.hpp>
#include <engine/physics/PhysicsWorld.hpp>
#include <engine/scene/Components.hpp>

#include <algorithm>
#include <limits>

namespace engine::physics {

	namespace {
		constexpr std::string_view PHYSICS_OBSERVATION_LOG_COMPONENT = "physics.observation-log";
		constexpr std::array<std::string_view, 3> OBSERVATION_HOOKS = {
			PHYSICS_POST_INTEGRATION_OBSERVATION,
			PHYSICS_PRE_SOLVE_OBSERVATION,
			PHYSICS_COMPLETED_SOLVER_OBSERVATION,
		};

		std::string_view HookFor(PhysicsObservationBoundary boundary) {
			switch (boundary) {
			case PhysicsObservationBoundary::PostIntegration:
				return PHYSICS_POST_INTEGRATION_OBSERVATION;
			case PhysicsObservationBoundary::PreSolve:
				return PHYSICS_PRE_SOLVE_OBSERVATION;
			case PhysicsObservationBoundary::CompletedSolver:
				return PHYSICS_COMPLETED_SOLVER_OBSERVATION;
			}
			return {};
		}

		uint32_t CountOf(size_t value) {
			return static_cast<uint32_t>(
				std::min(value, static_cast<size_t>(std::numeric_limits<uint32_t>::max()))
			);
		}

		PhysicsObservationIdentity IdentityOf(ecs::Store &store, PhysicsObservationBoundary boundary) {
			const PhysicsClock *clock = PhysicsClockOf(store);
			return PhysicsObservationIdentity{
				.Hook = HookFor(boundary),
				.World = store.Name(),
				.Tick = store.Time().Tick,
				.PhysicsStep = clock == nullptr ? 0 : clock->Steps,
				.StepInTick = clock == nullptr ? 0 : static_cast<uint32_t>(clock->StepInTick),
				.StepSeconds = PhysicsStepSeconds(store),
				.PhysicsStepAvailable = clock != nullptr,
				.StepInTickAvailable = clock != nullptr,
			};
		}

		void ResetObservationLogs(core::ByteReader &, void *destination, size_t count) {
			auto *logs = static_cast<PhysicsObservationLog *>(destination);
			for (size_t index = 0; index < count; index++) {
				logs[index] = PhysicsObservationLog{};
			}
		}

		void WriteObservationLogs(core::ByteWriter &, const void *, size_t) {}

		PhysicsObservationRecord
		RecordOf(const PhysicsObservationIdentity &identity, PhysicsObservationBoundary boundary) {
			PhysicsObservationRecord record;
			record.Boundary = boundary;
			record.Available = PhysicsObservationAvailability::World | PhysicsObservationAvailability::Tick |
							   PhysicsObservationAvailability::StepSeconds;
			if (identity.PhysicsStepAvailable) {
				record.Available = record.Available | PhysicsObservationAvailability::PhysicsStep;
			}
			if (identity.StepInTickAvailable) {
				record.Available = record.Available | PhysicsObservationAvailability::StepInTick;
			}
			record.Tick = identity.Tick;
			record.PhysicsStep = identity.PhysicsStep;
			record.StepInTick = identity.StepInTick;
			record.StepSeconds = identity.StepSeconds;
			record.TimeUnit = identity.TimeUnit;
			const size_t copied = std::min(identity.World.size(), record.World.size() - 1);
			std::copy_n(identity.World.data(), copied, record.World.data());
			if (copied != identity.World.size()) {
				record.Available = static_cast<PhysicsObservationAvailability>(
					static_cast<uint16_t>(record.Available) &
					~static_cast<uint16_t>(PhysicsObservationAvailability::World)
				);
			}
			return record;
		}
	}

	std::string_view PhysicsObservationRecord::Hook() const {
		return HookFor(Boundary);
	}

	std::span<const std::string_view> PhysicsObservationHooks() {
		return OBSERVATION_HOOKS;
	}

	std::string_view PhysicsObservationRecord::WorldName() const {
		if (!HasObservationValue(Available, PhysicsObservationAvailability::World)) {
			return {};
		}
		return World.data();
	}

	std::vector<PhysicsObservationRecord> PhysicsObservationLog::Copy() const {
		std::vector<PhysicsObservationRecord> copied;
		copied.reserve(Count);
		for (size_t index = 0; index < Count; index++) {
			copied.push_back(Records[(First + index) % CAPACITY]);
		}
		return copied;
	}

	uint64_t PhysicsObservationLog::Overwritten() const {
		return OverwrittenRecords;
	}

	void PhysicsObservationLog::Append(const PhysicsObservationRecord &record) {
		const size_t at = (First + Count) % CAPACITY;
		Records[at] = record;
		if (Count < CAPACITY) {
			Count++;
		} else {
			First = (First + 1) % CAPACITY;
			OverwrittenRecords++;
		}
	}

	std::vector<PhysicsObservationRecord> CopyPhysicsObservationRecords(const ecs::Store &store) {
		const PhysicsObservationLog *log = store.Resource<PhysicsObservationLog>();
		return log == nullptr ? std::vector<PhysicsObservationRecord>{} : log->Copy();
	}

	uint64_t OverwrittenPhysicsObservationRecords(const ecs::Store &store) {
		const PhysicsObservationLog *log = store.Resource<PhysicsObservationLog>();
		return log == nullptr ? 0 : log->Overwritten();
	}

	void RecordPostIntegration(ecs::Store &store) {
		const PostIntegrationObservationContext context{
			.Identity = IdentityOf(store, PhysicsObservationBoundary::PostIntegration),
			.MovingBodies = store.CountMatching<scene::Transform, scene::Motion>(),
		};
		PhysicsObservationRecord record =
			RecordOf(context.Identity, PhysicsObservationBoundary::PostIntegration);
		record.MovingBodies = CountOf(context.MovingBodies);
		record.Available = record.Available | PhysicsObservationAvailability::MovingBodies;
		if (PhysicsObservationLog *log = store.ResourceMutable<PhysicsObservationLog>(); log != nullptr) {
			log->Append(record);
		}
	}

	void RecordPreSolve(ecs::Store &store) {
		const PhysicsWorld *world = PreparedWorld(store);
		if (world == nullptr) {
			return;
		}
		const PreSolveObservationContext context{
			.Identity = IdentityOf(store, PhysicsObservationBoundary::PreSolve),
			.CandidatePairs = world->Pairs().size(),
			.Manifolds = world->Manifolds().size(),
		};
		PhysicsObservationRecord record = RecordOf(context.Identity, PhysicsObservationBoundary::PreSolve);
		record.CandidatePairs = CountOf(context.CandidatePairs);
		record.Manifolds = CountOf(context.Manifolds);
		record.Available = record.Available | PhysicsObservationAvailability::CandidatePairs |
						   PhysicsObservationAvailability::Manifolds;
		if (PhysicsObservationLog *log = store.ResourceMutable<PhysicsObservationLog>(); log != nullptr) {
			log->Append(record);
		}
	}

	void RecordCompletedSolver(ecs::Store &store) {
		const PhysicsWorld *world = PreparedWorld(store);
		if (world == nullptr) {
			return;
		}
		const CompletedSolverObservationContext context{
			.Identity = IdentityOf(store, PhysicsObservationBoundary::CompletedSolver),
			.SolverBodies = world->Bodies().size(),
			.SolverRows = world->RowCount(),
		};
		PhysicsObservationRecord record =
			RecordOf(context.Identity, PhysicsObservationBoundary::CompletedSolver);
		record.SolverBodies = CountOf(context.SolverBodies);
		record.SolverRows = CountOf(context.SolverRows);
		record.Available = record.Available | PhysicsObservationAvailability::SolverBodies |
						   PhysicsObservationAvailability::SolverRows;
		if (PhysicsObservationLog *log = store.ResourceMutable<PhysicsObservationLog>(); log != nullptr) {
			log->Append(record);
		}
	}

	void RegisterPhysicsObservations() {
		ecs::Components::Register<PhysicsObservationLog>(
			PHYSICS_OBSERVATION_LOG_COMPONENT, WriteObservationLogs, ResetObservationLogs
		);
	}

	void PreparePhysicsObservations(ecs::Store &store) {
		store.SetResource(PhysicsObservationLog{});
	}
}
