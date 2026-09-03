#include "ConvexQuery.hpp"
#include "PipelineInternals.hpp"
#include "WorldResource.hpp"

#include <engine/core/Profiling.hpp>
#include <engine/ecs/Entity.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/physics/Broadphase.hpp>
#include <engine/physics/Clock.hpp>
#include <engine/physics/Continuous.hpp>
#include <engine/physics/Integrate.hpp>
#include <engine/physics/PhysicsWorld.hpp>
#include <engine/physics/Shapes.hpp>
#include <engine/scene/CollisionShapes.hpp>
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

		// **The shapes the sync already resolved, by the same subscript as the
		// records.** This walk used to build its own `ShapeInstance` out of a
		// `Store::Get` pair per candidate, through the three-argument
		// constructor - which has nowhere to put a hull or a soup, so every
		// baked collider it swept against was demoted to the part's bound.
		//
		// What that cost was a character stuck in mid-air. A terrain chunk is
		// sixty-four studs of heightfield in a box the height of its tallest
		// point, so the bound's roof is the mountain top laid flat over the
		// whole chunk. A body falling fast enough to be admitted here was
		// clamped just short of that roof, ten studs above anything anybody can
		// see, and stayed: nothing was actually touching, so no contact
		// resolved it and no impulse cancelled its fall, gravity kept adding to
		// a velocity that never moved anything, and the next tick clamped it
		// against the same roof a fraction sooner.
		//
		// See `PlacedCollider`, which carries the measurement for why the
		// resolution is done once in the sync rather than per candidate here.
		const std::vector<PlacedCollider> &shapes = PipelineInternals::StaticShapes(*world);
		if (shapes.size() != records.size()) {
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

		uint64_t swept = 0;

		// The baked table, once for the walk. Every *candidate*'s shape is
		// already resolved above; this is only for the moving body, whose frame
		// has to be wound back to where the step started and so cannot be the
		// one the sync placed.
		const scene::CollisionShapes *baked = scene::CollisionShapesOf(store);

		store.Query<scene::Transform, const scene::Motion, const scene::Collider>()
			.With<scene::Simulated>()
			.Each([&](ecs::Entity entity,
					  scene::Transform &transform,
					  const scene::Motion &motion,
					  const scene::Collider &collider) {
				// **The step the integrator just took, reconstructed rather than
				// remembered.** `scene::PreviousTransform` exists and is the
				// obvious thing to read, but nothing in this module writes it -
				// it belongs to whoever is interpolating for a renderer, and a
				// physics step that depended on a presentation component would
				// be the layer inversion `AGENTS.md` refuses. Reversing the same
				// `Advanced` rule with both velocities reconstructs the integrator's
				// linear and angular start pose without presentation history.
				const core::Vector3 travelled = motion.Linear * delta;
				const float distance = travelled.Magnitude();

				const float thinnest = ThinnestHalfExtent(collider);
				if (!(thinnest > 0.0f) || distance <= thinnest * CONTINUOUS_MOTION_RATIO) {
					return;
				}

				const core::CFrame from = Advanced(transform.Frame, -motion.Linear, -motion.Angular, delta);

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

				// **The mover's own hull, and deliberately not its soup.** A
				// hull has a support function and sweeps exactly; a triangle
				// soup has none, so a moving mesh left unresolved falls back to
				// the part's bound - which is the conservative direction for
				// the body whose tunnelling this pass exists to stop.
				const collision::ConvexHull *movingHull =
					baked != nullptr && collider.Shape == scene::ShapeKind::Hull
						? baked->FindHull(collider.Geometry)
						: nullptr;

				const ShapeInstance moving{from, collider.Extent, collider.Shape, movingHull, nullptr};

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
					const auto which = static_cast<size_t>(candidates[at]);
					const ColliderRecord &other = records[which];
					if (other.Owner == entity || !PairAdmitted(self, other)) {
						continue;
					}

					const PlacedCollider &fixed = shapes[which];
					if (fixed.Trigger) {
						// **A trigger is never swept against.** It reports and
						// never pushes, so stopping a body at one would be a
						// wall made of something that is not there.
						continue;
					}

					const ConvexSweep hit = SweepConvex(moving, travelled, fixed.Shape);
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
			});

		PipelineInternals::SweptBodyCount(*world) += swept;
	}
}
