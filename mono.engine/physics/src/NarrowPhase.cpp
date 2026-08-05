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

		for (const CandidatePair &pair : world->Pairs()) {
			const PlacedCollider first = Resolve(store, pair.A);
			const PlacedCollider second = Resolve(store, pair.B);
			if (!first.Present || !second.Present) {
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
