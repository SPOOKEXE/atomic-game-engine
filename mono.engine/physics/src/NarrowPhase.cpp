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
#include <engine/parallel/Jobs.hpp>
#include <engine/physics/Clock.hpp>
#include <engine/physics/Contacts.hpp>
#include <engine/physics/NarrowPhase.hpp>
#include <engine/physics/PhysicsWorld.hpp>
#include <engine/scene/Components.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace engine::physics {

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

		// **Dispatched since v0.17, and the note it replaces said the opposite
		// twice.** A pair function is pure - it reads two placed shapes and
		// writes one manifold - so splitting the pair list across workers gives
		// bit-for-bit the same answer, and the obvious thing to do is dispatch
		// it. Two attempts did, and both lost:
		//
		// - the first wrote a slot per *candidate*, which cost a pass over
		//   several megabytes;
		// - the second wrote a slot only for the pairs that touch, and finished
		//   in 4.07 ms of wall against 4.19 ms serial - having spent **89.5 ms
		//   of worker time** doing it. Twenty-four workers each did twenty-one
		//   times the per-pair work one thread does.
		//
		// The cause was isolated rather than guessed: hoisting the two
		// `Store::Get` calls out of the dispatched body, and changing nothing
		// else, dropped worker time to 3.67 ms and wall time to 349 us.
		// `physics/AGENTS.md` carries the whole finding.
		//
		// **So this step no longer looks anything up.** `SyncBroadphase` reads
		// every collider's `Transform` and `Collider` to build the proxies, and
		// now keeps the placed shape beside each one; `BroadPhase` carries the
		// proxy indices alongside the entities it emits. A pair is two array
		// subscripts.
		const std::span<const CandidatePair> pairs = world->Pairs();
		const std::span<const CandidateSource> sources = PipelineInternals::PairSources(*world);
		if (pairs.empty() || sources.size() != pairs.size()) {
			return;
		}

		const std::vector<PlacedCollider> &dynamicShapes = PipelineInternals::DynamicShapes(*world);
		const std::vector<PlacedCollider> &staticShapes = PipelineInternals::StaticShapes(*world);

		std::vector<std::vector<ContactManifold>> &batches = PipelineInternals::ManifoldBatches(*world);
		const size_t batchCount = (pairs.size() + NARROW_GRAIN - 1) / NARROW_GRAIN;
		if (batches.size() < batchCount) {
			batches.resize(batchCount);
		}

		{
			ENGINE_PROFILE_CAT("physics.contact-measure", core::ProfileCategory::Physics);
			parallel::Jobs::For(
				pairs.size(),
				NARROW_GRAIN,
				[pairs, sources, &dynamicShapes, &staticShapes, &batches](size_t begin, size_t end) {
					std::vector<ContactManifold> &output = batches[begin / NARROW_GRAIN];
					output.clear();
					output.reserve(end - begin);
					for (size_t at = begin; at < end; at++) {
						const CandidateSource &source = sources[at];

						// **Bounds-checked, because the two index arrays are built
						// one step apart.** A row destroyed between the sync and the
						// broad phase leaves an index into an array that has since
						// been rebuilt shorter - deferred structural changes land at
						// the end of an `Each`, not at the end of the phase, so this
						// is an ordinary outcome and not a diagnostic.
						const std::vector<PlacedCollider> &firstTable =
							source.FirstIsStatic() ? staticShapes : dynamicShapes;
						const std::vector<PlacedCollider> &secondTable =
							source.SecondIsStatic() ? staticShapes : dynamicShapes;

						const size_t firstAt = source.FirstIndex();
						const size_t secondAt = source.SecondIndex();
						if (firstAt >= firstTable.size() || secondAt >= secondTable.size()) {
							continue;
						}

						const PlacedCollider &first = firstTable[firstAt];
						const PlacedCollider &second = secondTable[secondAt];

						const ContactSolution solution = ContactBetween(first.Shape, second.Shape);
						if (!solution.Touching) {
							continue;
						}

						ContactManifold manifold;
						manifold.A = pairs[at].A;
						manifold.B = pairs[at].B;
						manifold.Normal = solution.Normal;
						manifold.PointCount = solution.PointCount;

						// Either side being a trigger makes the whole manifold one.
						// There is no half-solved contact: a trigger reports and
						// never pushes, and a pair where one side pushed and the
						// other did not would apply an impulse to one body out of
						// two.
						manifold.Trigger = first.Trigger || second.Trigger;

						for (size_t index = 0; index < solution.PointCount; index++) {
							manifold.Points[index] = ContactPoint{
								solution.Positions[index],
								solution.Penetrations[index],
								solution.Features[index],
							};
						}

						output.push_back(manifold);
					}
				},
				NARROW_GRAIN
			);
		}

		// **In pair-range order, on one thread.** A worker appending to one
		// manifold list would make solver order depend on when workers finished.
		// Each batch covers one fixed input range, so concatenation carries the
		// pair order through while skipping candidates that did not touch.
		{
			ENGINE_PROFILE_CAT("physics.contact-compact", core::ProfileCategory::Physics);
			manifolds.reserve(pairs.size());
			for (size_t batch = 0; batch < batchCount; batch++) {
				const std::vector<ContactManifold> &output = batches[batch];
				manifolds.insert(manifolds.end(), output.begin(), output.end());
			}
		}
	}
}
