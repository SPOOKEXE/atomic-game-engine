#pragma once

// Reaching `PhysicsWorld`'s storage. Private to this module.
//
// The two indexes, the proxy and record arrays and the pair list are written by
// the systems in this directory and read by the suites and benchmarks beside
// them - and not one of those is another module. A set of public setters would
// turn the layout into an API somebody outside could depend on, so this is a
// friend instead. `spatial/src/GridInternals.hpp` does the same for the same
// reason, and the symmetry is deliberate.

#include <engine/ecs/Entity.hpp>
#include <engine/physics/Contacts.hpp>
#include <engine/physics/PhysicsWorld.hpp>
#include <engine/spatial/HashGrid.hpp>

#include <cstdint>
#include <vector>

namespace engine::physics {

	// Reaches the resource's storage, and nothing else.
	struct PipelineInternals {
		// The index over colliders that can move, rebuilt every tick.
		static spatial::HashGrid &DynamicIndex(PhysicsWorld &world) {
			return world.DynamicIndex;
		}

		static spatial::DynamicBvh &DynamicTree(PhysicsWorld &world) {
			return world.DynamicTree;
		}

		static const spatial::DynamicBvh &DynamicTree(const PhysicsWorld &world) {
			return world.DynamicTree;
		}

		static bool &DynamicTreeActive(PhysicsWorld &world) {
			return world.DynamicTreeActive;
		}

		static bool DynamicTreeActive(const PhysicsWorld &world) {
			return world.DynamicTreeActive;
		}

		static size_t &DynamicTreeSettledFrames(PhysicsWorld &world) {
			return world.DynamicTreeSettledFrames;
		}

		// The index over colliders that cannot, rebuilt when the set changes.
		static spatial::HashGrid &StaticIndex(PhysicsWorld &world) {
			return world.StaticIndex;
		}

		// The same four, for a reader. The collider queries take a `const
		// Store &` because several workers may run one each at the same time,
		// and a mutable accessor would be the one line that quietly ended that.
		static const spatial::HashGrid &DynamicIndex(const PhysicsWorld &world) {
			return world.DynamicIndex;
		}

		static const spatial::HashGrid &StaticIndex(const PhysicsWorld &world) {
			return world.StaticIndex;
		}

		static const std::vector<ColliderRecord> &DynamicRecords(const PhysicsWorld &world) {
			return world.DynamicRecords;
		}

		static const std::vector<ColliderRecord> &StaticRecords(const PhysicsWorld &world) {
			return world.StaticRecords;
		}

		// The dynamic bounds, parallel to `DynamicRecords` by index. The grid
		// owns its proxy rows, so this keeps only the data the pair walk needs.
		static std::vector<core::AABB> &DynamicBounds(PhysicsWorld &world) {
			return world.DynamicBounds;
		}

		static std::vector<core::AABB> &SpeculativeBounds(PhysicsWorld &world) {
			return world.SpeculativeBounds;
		}

		static std::vector<core::AABB> &PreviousDynamicBounds(PhysicsWorld &world) {
			return world.PreviousDynamicBounds;
		}

		static std::vector<ecs::Entity> &PreviousDynamicOwners(PhysicsWorld &world) {
			return world.PreviousDynamicOwners;
		}

		static std::vector<spatial::Proxy> &DynamicProxies(PhysicsWorld &world) {
			return world.DynamicProxies;
		}

		// What the broad phase knows about each dynamic collider.
		static std::vector<ColliderRecord> &DynamicRecords(PhysicsWorld &world) {
			return world.DynamicRecords;
		}

		// What the broad phase knows about each static collider.
		static std::vector<ColliderRecord> &StaticRecords(PhysicsWorld &world) {
			return world.StaticRecords;
		}

		// The pair list, cleared and refilled by `BroadPhase`.
		static std::vector<CandidatePair> &Pairs(PhysicsWorld &world) {
			return world.PairList;
		}

		// Which proxy each pair came from, parallel to the pair list.
		static std::vector<CandidateSource> &PairSources(PhysicsWorld &world) {
			return world.PairSourceList;
		}

		// Where the broad phase sorts the two together before splitting them.
		static std::vector<SourcedPair> &SourcedPairs(PhysicsWorld &world) {
			return world.SourcedPairList;
		}

		// The second array used while ordering pairs by their entity ids.
		static std::vector<SourcedPair> &SourcedPairSortScratch(PhysicsWorld &world) {
			return world.SourcedPairSortScratch;
		}

		// Per-batch pair output and the batches that need a full-size retry.
		static std::vector<std::vector<SourcedPair>> &SourcedPairBatches(PhysicsWorld &world) {
			return world.SourcedPairBatches;
		}

		static std::vector<uint8_t> &SourcedPairOverflow(PhysicsWorld &world) {
			return world.SourcedPairOverflow;
		}

		// The placed shape of every collider, parallel to the records.
		//@{
		static std::vector<PlacedCollider> &DynamicShapes(PhysicsWorld &world) {
			return world.DynamicShapes;
		}

		static std::vector<PlacedCollider> &StaticShapes(PhysicsWorld &world) {
			return world.StaticShapes;
		}
		//@}

		// The manifold list, cleared and refilled by `NarrowPhase`.
		static std::vector<ContactManifold> &Manifolds(PhysicsWorld &world) {
			return world.ManifoldList;
		}

		// One retained output list per fixed narrow-phase range.
		static std::vector<std::vector<ContactManifold>> &ManifoldBatches(PhysicsWorld &world) {
			return world.ManifoldBatches;
		}

		static std::vector<ContactManifold> &SpeculativeManifolds(PhysicsWorld &world) {
			return world.SpeculativeManifolds;
		}

		static std::vector<std::vector<ContactManifold>> &SpeculativeManifoldBatches(PhysicsWorld &world) {
			return world.SpeculativeManifoldBatches;
		}

		static std::vector<SolverManifoldIndex> &SolverManifoldOrder(PhysicsWorld &world) {
			return world.SolverManifoldOrder;
		}

		static std::vector<PersistentContactManifold> &PersistentManifolds(PhysicsWorld &world) {
			return world.PersistentManifolds;
		}

		static std::vector<PersistentContactManifold> &PersistentNext(PhysicsWorld &world) {
			return world.PersistentNext;
		}

		static std::vector<std::vector<PersistentContactManifold>> &
		PersistentManifoldBatches(PhysicsWorld &world) {
			return world.PersistentManifoldBatches;
		}

		static std::vector<PersistentContactCandidate> &PersistentCandidates(PhysicsWorld &world) {
			return world.PersistentCandidates;
		}

		static std::vector<PersistentContactCandidate> &PersistentCandidateNext(PhysicsWorld &world) {
			return world.PersistentCandidateNext;
		}

		static std::vector<std::vector<PersistentContactCandidate>> &
		PersistentCandidateBatches(PhysicsWorld &world) {
			return world.PersistentCandidateBatches;
		}

		static std::vector<PersistentContactBatchStats> &PersistentManifoldBatchStatsOf(PhysicsWorld &world) {
			return world.PersistentManifoldBatchStats;
		}

		// The event list, cleared by `NarrowPhase` and filled by `Publish`.
		static std::vector<ContactEvent> &Events(PhysicsWorld &world) {
			return world.EventList;
		}

		// The solver's compact body array, refilled by `Solve`.
		static std::vector<SolverBody> &Bodies(PhysicsWorld &world) {
			return world.BodyList;
		}

		// Which compact bodies the dense BasePart walk has already loaded.
		static std::vector<uint8_t> &BodyLoaded(PhysicsWorld &world) {
			return world.SolverBodyLoaded;
		}

		// The per-point constraint rows, refilled by `Solve`.
		//
		// **`size()` is the high-water mark**; `RowCount` below is the length.
		static std::vector<ContactRow> &Rows(PhysicsWorld &world) {
			return world.RowList;
		}

		// How many of those rows this tick actually filled.
		static size_t &RowCount(PhysicsWorld &world) {
			return world.SolverRowCount;
		}

		// The gather's sort buffer, and the body indices it resolves.
		static std::vector<ecs::Entity> &BodyOwners(PhysicsWorld &world) {
			return world.BodyOwners;
		}

		static std::vector<ecs::Entity> &BodyOwnerSortScratch(PhysicsWorld &world) {
			return world.BodyOwnerSortScratch;
		}

		// Lookup only. Iteration stays on `Bodies`, whose entity order is explicit.
		static std::unordered_map<uint64_t, size_t> &BodyIndexByOwner(PhysicsWorld &world) {
			return world.BodyIndexByOwner;
		}

		static std::vector<std::pair<uint32_t, uint32_t>> &ManifoldBodies(PhysicsWorld &world) {
			return world.ManifoldBodies;
		}

		// Last tick's accumulated impulses, which this tick warm-starts from.
		static std::vector<ContactImpulse> &ImpulseCache(PhysicsWorld &world) {
			return world.ImpulseCache;
		}

		// This tick's accumulated impulses, for the tick after it.
		static std::vector<ContactImpulse> &ImpulseNext(PhysicsWorld &world) {
			return world.ImpulseNext;
		}

		// How long each body has been still, carried across ticks.
		static std::vector<RestingBody> &Resting(PhysicsWorld &world) {
			return world.RestingList;
		}

		// Where the next tick's resting list is merged before the two swap.
		static std::vector<RestingBody> &RestingNext(PhysicsWorld &world) {
			return world.RestingNext;
		}

		static std::vector<WeldPose> &WeldPoses(PhysicsWorld &world) {
			return world.WeldPoses;
		}

		static std::vector<WeldPose> &WeldPosesNext(PhysicsWorld &world) {
			return world.WeldPosesNext;
		}

		static std::vector<RigidEdge> &RigidEdges(PhysicsWorld &world) {
			return world.RigidEdges;
		}

		static std::vector<RigidNode> &RigidNodes(PhysicsWorld &world) {
			return world.RigidNodes;
		}

		// The pairs that were touching at the end of the previous tick.
		static std::vector<CandidatePair> &TouchingLast(PhysicsWorld &world) {
			return world.TouchingLast;
		}

		// The pairs touching at the end of this one.
		static std::vector<CandidatePair> &TouchingNow(PhysicsWorld &world) {
			return world.TouchingNow;
		}

		// Scratch for an overlap query's answer, reused across every query in a
		// tick.
		static std::vector<uint64_t> &CandidateBuffer(PhysicsWorld &world) {
			return world.CandidateBuffer;
		}

		// Whether the static index is known to be out of date.
		static bool &StaticStale(PhysicsWorld &world) {
			return world.StaticStale;
		}

		// The store change counter as of the last time the static set was
		// examined. Equal to `Store::ChangeVersion()` means nothing has been
		// written through `Set` since, so there is nothing to re-examine.
		static uint64_t &StaticChangeVersion(PhysicsWorld &world) {
			return world.StaticChangeVersion;
		}

		// How many times the dynamic index has been rebuilt.
		static uint64_t &DynamicRebuildCount(PhysicsWorld &world) {
			return world.DynamicRebuildCount;
		}

		// How many times the static index has been rebuilt.
		static uint64_t &StaticRebuildCount(PhysicsWorld &world) {
			return world.StaticRebuildCount;
		}

		// How many bodies the continuous step has clamped.
		static uint64_t &SweptBodyCount(PhysicsWorld &world) {
			return world.SweptBodyCount;
		}

		// The cell size the world was constructed with, for a reader that has
		// to reconstruct the grids - a snapshot, above all.
		static float CellSize(const PhysicsWorld &world) {
			return world.DynamicIndex.CellSize();
		}

		// The partition the parallel solve batches by, and the points it is
		// built from.
		//@{
		static spatial::ChunkMap &SolverChunks(PhysicsWorld &world) {
			return world.SolverChunks;
		}

		static std::vector<spatial::Proxy> &SolverPoints(PhysicsWorld &world) {
			return world.SolverPoints;
		}

		// The chunk edge the last solve used, or zero when it did not partition.
		static float &SolverChunkEdge(PhysicsWorld &world) {
			return world.SolverChunkEdge;
		}
		//@}

		// The counting pass's answer per manifold, and the offsets it turns
		// into.
		//@{
		static std::vector<uint32_t> &GroupOfManifold(PhysicsWorld &world) {
			return world.GroupOfManifold;
		}

		static std::vector<uint32_t> &GroupRowStart(PhysicsWorld &world) {
			return world.GroupRowStart;
		}

		// Where the filling pass is up to in each group. A second array rather
		// than the starts advanced in place, for `HashGrid::Rebuild`'s reason:
		// the starts are what the sweeps read.
		static std::vector<uint32_t> &GroupRowCursor(PhysicsWorld &world) {
			return world.GroupRowCursor;
		}

		// Where each manifold's rows begin. What the dispatched set-up pass
		// writes against instead of a shared cursor.
		static std::vector<uint32_t> &RowStartOfManifold(PhysicsWorld &world) {
			return world.RowStartOfManifold;
		}

		// Where each manifold's impulses begin. Pair order, unlike the rows.
		static std::vector<uint32_t> &ImpulseStartOfManifold(PhysicsWorld &world) {
			return world.ImpulseStartOfManifold;
		}
		//@}

		// The groups a sweep dispatches over, and the rows left over.
		//@{
		static std::vector<SolverGroup> &SolverGroups(PhysicsWorld &world) {
			return world.SolverGroups;
		}

		static std::vector<SolverColor> &SolverColors(PhysicsWorld &world) {
			return world.SolverColors;
		}

		static const std::vector<SolverColor> &SolverColors(const PhysicsWorld &world) {
			return world.SolverColors;
		}

		static std::vector<SolverTopologyEntry> &SolverTopology(PhysicsWorld &world) {
			return world.SolverTopology;
		}

		static std::vector<SolverTopologyEntry> &SolverTopologyScratch(PhysicsWorld &world) {
			return world.SolverTopologyScratch;
		}

		static std::vector<uint32_t> &SolverColorOfManifold(PhysicsWorld &world) {
			return world.SolverColorOfManifold;
		}

		static const std::vector<uint32_t> &SolverColorOfManifold(const PhysicsWorld &world) {
			return world.SolverColorOfManifold;
		}

		static std::vector<uint64_t> &SolverColorClaims(PhysicsWorld &world) {
			return world.SolverColorClaims;
		}

		static std::vector<uint32_t> &SolverComponentParents(PhysicsWorld &world) {
			return world.SolverComponentParents;
		}

		static std::vector<uint32_t> &SolverIslandOfManifold(PhysicsWorld &world) {
			return world.SolverIslandOfManifold;
		}

		static const std::vector<uint32_t> &SolverIslandOfManifold(const PhysicsWorld &world) {
			return world.SolverIslandOfManifold;
		}

		static std::vector<uint32_t> &SolverIslandOfBody(PhysicsWorld &world) {
			return world.SolverIslandOfBody;
		}

		static std::vector<uint32_t> &SolverIslandRows(PhysicsWorld &world) {
			return world.SolverIslandRows;
		}

		static std::vector<uint32_t> &SolverIslandRowOrderScratch(PhysicsWorld &world) {
			return world.SolverIslandRowOrderScratch;
		}

		static bool &SolverIslandTopologyKnown(PhysicsWorld &world) {
			return world.SolverIslandTopologyKnown;
		}

		static bool &SolverUsesIslands(PhysicsWorld &world) {
			return world.SolverUsesIslands;
		}

		static size_t &SolverIslandCount(PhysicsWorld &world) {
			return world.SolverIslandCount;
		}

		static bool &SolverColorTopologyAccepted(PhysicsWorld &world) {
			return world.SolverColorTopologyAccepted;
		}

		static bool &SolverColorTopologyKnown(PhysicsWorld &world) {
			return world.SolverColorTopologyKnown;
		}

		static bool &SolverUsesColoring(PhysicsWorld &world) {
			return world.SolverUsesColoring;
		}

		static SolverGroup &BorderRows(PhysicsWorld &world) {
			return world.BorderRows;
		}
		//@}
	};
}
