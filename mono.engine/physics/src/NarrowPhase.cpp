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
#include <engine/physics/Contacts.hpp>
#include <engine/physics/NarrowPhase.hpp>
#include <engine/physics/PhysicsWorld.hpp>
#include <engine/scene/Components.hpp>

#include <cstddef>
#include <vector>

namespace engine::physics {

	namespace {
		// One collider's placed shape, or nothing when the row has gone.
		//
		// A pair is a snapshot of the world as the broad phase saw it, and a
		// row can be destroyed between the two steps — deferred structural
		// changes land at the end of an `Each`, not at the end of the phase. A
		// missing component is therefore an ordinary outcome and not a
		// diagnostic.
		struct PlacedCollider {
			ShapeInstance Shape;
			bool Trigger = false;
			bool Present = false;
		};

		PlacedCollider Resolve(const ecs::Store &store, ecs::Entity entity) {
			const scene::Transform *transform = store.Get<scene::Transform>(entity);
			const scene::Collider *collider = store.Get<scene::Collider>(entity);
			if (transform == nullptr || collider == nullptr) {
				return PlacedCollider{};
			}
			return PlacedCollider{
				ShapeInstance{transform->Frame, collider->Extent, collider->Shape}, collider->Trigger, true
			};
		}
	}

	void NarrowPhase(ecs::Store &store) {
		ENGINE_PROFILE_CAT("physics.narrowphase", core::ProfileCategory::Physics);

		PhysicsWorld *world = PreparedWorldMutable(store);
		if (world == nullptr) {
			return;
		}

		// Cleared, never freed — the allocation table names these two lists
		// beside the pair list, and this is the step that owns clearing them.
		// The event list is cleared here rather than in `Publish` so that a
		// world whose narrow phase ran and whose solver did not cannot hand a
		// reader last tick's events as though they were this tick's.
		std::vector<ContactManifold> &manifolds = PipelineInternals::Manifolds(*world);
		manifolds.clear();
		PipelineInternals::Events(*world).clear();

		// **Serial, and measured rather than assumed.** A pair function is pure
		// — it reads two placed shapes and writes one manifold — so splitting
		// the pair list across workers gives bit-for-bit the same answer, and
		// the obvious thing to do is dispatch it. Measured on a twenty-four
		// thread machine it was **twice as slow**: writing a slot per candidate
		// so workers never share a cursor costs a pass over several megabytes,
		// and the dispatch itself loses badly whenever the cores are already
		// busy with something else. Worth retrying on an idle machine against
		// `benchmarks/Solver.cpp`'s phase rows; not worth carrying on this
		// evidence.
		//
		// The pair list is sorted, so every pair that names the same first
		// collider arrives in one run — a body against the six things around it
		// is one lookup rather than six. `BroadPhase` sorts for determinism and
		// this is the second thing that sort buys. The second side varies within
		// a run and is resolved each time.
		ecs::Entity resolved;
		PlacedCollider first;

		for (const CandidatePair &pair : world->Pairs()) {
			if (pair.A != resolved) {
				first = Resolve(store, pair.A);
				resolved = pair.A;
			}
			if (!first.Present) {
				continue;
			}

			const PlacedCollider second = Resolve(store, pair.B);
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
