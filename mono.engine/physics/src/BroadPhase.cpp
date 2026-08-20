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
		ENGINE_PROFILE_CAT("physics.query", core::ProfileCategory::Physics);

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
				sourced.push_back(
					Ordered(a.Owner, b.Owner, static_cast<uint32_t>(index), static_cast<uint32_t>(other))
				);
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
				sourced.push_back(Ordered(
					a.Owner,
					b.Owner,
					static_cast<uint32_t>(index),
					static_cast<uint32_t>(candidates[at]) | CandidateSource::STATIC
				));
			}
		}

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

		// **Split into the public list and the private one, after the sort.**
		// The pair list is what a manifold, a contact event and every consumer
		// outside this module reads, and it names entities; the indices beside it
		// are into `PhysicsWorld`'s own arrays and are nobody else's business -
		// `CandidateSource` says why publishing one would hand a caller a number
		// that is plausible and wrong.
		pairs.reserve(sourced.size());
		sources.reserve(sourced.size());
		for (const SourcedPair &row : sourced) {
			pairs.push_back(row.Pair);
			sources.push_back(row.Source);
		}
	}
}
