#include "PipelineInternals.hpp"
#include "WorldResource.hpp"

#include <engine/core/FrameGraph.hpp>
#include <engine/core/Log.hpp>
#include <engine/core/Profiling.hpp>
#include <engine/core/types/Vector3.hpp>
#include <engine/ecs/Entity.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/physics/Clock.hpp>
#include <engine/physics/Contacts.hpp>
#include <engine/physics/PhysicsWorld.hpp>
#include <engine/physics/Solver.hpp>
#include <engine/scene/Components.hpp>

#include <cstddef>
#include <span>
#include <unordered_map>
#include <utility>
#include <vector>

namespace engine::physics {

	namespace {
		// Whether a pair that has left the manifold list is still really
		// touching.
		//
		// **A body that falls asleep leaves the dynamic index**, and two
		// anchored colliders are never a pair - so the tick a resting box
		// sleeps, the contact holding it up disappears from the broad phase.
		// Reporting that as `Ended` would tell a listener the box left the
		// floor, which is the one thing it definitely did not do.
		bool
		StillRestingTogether(const ecs::Store &store, const PhysicsWorld &world, const CandidatePair &pair) {
			if (!store.Alive(pair.A) || !store.Alive(pair.B)) {
				return false;
			}
			return world.Sleeping(pair.A) || world.Sleeping(pair.B);
		}

		// The body at `entity`, or nothing.
		//
		// The index is lookup-only. Contact and solver order remains the sorted
		// body array, while publication avoids searching it for every dynamic row.
		const SolverBody *BodyOf(
			std::span<const SolverBody> bodies,
			const std::unordered_map<uint64_t, size_t> &indexByOwner,
			ecs::Entity entity
		) {
			const auto found = indexByOwner.find(entity.Id);
			if (found == indexByOwner.end() || found->second >= bodies.size() || bodies[found->second].Owner != entity) {
				return nullptr;
			}
			return &bodies[found->second];
		}
	}

	void Publish(ecs::Store &store) {
		ENGINE_PROFILE_CAT("physics.publish", core::ProfileCategory::Physics);

		PhysicsWorld *world = PreparedWorldMutable(store);
		if (world == nullptr) {
			return;
		}

		// --- overlaps --------------------------------------------------------
		//
		// The correction velocity, added to the transforms and then dropped.
		// This is the one place physics moves something outside
		// `IntegrateMotion`, and it moves positions only - see
		// `SolverBody::CorrectionLinear`, which is why there is no quaternion
		// step here to keep in agreement with the integrator's.
		//
		// **Written through the reference an `Each` hands out, and never
		// through `Store::Set`.** A write through `Set` stamps the row, and
		// `SyncBroadphase` reads those stamps to decide whether *static*
		// geometry moved - a stamp left on a body that later falls asleep, and
		// therefore has no `Motion` to exclude it, rebuilds the static index
		// every tick forever. `Store::Each` documents a write through the
		// reference as a direct memory write, which is exactly what is wanted.
		const float delta = PhysicsStepSeconds(store);
		const std::span<const SolverBody> bodies = world->Bodies();
		const std::unordered_map<uint64_t, size_t> &bodyIndex = PipelineInternals::BodyIndexByOwner(*world);
		{
			ENGINE_PROFILE_CAT("physics.publish-correction", core::ProfileCategory::Physics);
			if (!bodies.empty() && delta > 0.0f) {
				store.Each<scene::Transform, const scene::Motion>(
					[bodies, &bodyIndex, delta](ecs::Entity entity, scene::Transform &transform, const scene::Motion &) {
						const SolverBody *body = BodyOf(bodies, bodyIndex, entity);
						if (body == nullptr || !body->Movable) {
							return;
						}
						transform.Frame.Position = transform.Frame.Position + body->CorrectionLinear * delta;
					}
				);
			}
		}

		// --- velocities ------------------------------------------------------
		//
		// The only writes the pipeline makes to a component after
		// `SyncBroadphase`, and they are all here rather than spread through
		// the solver's iteration. Eight sweeps writing through `Store::Set`
		// would be eight sparse-set lookups per body per contact for a value
		// only the last one is true.
		{
			ENGINE_PROFILE_CAT("physics.publish-velocity", core::ProfileCategory::Physics);
			for (const SolverBody &body : bodies) {
				if (body.Asleep) {
					// **The archetype move.** Losing `scene::Motion` takes the row
					// out of `IntegrateMotion`'s query and out of the dynamic half
					// of the broad phase - the query never visits it, which a tag
					// could not deliver without a "without this component" query
					// term the ECS does not have.
					if (store.Has<scene::Motion>(body.Owner)) {
						store.Remove<scene::Motion>(body.Owner);
					}
					continue;
				}

				// A kinematic body and a piece of static geometry are both in the
				// body array - they take part in every contact - and neither has a
				// velocity the solver is allowed to have changed. Writing one back
				// would overwrite whatever moves the platform.
				if (!body.Movable) {
					continue;
				}

				// `Set` rather than `GetMutable`, because a body that has just
				// woken has no `scene::Motion` at all and this is the write that
				// gives it one back.
				store.Set<scene::Motion>(
					body.Owner, scene::Motion{body.LinearVelocity, body.AngularVelocity}
				);
			}
		}

		// --- what moved ------------------------------------------------------
		//
		// **The transform, which nothing was claiming.** `IntegrateMotion` moves
		// it through `EachParallel` and says in as many words that it will not
		// mark it - a parallel body may not write a shared bitset, and
		// `MarkAllChanged` would cover the anchored rows and rebuild the static
		// index every tick for ever. It leaves the claim to "a consumer that
		// needs a replication delta out of an integrated world", and for three
		// versions there was no such consumer, so a physics-driven body was
		// invisible to `replication`: a character walked on the server and stood
		// still on every client.
		//
		// **Every row with a `Motion`, and not the solver's body array**, which
		// is the correction. `PhysicsWorld::Bodies` holds only the bodies a
		// manifold names - a body nothing touches has no constraint to solve -
		// so marking from that loop marks exactly the bodies that are *resting
		// on something* and misses every body in the air. A character standing
		// on a floor replicated; the moment it jumped it was in contact with
		// nothing, its transform stopped being marked, and every client watched
		// it hang motionless at the height it left the ground while the server
		// sailed it through a full arc. Falling scenery had the same bug and
		// nothing was watching for it.
		//
		// The right set is the one `IntegrateMotion` moved, which is every row
		// carrying a `Motion` - awake, dynamic, and therefore integrated. A
		// sleeping body has had its `Motion` taken away by the loop above, so it
		// is excluded by construction rather than by a second test, and
		// `SyncBroadphase`'s inner gate - "was a changed row one without a
		// `Motion`" - still answers no for every row this touches.
		{
			ENGINE_PROFILE_CAT("physics.publish-changes", core::ProfileCategory::Physics);
			store.Each<const scene::Motion>([&store](ecs::Entity entity, const scene::Motion &) {
				store.MarkChanged<scene::Transform>(entity);
			});
		}

		// --- events ----------------------------------------------------------
		//
		// A merge of this tick's manifolds against last tick's touching set,
		// both sorted by `(min id, max id)`. A merge rather than a lookup per
		// pair because the two lists are already in the same order, and because
		// a set would be an unordered container in the middle of the one path
		// §2.4 says must not hold one.
		{
			ENGINE_PROFILE_CAT("physics.publish-events", core::ProfileCategory::Physics);
			std::vector<ContactEvent> &events = PipelineInternals::Events(*world);
			std::vector<CandidatePair> &last = PipelineInternals::TouchingLast(*world);
			std::vector<CandidatePair> &now = PipelineInternals::TouchingNow(*world);
			now.clear();

			size_t previous = 0;
			const auto retire = [&](const CandidatePair &limit, bool all) {
				while (previous < last.size() && (all || last[previous] < limit)) {
					const CandidatePair gone = last[previous];
					previous++;

					if (StillRestingTogether(store, *world, gone)) {
						now.push_back(gone);
						events.push_back(ContactEvent{gone.A, gone.B, ContactPhase::Persisted});
						continue;
					}
					events.push_back(ContactEvent{gone.A, gone.B, ContactPhase::Ended});
				}
			};

			for (const ContactManifold &manifold : world->Manifolds()) {
				const CandidatePair pair{manifold.A, manifold.B};
				retire(pair, false);

				ContactPhase phase = ContactPhase::Began;
				if (previous < last.size() && last[previous] == pair) {
					phase = ContactPhase::Persisted;
					previous++;
				}
				events.push_back(ContactEvent{pair.A, pair.B, phase});
				now.push_back(pair);
			}
			retire(CandidatePair{}, true);

			std::swap(last, now);
		}
	}
}
