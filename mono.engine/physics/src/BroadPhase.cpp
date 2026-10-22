#include "PairSort.hpp"
#include "PipelineInternals.hpp"
#include "WorkBudgetInternal.hpp"
#include "WorldResource.hpp"

#include <engine/core/FrameGraph.hpp>
#include <engine/core/Log.hpp>
#include <engine/core/Metrics.hpp>
#include <engine/core/Profiling.hpp>
#include <engine/core/types/AABB.hpp>
#include <engine/ecs/Entity.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/parallel/Jobs.hpp>
#include <engine/physics/Broadphase.hpp>
#include <engine/physics/Clock.hpp>
#include <engine/physics/NarrowPhase.hpp>
#include <engine/physics/PhysicsWorld.hpp>
#include <engine/spatial/DynamicBvh.hpp>
#include <engine/spatial/HashGrid.hpp>
#include <engine/spatial/Query.hpp>

#include <algorithm>
#include <array>
#include <atomic>
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

		bool PotentiallyClosing(
			const core::AABB &firstBounds,
			const core::AABB &secondBounds,
			const PlacedCollider &first,
			const PlacedCollider &second,
			float deltaSeconds
		) {
			if (firstBounds.Overlaps(secondBounds)) {
				return true;
			}

			const core::Vector3 relativeTravel =
				(first.LinearVelocity - second.LinearVelocity) * deltaSeconds;
			const core::AABB travelled{
				firstBounds.Minimum + relativeTravel,
				firstBounds.Maximum + relativeTravel,
			};
			const core::AABB swept = firstBounds.Union(travelled);
			const float angularReach = (first.AngularVelocity.Magnitude() * first.MaximumRadius +
										second.AngularVelocity.Magnitude() * second.MaximumRadius) *
									   deltaSeconds;
			const float marginMetres = SPECULATIVE_DISTANCE + angularReach;
			const core::Vector3 margin{marginMetres, marginMetres, marginMetres};
			return swept.Overlaps(core::AABB{secondBounds.Minimum - margin, secondBounds.Maximum + margin});
		}

		// Collects one disjoint range of dynamic proxies into caller-owned output.
		// Returns false without a partial answer when either overlap query fills
		// its scratch, so the caller can retry the same range with wider storage.
		enum class Collection : uint8_t { Complete, Overflow, Aborted };

		Collection CollectPairs(
			const PhysicsWorld &world,
			const std::vector<core::AABB> &dynamicBounds,
			const std::vector<core::AABB> &queryBounds,
			const std::vector<ColliderRecord> &dynamicRecords,
			const std::vector<ColliderRecord> &staticRecords,
			const std::vector<PlacedCollider> &dynamicShapes,
			const spatial::HashGrid &dynamicIndex,
			const spatial::DynamicBvh &dynamicTree,
			bool dynamicTreeActive,
			const spatial::HashGrid &staticIndex,
			size_t begin,
			size_t end,
			std::span<uint64_t> candidates,
			std::vector<SourcedPair> &output,
			float deltaSeconds,
			spatial::TraversalBudget *budget
		) {
			output.clear();
			for (size_t index = begin; index < end; index++) {
				const ColliderRecord &a = dynamicRecords[index];
				const core::AABB &box = queryBounds[index];

				const core::AABB &tight = dynamicBounds[index];
				const bool hasSpeculativeReach = box.Minimum != tight.Minimum || box.Maximum != tight.Maximum;
				if (!dynamicTreeActive || hasSpeculativeReach) {
					const spatial::QueryResult moving =
						dynamicTreeActive
							? (budget ? spatial::OverlapBox(dynamicTree, box, a.Mask, candidates, *budget)
									  : spatial::OverlapBox(dynamicTree, box, a.Mask, candidates))
							: (budget ? spatial::OverlapBox(dynamicIndex, box, a.Mask, candidates, *budget)
									  : spatial::OverlapBox(dynamicIndex, box, a.Mask, candidates));
					if (moving.BudgetExhausted) return Collection::Aborted;
					if (moving.Overflowed) {
						output.clear();
						return Collection::Overflow;
					}
					for (size_t at = 0; at < moving.Written; at++) {
						const auto other = static_cast<size_t>(candidates[at]);
						const bool tightOverlap = dynamicBounds[index].Overlaps(dynamicBounds[other]);
						if (other == index || (tightOverlap && other < index)) {
							continue;
						}

						const ColliderRecord &b = dynamicRecords[other];
						if (!PairAdmitted(a, b) || world.RigidlyConnected(a.Owner, b.Owner) ||
							!PotentiallyClosing(
								dynamicBounds[index],
								dynamicBounds[other],
								dynamicShapes[index],
								dynamicShapes[other],
								deltaSeconds
							)) {
							continue;
						}
						output.push_back(Ordered(
							a.Owner, b.Owner, static_cast<uint32_t>(index), static_cast<uint32_t>(other)
						));
					}
				}

				const spatial::QueryResult anchored =
					budget ? spatial::OverlapBox(staticIndex, box, a.Mask, candidates, *budget)
						   : spatial::OverlapBox(staticIndex, box, a.Mask, candidates);
				if (anchored.BudgetExhausted) return Collection::Aborted;
				if (anchored.Overflowed) {
					output.clear();
					return Collection::Overflow;
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
			return Collection::Complete;
		}
	}

	void BroadPhase(ecs::Store &store) {
		ChargePhysicsWork(store, PhysicsWorkStage::Broadphase, 0);
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

		const std::vector<core::AABB> &dynamicBounds = PipelineInternals::DynamicBounds(*world);
		const std::vector<core::AABB> &speculativeBounds = PipelineInternals::SpeculativeBounds(*world);
		const std::vector<ColliderRecord> &dynamicRecords = PipelineInternals::DynamicRecords(*world);
		const std::vector<PlacedCollider> &dynamicShapes = PipelineInternals::DynamicShapes(*world);
		const std::vector<ColliderRecord> &staticRecords = PipelineInternals::StaticRecords(*world);
		const spatial::HashGrid &dynamicIndex = PipelineInternals::DynamicIndex(*world);
		const spatial::DynamicBvh &dynamicTree = PipelineInternals::DynamicTree(*world);
		const bool dynamicTreeActive = PipelineInternals::DynamicTreeActive(*world);
		const spatial::HashGrid &staticIndex = PipelineInternals::StaticIndex(*world);
		const float deltaSeconds = PhysicsStepSeconds(store);

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
			if (dynamicTreeActive) {
				auto visitPair = [&](const spatial::Proxy &first, const spatial::Proxy &second) {
					const size_t left = static_cast<size_t>(first.Id);
					const size_t right = static_cast<size_t>(second.Id);
					const ColliderRecord &a = dynamicRecords[left];
					const ColliderRecord &b = dynamicRecords[right];
					if (PairAdmitted(a, b) && !world->RigidlyConnected(a.Owner, b.Owner)) {
						sourced.push_back(Ordered(
							a.Owner, b.Owner, static_cast<uint32_t>(left), static_cast<uint32_t>(right)
						));
					}
					return true;
				};
				if (PhysicsWorkActive(store)) {
					auto budget = PhysicsTraversalBudget(store);
					const size_t initial = budget.Remaining;
					dynamicTree.ForEachOverlappingPair(visitPair, budget);
					FinishPhysicsTraversal(store, PhysicsWorkStage::Broadphase, budget, initial);
				} else
					dynamicTree.ForEachOverlappingPair(visitPair);
			}

			// Only dynamic colliders are queried. Each batch owns its output,
			// while every query reads the same immutable indexes. The final sort
			// below keeps the result independent of which worker finished first.
			const size_t batchCount =
				(dynamicRecords.size() + BROADPHASE_BATCH_SIZE - 1) / BROADPHASE_BATCH_SIZE;
			std::vector<std::vector<SourcedPair>> &batches = PipelineInternals::SourcedPairBatches(*world);
			std::vector<uint8_t> &overflowed = PipelineInternals::SourcedPairOverflow(*world);
			batches.resize(batchCount);
			overflowed.assign(batchCount, 0);

			const bool limited = PhysicsWorkActive(store);
			const auto *workStatus = PhysicsWorkBudgetOf(store);
			const size_t available =
				limited ? static_cast<size_t>(workStatus->Limits.WorkPerTick - workStatus->Used) : 0;
			std::atomic<size_t> reservoir{available / BROADPHASE_BATCH_SIZE * BROADPHASE_BATCH_SIZE};
			const size_t leasedInitial = reservoir.load(std::memory_order_relaxed);
			const auto lease = [](void *context, size_t requested) -> size_t {
				auto &remaining = *static_cast<std::atomic<size_t> *>(context);
				size_t observed = remaining.load(std::memory_order_relaxed);
				while (observed >= requested) {
					if (remaining.compare_exchange_weak(
							observed, observed - requested, std::memory_order_relaxed
						))
						return requested;
				}
				return 0;
			};
			const auto localBudget = [&] {
				spatial::TraversalBudget budget;
				budget.Context = &reservoir;
				budget.Lease = lease;
				return budget;
			};
			parallel::Jobs::For(
				batchCount,
				1,
				[&](size_t firstBatch, size_t lastBatch) {
					std::array<uint64_t, LOCAL_CANDIDATES> localCandidates;
					for (size_t batch = firstBatch; batch < lastBatch; batch++) {
						auto budget = localBudget();
						const size_t begin = batch * BROADPHASE_BATCH_SIZE;
						const size_t end = std::min(begin + BROADPHASE_BATCH_SIZE, dynamicRecords.size());
						overflowed[batch] = static_cast<uint8_t>(CollectPairs(
							*world,
							dynamicBounds,
							speculativeBounds,
							dynamicRecords,
							staticRecords,
							dynamicShapes,
							dynamicIndex,
							dynamicTree,
							dynamicTreeActive,
							staticIndex,
							begin,
							end,
							localCandidates,
							batches[batch],
							deltaSeconds,
							limited ? &budget : nullptr
						));
					}
				},
				2
			);

			if (std::find(overflowed.begin(), overflowed.end(), static_cast<uint8_t>(Collection::Aborted)) !=
				overflowed.end()) {
				ChargePhysicsWork(
					store,
					PhysicsWorkStage::Broadphase,
					leasedInitial - reservoir.load(std::memory_order_relaxed)
				);
				FailPhysicsWork(store, PhysicsWorkStage::Broadphase);
			}

			// A dense or deliberately adversarial patch may exceed the stack
			// scratch. Retry only those batches on the caller with storage wide
			// enough for every proxy, preserving the old no-overflow contract.
			for (size_t batch = 0; batch < batchCount; batch++) {
				if (overflowed[batch] == 0) {
					continue;
				}
				const size_t begin = batch * BROADPHASE_BATCH_SIZE;
				const size_t end = std::min(begin + BROADPHASE_BATCH_SIZE, dynamicRecords.size());
				auto budget = localBudget();
				const auto collected = CollectPairs(
					*world,
					dynamicBounds,
					speculativeBounds,
					dynamicRecords,
					staticRecords,
					dynamicShapes,
					dynamicIndex,
					dynamicTree,
					dynamicTreeActive,
					staticIndex,
					begin,
					end,
					candidates,
					batches[batch],
					deltaSeconds,
					limited ? &budget : nullptr
				);
				if (collected == Collection::Aborted) {
					ChargePhysicsWork(
						store,
						PhysicsWorkStage::Broadphase,
						leasedInitial - reservoir.load(std::memory_order_relaxed)
					);
					FailPhysicsWork(store, PhysicsWorkStage::Broadphase);
				}
			}

			ChargePhysicsWork(
				store, PhysicsWorkStage::Broadphase, leasedInitial - reservoir.load(std::memory_order_relaxed)
			);
			size_t pairCount = sourced.size();
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
			std::vector<SourcedPair> &sortScratch = PipelineInternals::SourcedPairSortScratch(*world);
			{
				ENGINE_PROFILE_CAT("physics.pair-sort-b", core::ProfileCategory::Physics);
				RadixSortPairMember(sourced, sortScratch, [](const SourcedPair &pair) {
					return pair.Pair.B.Id;
				});
			}
			{
				ENGINE_PROFILE_CAT("physics.pair-sort-a", core::ProfileCategory::Physics);
				RadixSortPairMember(sourced, sortScratch, [](const SourcedPair &pair) {
					return pair.Pair.A.Id;
				});
			}

			// Each pair once. The generation above already reports each one once;
			// this is what makes "once" a property of the list the solver reads
			// rather than a property of how it was filled, and applying one contact
			// twice doubles its impulse.
			{
				ENGINE_PROFILE_CAT("physics.pair-deduplicate", core::ProfileCategory::Physics);
				sourced.erase(std::unique(sourced.begin(), sourced.end()), sourced.end());
			}
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
			core::Metrics::SetGauge("physics.broadphase.pairs", static_cast<double>(pairs.size()));
		}
	}
}
