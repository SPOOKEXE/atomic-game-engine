#include "ContactPairs.hpp"
#include "FaceManifold.hpp"
#include "PipelineInternals.hpp"
#include "ShapeSupport.hpp"
#include "WorldResource.hpp"

#include <engine/core/FrameGraph.hpp>
#include <engine/core/Log.hpp>
#include <engine/core/Profiling.hpp>
#include <engine/ecs/Entity.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/physics/Clock.hpp>
#include <engine/physics/Contacts.hpp>
#include <engine/physics/NarrowPhase.hpp>
#include <engine/physics/PhysicsWorld.hpp>
#include <engine/scene/CollisionShapes.hpp>
#include <engine/scene/Components.hpp>

#include <cstddef>
#include <vector>

namespace engine::physics {

	namespace {
		// One collider's placed shape, or nothing when the row has gone.
		//
		// A pair is a snapshot of the world as the broad phase saw it, and a
		// row can be destroyed between the two steps - deferred structural
		// changes land at the end of an `Each`, not at the end of the phase. A
		// missing component is therefore an ordinary outcome and not a
		// diagnostic.
		struct PlacedCollider {
			ShapeInstance Shape;
			bool Trigger = false;
			bool Present = false;
		};

		PlacedCollider
		Resolve(const ecs::Store &store, const scene::CollisionShapes *shapes, ecs::Entity entity) {
			const scene::Transform *transform = store.Get<scene::Transform>(entity);
			const scene::Collider *collider = store.Get<scene::Collider>(entity);
			if (transform == nullptr || collider == nullptr) {
				return PlacedCollider{};
			}

			// **The table is looked up only for the kinds that name one**, which
			// is what keeps a world of boxes paying nothing for a feature it does
			// not use: a scan over the hull rows is cheap and a scan that never
			// happens is cheaper, and this runs once per collider per run of the
			// sorted pair list.
			//
			// **A name that resolves to nothing leaves both pointers null**, and
			// `ShapeInstance` demotes the kind to `Box` - so the part collides as
			// its own extent rather than not at all. `scene::Collider::Geometry`
			// states that as the behaviour and gives the argument for it.
			const collision::ConvexHull *hull = nullptr;
			const collision::TriangleMesh *mesh = nullptr;
			if (shapes != nullptr) {
				if (collider->Shape == scene::ShapeKind::Hull) {
					hull = shapes->FindHull(collider->Geometry);
				} else if (collider->Shape == scene::ShapeKind::Mesh) {
					mesh = shapes->FindMesh(collider->Geometry);
				}
			}

			return PlacedCollider{
				ShapeInstance{transform->Frame, collider->Extent, collider->Shape, hull, mesh},
				collider->Trigger,
				true,
			};
		}
	}

	void NarrowPhase(ecs::Store &store) {
		ENGINE_PROFILE_CAT("physics.narrowphase", core::ProfileCategory::Physics);

		PhysicsWorld *world = PreparedWorldMutable(store);
		if (world == nullptr) {
			return;
		}

		// Cleared, never freed - the allocation table names these two lists
		// beside the pair list, and this is the step that owns clearing them.
		// The event list is cleared here rather than in `Publish` so that a
		// world whose narrow phase ran and whose solver did not cannot hand a
		// reader last tick's events as though they were this tick's.
		//
		// **The manifolds belong to a step and the events belong to a tick**,
		// which only differ on a world stepping physics more than once per
		// tick. A reader asks what touched this tick, and a touch that began on
		// the second step of one is a touch that happened - clearing per step
		// would drop every contact that began and ended inside a tick, and the
		// faster the world was configured the more of them it would drop.
		std::vector<ContactManifold> &manifolds = PipelineInternals::Manifolds(*world);
		manifolds.clear();

		if (FirstPhysicsStepOfTick(store)) {
			PipelineInternals::Events(*world).clear();
		}

		// **Serial, and measured rather than assumed - twice now.** A pair
		// function is pure: it reads two placed shapes and writes one manifold,
		// so splitting the pair list across workers gives bit-for-bit the same
		// answer, and the obvious thing to do is dispatch it.
		//
		// The first attempt was twice as slow because writing a slot per
		// candidate cost a pass over several megabytes. **The second attempt
		// wrote a slot only for the pairs that touch, and lost for a different
		// reason worth writing down**: on ten thousand boxes it finished in
		// 4.07 ms of wall against 4.19 ms serial, having spent **89.5 ms of
		// worker time** doing it. Twenty-four workers each did twenty-one times
		// the work a single thread does for the same pairs.
		//
		// The cause is `Store::Get`, and it was isolated rather than guessed:
		// hoisting the two component lookups out of the dispatched body - and
		// changing nothing else - dropped worker time from 89.5 ms to 3.67 ms
		// and wall time to 349 us. Twenty-four threads chasing the entity
		// directory into columns that are not the one before them do not share
		// the work, they contend for it. `Solve`'s gather found the same thing
		// independently: 1375 us serial against 2977 us dispatched.
		//
		// **So the next attempt is not a dispatch, it is removing the
		// lookups.** The narrow phase resolves a collider once per *pair* it
		// appears in - about twenty-five thousand resolutions for ten thousand
		// colliders - where `SyncBroadphase` has already read the same
		// `Transform` and `Collider` for every one of them. A placed shape
		// stored beside each proxy, with `BroadPhase` carrying proxy indices
		// alongside the entities it already emits, removes every store lookup
		// from this step; the dispatch then scales, because the body is
		// arithmetic on its own stack. That is a change to `ColliderRecord`,
		// whose comment currently argues the other way on the grounds that this
		// step visits a fraction of the colliders - which is true for a sparse
		// world and measurably false for a dense one.
		//
		// The pair list is sorted, so every pair that names the same first
		// collider arrives in one run - a body against the six things around it
		// is one lookup rather than six. `BroadPhase` sorts for determinism and
		// this is the second thing that sort buys. The second side varies within
		// a run and is resolved each time.
		// **Resolved once for the whole step, not once per collider.** The table
		// is a world resource; asking the store for it per pair would be the
		// lookup this whole design exists to have paid once, and `Solve`'s
		// `SurfaceTable` read makes the same point one file over.
		const scene::CollisionShapes *shapes = scene::CollisionShapesOf(store);

		ecs::Entity resolved;
		PlacedCollider first;

		for (const CandidatePair &pair : world->Pairs()) {
			if (pair.A != resolved) {
				first = Resolve(store, shapes, pair.A);
				resolved = pair.A;
			}
			if (!first.Present) {
				continue;
			}

			const PlacedCollider second = Resolve(store, shapes, pair.B);
			if (!second.Present) {
				continue;
			}

			const ContactSolution solution = ContactBetween(first.Shape, second.Shape);
			if (!solution.Touching) {
				continue;
			}

			ContactManifold manifold;
			manifold.A = pair.A;
			manifold.B = pair.B;
			manifold.Normal = solution.Normal;
			manifold.PointCount = solution.PointCount;

			// Either side being a trigger makes the whole manifold one. There
			// is no half-solved contact: a trigger reports and never pushes,
			// and a pair where one side pushed and the other did not would
			// apply an impulse to one body out of two.
			manifold.Trigger = first.Trigger || second.Trigger;

			for (size_t index = 0; index < solution.PointCount; index++) {
				manifold.Points[index] = ContactPoint{
					solution.Positions[index], solution.Penetrations[index], solution.Features[index]
				};
			}
			manifolds.push_back(manifold);
		}
	}
}
