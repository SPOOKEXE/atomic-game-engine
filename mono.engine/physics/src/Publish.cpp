#include "PipelineInternals.hpp"
#include "WorkBudgetInternal.hpp"
#include "WorldResource.hpp"

#include <engine/core/FrameGraph.hpp>
#include <engine/core/Log.hpp>
#include <engine/core/Metrics.hpp>
#include <engine/core/Profiling.hpp>
#include <engine/core/types/Vector3.hpp>
#include <engine/ecs/Entity.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/parallel/Jobs.hpp>
#include <engine/physics/Clock.hpp>
#include <engine/physics/Contacts.hpp>
#include <engine/physics/PhysicsWorld.hpp>
#include <engine/physics/Solver.hpp>
#include <engine/scene/Components.hpp>
#include <engine/scene/WorldMembership.hpp>

#include <cstddef>
#include <span>
#include <unordered_map>
#include <utility>
#include <vector>

namespace engine::physics {

	namespace {
		void ReportMemory(const PhysicsWorld &world) {
			const PhysicsMemoryStats stats = world.MemoryStats();
			const PhysicsMemoryBytes broadphase = stats.Broadphase();
			const PhysicsMemoryBytes total = stats.Total();
			const auto report = [](const char *liveName, const char *retainedName, PhysicsMemoryBytes bytes) {
				core::Metrics::SetGauge(liveName, static_cast<double>(bytes.LiveBytes));
				core::Metrics::SetGauge(retainedName, static_cast<double>(bytes.RetainedBytes));
			};
			report(
				"physics.memory.broadphase.live-bytes", "physics.memory.broadphase.retained-bytes", broadphase
			);
			report(
				"physics.memory.dynamic-grid.live-bytes",
				"physics.memory.dynamic-grid.retained-bytes",
				stats.DynamicGrid
			);
			report(
				"physics.memory.static-grid.live-bytes",
				"physics.memory.static-grid.retained-bytes",
				stats.StaticGrid
			);
			report(
				"physics.memory.dynamic-tree.live-bytes",
				"physics.memory.dynamic-tree.retained-bytes",
				stats.DynamicTree
			);
			report("physics.memory.solver.live-bytes", "physics.memory.solver.retained-bytes", stats.Solver);
			report(
				"physics.memory.persistent.live-bytes",
				"physics.memory.persistent.retained-bytes",
				stats.Persistent
			);
			report("physics.memory.total.live-bytes", "physics.memory.total.retained-bytes", total);
		}

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
			if (found == indexByOwner.end() || found->second >= bodies.size() ||
				bodies[found->second].Owner != entity) {
				return nullptr;
			}
			return &bodies[found->second];
		}
	}

	namespace {
		// Rows per worker for the correction write. A hash lookup per row puts
		// the crossover far below the three-add figure `Store::EachParallel`
		// quotes.
		constexpr size_t PUBLISH_TRANSFORM_GRAIN = 2048;
		// Manifolds per worker for the touch flags: two component reads each.
		constexpr size_t PUBLISH_EVENT_GRAIN = 4096;
	}

	void Publish(ecs::Store &store) {
		ChargePhysicsWork(store, PhysicsWorkStage::Solver, 0);
		ENGINE_PROFILE_CAT("physics.publish", core::ProfileCategory::Physics);

		PhysicsWorld *world = PreparedWorldMutable(store);
		if (world == nullptr) {
			return;
		}

		// Reserve publication before any transform, sleep or event mutation,
		// including retirement of contacts absent from this step's manifolds.
		ChargePhysicsWork(store, PhysicsWorkStage::Solver, PipelineInternals::TouchingLast(*world).size());
		ChargePhysicsWork(store, PhysicsWorkStage::Solver, world->Manifolds().size());
		ChargePhysicsWork(store, PhysicsWorkStage::Solver, store.Query<const scene::Motion>().Count());
		ChargePhysicsWork(store, PhysicsWorkStage::Solver, PipelineInternals::Bodies(*world).size());

		// --- overlaps --------------------------------------------------------
		//
		// The correction velocity, added to the transforms and then dropped.
		// This is the one place physics moves something outside
		// `IntegrateMotion`, and it moves positions only - see
		// `SolverBody::CorrectionLinear`, which is why there is no quaternion
		// step here to keep in agreement with the integrator's.
		//
		// **Written through the reference a query hands out, and never
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
			ENGINE_PROFILE_CAT("physics.publish-transform", core::ProfileCategory::Physics);
			if (!bodies.empty() && delta > 0.0f) {
				// Each row reads the solver's arrays and writes only its own
				// Transform, so rows run on workers.
				store.Query<scene::Transform, const scene::Motion>()
					.Without<scene::OutsideWorld>()
					.EachParallel(
						[bodies,
						 &bodyIndex,
						 delta](ecs::Entity entity, scene::Transform &transform, const scene::Motion &) {
							const SolverBody *body = BodyOf(bodies, bodyIndex, entity);
							if (body == nullptr || !body->Movable) {
								return;
							}
							transform.Frame.Position =
								transform.Frame.Position + body->CorrectionLinear * delta;
						},
						PUBLISH_TRANSFORM_GRAIN
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
			ENGINE_PROFILE_CAT("physics.publish-structural", core::ProfileCategory::Physics);
			// A parked body was not integrated, so its transform did not move.
			const PhysicsWorkBudget *window = PhysicsWorkBudgetOf(store);
			store.Query<const scene::Motion>().Without<scene::OutsideWorld>().Each(
				[&store, window](ecs::Entity entity, const scene::Motion &) {
					if (!PhysicsBodyParked(window, entity)) store.MarkChanged<scene::Transform>(entity);
				}
			);
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

			// `CanTouch` off on either side: the contact is still solved, and
			// nothing is reported for it.
			const auto touchDisabled = [&store](ecs::Entity entity) {
				const auto *collider = store.Get<scene::Collider>(entity);
				return collider != nullptr && collider->TouchDisabled;
			};
			size_t previous = 0;
			const auto retire = [&](const CandidatePair &limit, bool all) {
				while (previous < last.size() && (all || last[previous] < limit)) {
					const CandidatePair gone = last[previous];
					previous++;

					if (!touchDisabled(gone.A) && !touchDisabled(gone.B) &&
						StillRestingTogether(store, *world, gone)) {
						now.push_back(gone);
						events.push_back(ContactEvent{gone.A, gone.B, ContactPhase::Persisted});
						continue;
					}
					events.push_back(ContactEvent{gone.A, gone.B, ContactPhase::Ended});
				}
			};

			// Search, then merge. Whether each manifold's touch is off is two
			// component reads, done on workers; the ordered merge stays serial.
			const std::span<const ContactManifold> manifolds = world->Manifolds();
			static thread_local std::vector<uint8_t> ownerMuted;
			std::vector<uint8_t> &muted = ownerMuted;
			muted.resize(manifolds.size());
			parallel::Jobs::For(manifolds.size(), PUBLISH_EVENT_GRAIN, [&](size_t begin, size_t end) {
				for (size_t at = begin; at < end; at++)
					muted[at] = touchDisabled(manifolds[at].A) || touchDisabled(manifolds[at].B) ? 1 : 0;
			});
			// `events` is appended to, not rebuilt: NarrowPhase clears it, and a
			// Publish that runs without one keeps adding. An exact reserve here
			// would reallocate and copy the whole list on every call, so it
			// grows geometrically through push_back instead.
			now.reserve(manifolds.size() + last.size());

			for (size_t at = 0; at < manifolds.size(); at++) {
				const ContactManifold &manifold = manifolds[at];
				const CandidatePair pair{manifold.A, manifold.B};
				retire(pair, false);
				if (muted[at] != 0) {
					// A pair that was touching when its touch was switched
					// off ends, so no listener is left waiting on it.
					if (previous < last.size() && last[previous] == pair) {
						previous++;
						events.push_back(ContactEvent{pair.A, pair.B, ContactPhase::Ended});
					}
					continue;
				}

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

		// Publishing is the end of the full pipeline: pairs, manifolds, rows,
		// persistent contact state, and both indexes now describe one tick.
		ReportMemory(*world);
	}
}
