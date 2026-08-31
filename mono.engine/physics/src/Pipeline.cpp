#include "PipelineInternals.hpp"
#include "WorldResource.hpp"

#include <engine/core/Bytes.hpp>
#include <engine/ecs/Components.hpp>
#include <engine/ecs/Scheduler.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/physics/Broadphase.hpp>
#include <engine/physics/Characters.hpp>
#include <engine/physics/Clock.hpp>
#include <engine/physics/Continuous.hpp>
#include <engine/physics/Integrate.hpp>
#include <engine/physics/NarrowPhase.hpp>
#include <engine/physics/PhysicsWorld.hpp>
#include <engine/physics/Pipeline.hpp>
#include <engine/physics/Solver.hpp>
#include <engine/scene/Components.hpp>
#include <engine/scene/EditableMesh.hpp>
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

				// **Whether the size was chosen or measured, because the number
				// alone no longer says.** A measured world's cell size is
				// derived from its colliders exactly as the grids are, so
				// restoring it as though an author had named it would pin a
				// world to the scale it happened to have when it was saved -
				// and it would never measure again.
				writer.WriteBool(worlds[index].CellSizeMeasured());
			}
		}

		// The clock's rate and nothing else, for `WritePhysicsWorlds`' reason
		// one step further: the accumulator, the owed count and the counters
		// are all derived from the rate and from the ticks that have run since,
		// so writing them would be writing a second copy of a fact the tick
		// stream already carries.
		void WritePhysicsClocks(core::ByteWriter &writer, const void *source, size_t count) {
			const auto *clocks = static_cast<const PhysicsClock *>(source);
			for (size_t index = 0; index < count; index++) {
				writer.WriteDouble(clocks[index].Rate);
			}
		}

		void ReadPhysicsClocks(core::ByteReader &reader, void *destination, size_t count) {
			auto *clocks = static_cast<PhysicsClock *>(destination);
			for (size_t index = 0; index < count; index++) {
				// A fresh clock at the saved rate. A restored world owes no
				// steps until its next tick charges one, which is what stops a
				// load from stepping physics for the time the file spent on
				// disk.
				//
				// **Through the same rule the setter uses, because a snapshot
				// is hostile.** A raw `Rate` of NaN or infinity out of a
				// crafted file reaches `BeginPhysicsTick`'s cast to an
				// `int32_t`, and that is undefined behaviour rather than a
				// strange step count.
				clocks[index] = PhysicsClock{};
				clocks[index].Rate = SanePhysicsRate(reader.ReadDouble());
			}
		}

		void ReadPhysicsWorlds(core::ByteReader &reader, void *destination, size_t count) {
			auto *worlds = static_cast<PhysicsWorld *>(destination);
			for (size_t index = 0; index < count; index++) {
				const float cellSize = reader.ReadFloat();
				const bool measured = reader.ReadBool();

				// A fresh world, which leaves the static index marked stale - so
				// the first `SyncBroadphase` after a load rebuilds it rather than
				// querying a grid that describes the world the snapshot was
				// taken from.
				//
				// **A measured world is reconstructed with no size at all**, so
				// it measures again on that first sync. Passing the saved number
				// back would make it configured, which is the one reading it
				// must not have.
				worlds[index] = PhysicsWorld{measured ? 0.0f : cellSize};
			}
		}
	}

	void RegisterPhysicsComponents() {
		// The components this module iterates belong to `scene` and have to
		// carry `scene`'s explicit names. Registering them here first is
		// idempotent, and it is what keeps a caller that only prepared physics
		// from minting `engine::scene::Transform` under the compiler's spelling
		// of it - a snapshot that loads, and is wrong.
		scene::RegisterSceneComponents();

		// The name comes from `WorldResource.hpp` rather than from a literal
		// here, because the lookup that asks whether a world is prepared has to
		// spell it the same way. Two copies would disagree exactly once, and
		// the symptom would be a module convinced no world is ever prepared.
		ecs::Components::Register<PhysicsWorld>(
			PHYSICS_WORLD_COMPONENT, WritePhysicsWorlds, ReadPhysicsWorlds
		);

		// **The character pass's own resource, which used to install itself.**
		// It is declared in `Characters.cpp` and cannot be named from here, so
		// it registers itself through an entry point rather than on first touch.
		// See `RegisterCharacterComponents`.
		RegisterCharacterComponents();

		ecs::Components::Register<PhysicsClock>(
			PHYSICS_CLOCK_COMPONENT, WritePhysicsClocks, ReadPhysicsClocks
		);
	}

	void PreparePhysicsWorld(ecs::Store &store, float cellSize) {
		RegisterPhysicsComponents();

		store.SetResource(PhysicsWorld{cellSize});

		// At zero, which follows the world's tick rate. A host with a rate to
		// apply calls `SetPhysicsTickRate` after this; a host with nothing to
		// say gets what physics did before the clock existed.
		store.SetResource(PhysicsClock{});

		// Declared when the world is built, per `Store::Observe`: observing
		// later moves every row already carrying the component into an
		// archetype that has somewhere to put the bits.
		//
		// Without these two, `Store::Changed` is always false and
		// `SyncBroadphase` would decide the static geometry never moved - an
		// index describing where the world used to be, and no diagnostic.
		store.Observe<scene::Transform>();
		store.Observe<scene::Collider>();
	}

	void RegisterPhysicsSystems(ecs::Scheduler &scheduler) {
		// **A mesh a script built this tick has no collision shape until
		// something bakes one**, and this is where that happens for every host
		// that solves. `engine::render::EditableMeshUploader` hands a run-time mesh to
		// the renderer and registers nothing with `scene::CollisionShapes`, so
		// a `MeshPart` naming one fell back to colliding as its own bound - a
		// box the size of the whole thing, which a character standing on a
		// script-built heightfield is *inside*.
		//
		// **Registered here rather than beside the uploader**, because the
		// consumer is the solver: a dedicated server has no uploader at all and
		// is the machine that decides where anybody is standing. Anything that
		// calls `RegisterPhysicsSystems` gets it with nothing else edited.
		//
		// `PreSimulation`, so the shape exists before the broad phase indexes
		// the collider that names it. A mesh whose revision has not moved costs
		// one integer compare, which is what makes this affordable in a world
		// that streams a chunk a frame.
		scheduler.Add("physics.editable-mesh", ecs::Phase::PreSimulation, [](ecs::Store &store) {
			(void)scene::RefreshEditableMeshCollision(store);
		});

		// One system per phase rather than one per step. `SyncBroadphase`
		// reading what `IntegrateMotion` just wrote is a tight dependency, so
		// this pipeline remains one indivisible scheduled operation. Each step opens its own
		// profiler span, so the overlay still separates them.
		scheduler.Add("physics.simulation", ecs::Phase::Simulation, [](ecs::Store &store) {
			// **The tick is charged here and spent across both systems.** A
			// world at the default rate owes exactly one step and this reads as
			// it always did; a world with a rate of its own may owe none, in
			// which case both systems are a branch and nothing else.
			//
			// **A world with no clock at all steps once**, which is a world
			// somebody registered the systems on without preparing. Every
			// other step in the pipeline already refuses that world loudly
			// through `PreparedWorld`; making this one silently integrate
			// nothing would turn a noisy misconfiguration into a still one.
			if (PhysicsClockOf(store) != nullptr) {
				BeginPhysicsTick(store);
				if (!BeginPhysicsStep(store)) {
					return;
				}
			}

			IntegrateMotion(store);

			// **Between the two, and the order is the whole of why it works.**
			// After the positions have been stepped, so there is a motion to
			// sweep; before the index is rebuilt, so the index the sync produces
			// describes where the bodies actually ended up rather than where the
			// integrator would have put them. See `Continuous.hpp`.
			SweepFastBodies(store);

			SyncBroadphase(store);
		});

		// The four `PostSimulation` steps are one system for the same reason,
		// and here it is not a preference at all: the narrow phase reads the
		// pair list the broad phase just wrote, the solver reads the manifolds
		// the narrow phase just wrote, and `Publish` writes back what the
		// solver left in the body array. Registered separately they would run
		// in whatever order the scheduler chose, and three of the four would
		// read last tick's answer.
		//
		// **The second and later steps of a fast world run here too**, after
		// the first one's contacts. A world stepping physics twice per tick
		// integrates once in `Phase::Simulation` - so everything else in that
		// phase still sees exactly one integration, as it did before rates
		// existed - and finishes the rest of its steps whole inside this one.
		// Splitting them across the two phases instead would run a bare
		// integration with no solver behind it, which is a body passing through
		// a wall on every other step.
		scheduler.Add("physics.contacts", ecs::Phase::PostSimulation, [](ecs::Store &store) {
			const PhysicsClock *clock = PhysicsClockOf(store);
			if (clock != nullptr && !clock->Stepping) {
				// `physics.simulation` owed no step this tick, so there is
				// nothing integrated for a contact to be against. A world with
				// no clock is the unprepared one above, and it falls through
				// to the same four steps it always ran.
				return;
			}

			BroadPhase(store);
			NarrowPhase(store);
			Solve(store);
			Publish(store);

			while (BeginPhysicsStep(store)) {
				IntegrateMotion(store);
				SweepFastBodies(store);
				SyncBroadphase(store);
				BroadPhase(store);
				NarrowPhase(store);
				Solve(store);
				Publish(store);
			}
		});
	}
}
