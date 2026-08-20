#include "ConvexQuery.hpp"
#include "PipelineInternals.hpp"
#include "WorldResource.hpp"

#include <engine/core/Profiling.hpp>
#include <engine/ecs/Entity.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/physics/Broadphase.hpp>
#include <engine/physics/Clock.hpp>
#include <engine/physics/Continuous.hpp>
#include <engine/physics/PhysicsWorld.hpp>
#include <engine/physics/Shapes.hpp>
#include <engine/scene/Components.hpp>
#include <engine/spatial/HashGrid.hpp>
#include <engine/spatial/Query.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace engine::physics {

	namespace {
		// The smallest half-extent a shape actually has, in its own axes.
		//
		// `ShapeHalfExtent` reads `Collider::Extent` according to the shape kind,
		// which is the one place that table lives - so a sphere reports its
		// radius on all three axes rather than whatever its unused components
		// were left at.
		float ThinnestHalfExtent(const scene::Collider &collider) {
			const core::Vector3 half = ShapeHalfExtent(collider.Shape, collider.Extent);
			return std::min({half.X, half.Y, half.Z});
		}
	}

	void SweepFastBodies(ecs::Store &store) {
		ENGINE_PROFILE_CAT("physics.continuous", core::ProfileCategory::Physics);

		PhysicsWorld *world = PreparedWorldMutable(store);
		if (world == nullptr) {
			return;
		}

		const spatial::HashGrid &index = PipelineInternals::StaticIndex(*world);
		const std::vector<ColliderRecord> &records = PipelineInternals::StaticRecords(*world);
		if (records.empty()) {
			return;
		}

		const float delta = PhysicsStepSeconds(store);
		if (!(delta > 0.0f)) {
			return;
		}

		std::vector<uint64_t> &candidates = PipelineInternals::CandidateBuffer(*world);
		if (candidates.size() < records.size()) {
			candidates.resize(records.size());
		}

		// **The reader is the same store, taken `const` so the reads below say
		// so.** Every candidate's shape is resolved through it while an `Each`
		// is running, which the ECS allows: a read is not a structural change.
		const ecs::Store &reader = store;
		uint64_t swept = 0;

		store.Each<scene::Transform, const scene::Motion, const scene::Collider>(
			[&](ecs::Entity entity,
				scene::Transform &transform,
				const scene::Motion &motion,
				const scene::Collider &collider) {
				// **Anchored bodies are not swept.** Whatever moves one is not
				// the integrator, so the motion this step would reconstruct did
				// not happen.
				if (reader.Has<scene::Anchored>(entity)) {
					return;
				}

				// **The step the integrator just took, reconstructed rather than
				// remembered.** `scene::PreviousTransform` exists and is the
				// obvious thing to read, but nothing in this module writes it -
				// it belongs to whoever is interpolating for a renderer, and a
				// physics step that depended on a presentation component would
				// be the layer inversion `AGENTS.md` refuses. The integrator
				// added exactly `Linear * delta`, so this is that value and not
				// an approximation of it.
				const core::Vector3 travelled = motion.Linear * delta;
				const float distance = travelled.Magnitude();

				const float thinnest = ThinnestHalfExtent(collider);
				if (!(thinnest > 0.0f) || distance <= thinnest * CONTINUOUS_MOTION_RATIO) {
					return;
				}

				core::CFrame from = transform.Frame;
				from.Position = transform.Frame.Position - travelled;

				// The volume the shape passes through, which is what the index
				// is asked for. Loose - it is the union of the two end bounds
				// rather than the swept hull - and loose is the safe direction:
				// a candidate too many costs a sweep that misses.
				const core::AABB envelope =
					ShapeWorldBounds(collider, from).Union(ShapeWorldBounds(collider, transform.Frame));

				const spatial::QueryResult found =
					spatial::OverlapBox(index, envelope, collider.Mask, candidates);
				if (found.Written == 0) {
					return;
				}

				const ShapeInstance moving{from, collider.Extent, collider.Shape};

				// **The earliest hit, with the entity id breaking a tie.** The
				// grid walk's order is a function of the index rather than of
				// the scene, so two surfaces reached at the same moment have to
				// be separated by something the scene decides - otherwise a body
				// wedged into a corner is clamped against whichever wall the walk
				// happened to reach first, and that changes when anything else
				// in the world is added.
				float earliest = 1.0f;
				bool clamped = false;
				ecs::Entity against;

				// The querying body's own record, built once outside the walk -
				// the index filters on its mask and `PairAdmitted` is what
				// applies the other half of the rule, exactly as `BroadPhase`
				// does.
				const ColliderRecord self{entity, collider.Layer, collider.Mask};

				for (size_t at = 0; at < found.Written; at++) {
					const ColliderRecord &other = records[static_cast<size_t>(candidates[at])];
					if (other.Owner == entity || !PairAdmitted(self, other)) {
						continue;
					}

					const scene::Transform *placed = reader.Get<scene::Transform>(other.Owner);
					const scene::Collider *shape = reader.Get<scene::Collider>(other.Owner);
					if (placed == nullptr || shape == nullptr || shape->Trigger) {
						// **A trigger is never swept against.** It reports and
						// never pushes, so stopping a body at one would be a
						// wall made of something that is not there.
						continue;
					}

					const ShapeInstance fixed{placed->Frame, shape->Extent, shape->Shape};
					const ConvexSweep hit = SweepConvex(moving, travelled, fixed);
					if (!hit.Hit) {
						continue;
					}

					if (!clamped || hit.Fraction < earliest ||
						(hit.Fraction == earliest && other.Owner.Id < against.Id)) {
						earliest = hit.Fraction;
						against = other.Owner;
						clamped = true;
					}
				}

				if (!clamped) {
					return;
				}

				// **Just past the moment of contact, not short of it** - see
				// `CONTINUOUS_BITE`, which carries the argument. Expressed as a
				// fraction of this body's own travel, so the same millimetre is
				// the same millimetre however fast it was going, and never past
				// where the integrator had already put it.
				const float bite = CONTINUOUS_BITE / std::max(distance, 1e-6f);
				const float stop = std::clamp(earliest + bite, 0.0f, 1.0f);
				transform.Frame.Position = from.Position + travelled * stop;
				swept++;
			}
		);

		PipelineInternals::SweptBodyCount(*world) += swept;
	}
}
