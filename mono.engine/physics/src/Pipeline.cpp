#include "PipelineInternals.hpp"
#include "WorldResource.hpp"

#include <engine/core/Bytes.hpp>
#include <engine/ecs/Components.hpp>
#include <engine/ecs/Scheduler.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/physics/Broadphase.hpp>
#include <engine/physics/Integrate.hpp>
#include <engine/physics/NarrowPhase.hpp>
#include <engine/physics/PhysicsWorld.hpp>
#include <engine/physics/Pipeline.hpp>
#include <engine/physics/Solver.hpp>
#include <engine/scene/Components.hpp>
#include <engine/scene/Registration.hpp>

#include <cstddef>

namespace engine::physics {

	namespace {
		// `PhysicsWorld` holds vectors and two grids, so the raw object
		// representation is not a serialisation and `TypeDescriptor` correctly
		// refuses to invent one. What crosses is the **cell size and nothing
		// else**: every other member is derived from `Transform` and
		// `Collider`, which the same snapshot already carries, and writing a
		// derived fact is writing a second copy of it that can disagree with
		// the first. It would also make the format depend on a grid's internal
		// layout, which `spatial` is explicit about keeping private.

		void WritePhysicsWorlds(core::ByteWriter &writer, const void *source, size_t count) {
			const auto *worlds = static_cast<const PhysicsWorld *>(source);
			for (size_t index = 0; index < count; index++) {
				writer.WriteFloat(PipelineInternals::CellSize(worlds[index]));
			}
		}

		void ReadPhysicsWorlds(core::ByteReader &reader, void *destination, size_t count) {
			auto *worlds = static_cast<PhysicsWorld *>(destination);
			for (size_t index = 0; index < count; index++) {
				// A fresh world at the restored cell size, which leaves the
				// static index marked stale — so the first `SyncBroadphase`
				// after a load rebuilds it rather than querying a grid that
				// describes the world the snapshot was taken from.
				worlds[index] = PhysicsWorld{reader.ReadFloat()};
			}
		}
	}

	void RegisterPhysicsComponents() {
		// The components this module iterates belong to `scene` and have to
		// carry `scene`'s explicit names. Registering them here first is
		// idempotent, and it is what keeps a caller that only prepared physics
		// from minting `engine::scene::Transform` under the compiler's spelling
		// of it — a snapshot that loads, and is wrong.
		scene::RegisterSceneComponents();

		// The name comes from `WorldResource.hpp` rather than from a literal
		// here, because the lookup that asks whether a world is prepared has to
		// spell it the same way. Two copies would disagree exactly once, and
		// the symptom would be a module convinced no world is ever prepared.
		ecs::Components::Register<PhysicsWorld>(
			PHYSICS_WORLD_COMPONENT, WritePhysicsWorlds, ReadPhysicsWorlds
		);
	}

	void PreparePhysicsWorld(ecs::Store &store, float cellSize) {
		RegisterPhysicsComponents();

		store.SetResource(PhysicsWorld{cellSize});

		// Declared when the world is built, per `Store::Observe`: observing
		// later moves every row already carrying the component into an
		// archetype that has somewhere to put the bits.
		//
		// Without these two, `Store::Changed` is always false and
		// `SyncBroadphase` would decide the static geometry never moved — an
		// index describing where the world used to be, and no diagnostic.
		store.Observe<scene::Transform>();
		store.Observe<scene::Collider>();
	}

	void RegisterPhysicsSystems(ecs::Scheduler &scheduler) {
		// One system per phase rather than one per step. `ecs::Scheduler` gives
		// no ordering between two systems in one phase, and `SyncBroadphase`
		// reading what `IntegrateMotion` just wrote is a hard dependency — so
		// the order is composition, which the contract supports, and not
		// registration order, which it does not. Each step opens its own
		// profiler span, so the overlay still separates them.
		scheduler.Add("physics.simulation", ecs::Phase::Simulation, [](ecs::Store &store) {
			IntegrateMotion(store);
			SyncBroadphase(store);
		});

		// The four `PostSimulation` steps are one system for the same reason,
		// and here it is not a preference at all: the narrow phase reads the
		// pair list the broad phase just wrote, the solver reads the manifolds
		// the narrow phase just wrote, and `Publish` writes back what the
		// solver left in the body array. Registered separately they would run
		// in whatever order the scheduler chose, and three of the four would
		// read last tick's answer.
		scheduler.Add("physics.contacts", ecs::Phase::PostSimulation, [](ecs::Store &store) {
			BroadPhase(store);
			NarrowPhase(store);
			Solve(store);
			Publish(store);
		});
	}
}
