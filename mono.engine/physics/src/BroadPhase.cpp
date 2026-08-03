#include "PipelineInternals.hpp"
#include "WorldResource.hpp"

#include <engine/core/FrameGraph.hpp>
#include <engine/core/Log.hpp>
#include <engine/core/Profiling.hpp>
#include <engine/core/types/AABB.hpp>
#include <engine/ecs/Entity.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/physics/Broadphase.hpp>
#include <engine/physics/PhysicsWorld.hpp>
#include <engine/spatial/HashGrid.hpp>
#include <engine/spatial/Query.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace engine::physics {

	namespace {
		// The pair, with the smaller entity id first.
		CandidatePair Ordered(ecs::Entity left, ecs::Entity right) {
			if (left.Id < right.Id) {
				return CandidatePair{left, right};
			}
			return CandidatePair{right, left};
		}
	}

	void BroadPhase(ecs::Store &store) {
		ENGINE_PROFILE_CAT("physics.broadphase", core::ProfileCategory::ECS);

		PhysicsWorld *world = PreparedWorldMutable(store);
		if (world == nullptr) {
			return;
		}

		// Cleared, never freed. A steady scene stops allocating a pair list
		// after its first tick.
		std::vector<CandidatePair> &pairs = PipelineInternals::Pairs(*world);
		pairs.clear();

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

		// Only dynamic colliders are queried. Two anchored parts overlapping is
		// the level author's business, and a pair the solver could not move
		// either half of costs a contact for nothing.
		for (size_t index = 0; index < dynamicRecords.size(); index++) {
			const ColliderRecord &a = dynamicRecords[index];
			const core::AABB &box = dynamicProxies[index].Bounds;

			// The index filters on `a.Mask` against each candidate's own
			// layers, which is one half of the rule. The other half needs the
			// candidate's mask, which is why a record exists at all.
			const spatial::QueryResult moving = spatial::OverlapBox(dynamicIndex, box, a.Mask, candidates);
			for (size_t at = 0; at < moving.Written; at++) {
				const auto other = static_cast<size_t>(candidates[at]);

				// Strictly greater, which does three things in one comparison:
				// it drops the query's own proxy, so nothing is ever paired
				// with itself; it keeps each unordered pair exactly once, since
				// the other side's query rejects the mirror; and it does both
				// without depending on entity ids, so the *set* of pairs is the
				// same however the rows happened to be laid out.
				if (other <= index) {
					continue;
				}

				const ColliderRecord &b = dynamicRecords[other];
				if (!PairAdmitted(a, b)) {
					continue;
				}
				pairs.push_back(Ordered(a.Owner, b.Owner));
			}

			const spatial::QueryResult anchored = spatial::OverlapBox(staticIndex, box, a.Mask, candidates);
			for (size_t at = 0; at < anchored.Written; at++) {
				const ColliderRecord &b = staticRecords[static_cast<size_t>(candidates[at])];

				// The two indexes are rebuilt on different schedules, so an
				// entity that has just gained a `Motion` is in the dynamic one
				// and still in the stale static one. `SyncBroadphase` notices
				// and rebuilds within the same tick, but a body paired with
				// itself is a body pushing itself, and one comparison is
				// cheaper than relying on that ordering staying true.
				if (b.Owner == a.Owner) {
					continue;
				}
				if (!PairAdmitted(a, b)) {
					continue;
				}
				pairs.push_back(Ordered(a.Owner, b.Owner));
			}
		}

		// **The sort is the determinism requirement.** Sequential impulse is
		// order-dependent, so a solver visiting contacts in grid-walk order
		// gives one answer on a scene built one way and another on the same
		// scene built another way — and `just determinism` reports it a long
		// way from here.
		std::sort(pairs.begin(), pairs.end());

		// Each pair once. The generation above already reports each one once;
		// this is what makes "once" a property of the list the solver reads
		// rather than a property of how it was filled, and applying one contact
		// twice doubles its impulse.
		pairs.erase(std::unique(pairs.begin(), pairs.end()), pairs.end());
	}
}
