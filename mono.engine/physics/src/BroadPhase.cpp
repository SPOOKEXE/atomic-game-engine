#include "PipelineInternals.hpp"
#include "WorldResource.hpp"

#include <engine/core/FrameGraph.hpp>
#include <engine/core/Log.hpp>
#include <engine/core/Profiling.hpp>
#include <engine/core/types/AABB.hpp>
#include <engine/ecs/Entity.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/parallel/Jobs.hpp>
#include <engine/physics/Broadphase.hpp>
#include <engine/physics/PhysicsWorld.hpp>
#include <engine/spatial/HashGrid.hpp>
#include <engine/spatial/Query.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace engine::physics {

	namespace {
		// Enough colliders to make one handover worthwhile without leaving a
		// dense patch on one worker. The StressPhysics release profile measures
		// the pair walk at roughly 340 microseconds per thousand moving bodies.
		constexpr size_t BROADPHASE_BATCH_SIZE = 1024;

		// The common query stays on the stack. A batch that finds a denser cell is
		// replayed after the join with the world's full retained candidate buffer,
		// so this bound changes memory traffic and never changes the pair set.
		constexpr size_t LOCAL_CANDIDATES = 256;

		// The pair, with the smaller entity id first, and its indices swapped to
		// match.
		//
		// **The indices swap with the entities, and forgetting that is the one
		// silent way to get this wrong**: the narrow phase would then test the
		// right two shapes in the wrong order, and every normal from an
		// asymmetric pair would point the wrong way.
		SourcedPair Ordered(ecs::Entity left, ecs::Entity right, uint32_t leftAt, uint32_t rightAt) {
			if (left.Id < right.Id) {
				return SourcedPair{CandidatePair{left, right}, CandidateSource{leftAt, rightAt}};
			}
			return SourcedPair{CandidatePair{right, left}, CandidateSource{rightAt, leftAt}};
		}

		// Collects one disjoint range of dynamic proxies into caller-owned output.
		// Returns false without a partial answer when either overlap query fills
		// its scratch, so the caller can retry the same range with wider storage.
		bool CollectPairs(
			const PhysicsWorld &world,
			const std::vector<spatial::Proxy> &dynamicProxies,
			const std::vector<ColliderRecord> &dynamicRecords,
			const std::vector<ColliderRecord> &staticRecords,
			const spatial::HashGrid &dynamicIndex,
			const spatial::HashGrid &staticIndex,
			size_t begin,
			size_t end,
			std::span<uint64_t> candidates,
			std::vector<SourcedPair> &output
		) {
			output.clear();
			for (size_t index = begin; index < end; index++) {
				const ColliderRecord &a = dynamicRecords[index];
				const core::AABB &box = dynamicProxies[index].Bounds;

				const spatial::QueryResult moving = spatial::OverlapBox(dynamicIndex, box, a.Mask, candidates);
				if (moving.Overflowed) {
					output.clear();
					return false;
				}
				for (size_t at = 0; at < moving.Written; at++) {
					const auto other = static_cast<size_t>(candidates[at]);
					if (other <= index) {
						continue;
					}

					const ColliderRecord &b = dynamicRecords[other];
					if (!PairAdmitted(a, b) || world.RigidlyConnected(a.Owner, b.Owner)) {
						continue;
					}
					output.push_back(
						Ordered(a.Owner, b.Owner, static_cast<uint32_t>(index), static_cast<uint32_t>(other))
					);
				}

				const spatial::QueryResult anchored = spatial::OverlapBox(staticIndex, box, a.Mask, candidates);
				if (anchored.Overflowed) {
					output.clear();
					return false;
				}
				for (size_t at = 0; at < anchored.Written; at++) {
					const auto other = static_cast<size_t>(candidates[at]);
					const ColliderRecord &b = staticRecords[other];
					if (b.Owner == a.Owner) {
						continue;
					}
					if (!PairAdmitted(a, b) || world.RigidlyConnected(a.Owner, b.Owner)) {
						continue;
					}
					output.push_back(Ordered(
						a.Owner,
						b.Owner,
						static_cast<uint32_t>(index),
						static_cast<uint32_t>(other) | CandidateSource::STATIC
					));
				}
			}
			return true;
		}
	}

	void BroadPhase(ecs::Store &store) {
		ENGINE_PROFILE_CAT("physics.broadphase", core::ProfileCategory::Physics);

		PhysicsWorld *world = PreparedWorldMutable(store);
		if (world == nullptr) {
			return;
		}

		// Cleared, never freed. A steady scene stops allocating a pair list
		// after its first tick.
		std::vector<CandidatePair> &pairs = PipelineInternals::Pairs(*world);
		std::vector<CandidateSource> &sources = PipelineInternals::PairSources(*world);
		std::vector<SourcedPair> &sourced = PipelineInternals::SourcedPairs(*world);
		pairs.clear();
		sources.clear();
		sourced.clear();

		const std::vector<spatial::Proxy> &dynamicProxies = PipelineInternals::DynamicProxies(*world);
		const std::vector<ColliderRecord> &dynamicRecords = PipelineInternals::DynamicRecords(*world);
		const std::vector<ColliderRecord> &staticRecords = PipelineInternals::StaticRecords(*world);
		const spatial::HashGrid &dynamicIndex = PipelineInternals::DynamicIndex(*world);
		const spatial::HashGrid &staticIndex = PipelineInternals::StaticIndex(*world);

		// Sized to the widest possible answer, which is every proxy in one
		// index, so no query can overflow and no query has to be retried. One
		// resize on the tick a world grows and none after it.
		std::vector<uint64_t> &candidates = PipelineInternals::CandidateBuffer(*world);
		const size_t widest = std::max(dynamicRecords.size(), staticRecords.size());
		if (candidates.size() < widest) {
			candidates.resize(widest);
		}

		// The queries themselves, separate from the sync that built the
		// index and from the narrow phase that consumes the pairs. This is
		// the part that scales with how *clustered* a scene is rather than
		// with how large it is, and the two want different answers.
		{
			ENGINE_PROFILE_CAT("physics.query", core::ProfileCategory::Physics);

			// Only dynamic colliders are queried. Each batch owns its output,
			// while every query reads the same immutable indexes. The final sort
			// below keeps the result independent of which worker finished first.
			const size_t batchCount =
				(dynamicRecords.size() + BROADPHASE_BATCH_SIZE - 1) / BROADPHASE_BATCH_SIZE;
			std::vector<std::vector<SourcedPair>> &batches = PipelineInternals::SourcedPairBatches(*world);
			std::vector<uint8_t> &overflowed = PipelineInternals::SourcedPairOverflow(*world);
			batches.resize(batchCount);
			overflowed.assign(batchCount, 0);

			parallel::Jobs::For(
				batchCount,
				1,
				[&](size_t firstBatch, size_t lastBatch) {
					std::array<uint64_t, LOCAL_CANDIDATES> localCandidates;
					for (size_t batch = firstBatch; batch < lastBatch; batch++) {
						const size_t begin = batch * BROADPHASE_BATCH_SIZE;
						const size_t end = std::min(begin + BROADPHASE_BATCH_SIZE, dynamicRecords.size());
						overflowed[batch] = CollectPairs(
							*world,
							dynamicProxies,
							dynamicRecords,
							staticRecords,
							dynamicIndex,
							staticIndex,
							begin,
							end,
							localCandidates,
							batches[batch]
						)
							? 0
							: 1;
					}
				},
				2
			);

			// A dense or deliberately adversarial patch may exceed the stack
			// scratch. Retry only those batches on the caller with storage wide
			// enough for every proxy, preserving the old no-overflow contract.
			for (size_t batch = 0; batch < batchCount; batch++) {
				if (overflowed[batch] == 0) {
					continue;
				}
				const size_t begin = batch * BROADPHASE_BATCH_SIZE;
				const size_t end = std::min(begin + BROADPHASE_BATCH_SIZE, dynamicRecords.size());
				CollectPairs(
					*world,
					dynamicProxies,
					dynamicRecords,
					staticRecords,
					dynamicIndex,
					staticIndex,
					begin,
					end,
					candidates,
					batches[batch]
				);
			}

			size_t pairCount = 0;
			for (const std::vector<SourcedPair> &batch : batches) {
				pairCount += batch.size();
			}
			sourced.reserve(pairCount);
			for (const std::vector<SourcedPair> &batch : batches) {
				sourced.insert(sourced.end(), batch.begin(), batch.end());
			}
		}

		{
			ENGINE_PROFILE_CAT("physics.pair-order", core::ProfileCategory::Physics);

			// **The sort is the determinism requirement.** Sequential impulse is
			// order-dependent, so a solver visiting contacts in grid-walk order
			// gives one answer on a scene built one way and another on the same
			// scene built another way - and `just determinism` reports it a long
			// way from here.
			std::sort(sourced.begin(), sourced.end());

			// Each pair once. The generation above already reports each one once;
			// this is what makes "once" a property of the list the solver reads
			// rather than a property of how it was filled, and applying one contact
			// twice doubles its impulse.
			sourced.erase(std::unique(sourced.begin(), sourced.end()), sourced.end());
		}

		// **Split into the public list and the private one, after the sort.**
		// The pair list is what a manifold, a contact event and every consumer
		// outside this module reads, and it names entities; the indices beside it
		// are into `PhysicsWorld`'s own arrays and are nobody else's business -
		// `CandidateSource` says why publishing one would hand a caller a number
		// that is plausible and wrong.
		{
			ENGINE_PROFILE_CAT("physics.pair-publish", core::ProfileCategory::Physics);
			pairs.reserve(sourced.size());
			sources.reserve(sourced.size());
			for (const SourcedPair &row : sourced) {
				pairs.push_back(row.Pair);
				sources.push_back(row.Source);
			}
		}
	}
}
