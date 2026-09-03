#include <engine/ecs/Entity.hpp>
#include <engine/physics/PhysicsWorld.hpp>
#include <engine/spatial/HashGrid.hpp>

#include <algorithm>
#include <cstddef>

namespace engine::physics {
	namespace {
		float InitialGridCellSize(float requested) {
			return requested > 0.0f ? requested : spatial::HashGrid::DEFAULT_CELL_SIZE;
		}

		template <class Value>
		PhysicsMemoryBytes VectorBytes(const std::vector<Value> &values, size_t liveCount) {
			return {liveCount * sizeof(Value), values.capacity() * sizeof(Value)};
		}

		template <class Value> PhysicsMemoryBytes VectorBytes(const std::vector<Value> &values) {
			return VectorBytes(values, values.size());
		}

		template <class Value>
		PhysicsMemoryBytes NestedVectorBytes(const std::vector<std::vector<Value>> &values) {
			PhysicsMemoryBytes bytes = VectorBytes(values);
			for (const std::vector<Value> &value : values) {
				const PhysicsMemoryBytes nested = VectorBytes(value);
				bytes.LiveBytes += nested.LiveBytes;
				bytes.RetainedBytes += nested.RetainedBytes;
			}
			return bytes;
		}

		template <class Key, class Value>
		PhysicsMemoryBytes UnorderedMapBytes(const std::unordered_map<Key, Value> &values) {
			// `clear()` releases nodes but keeps buckets. Live bytes name only the
			// current key-value payload; retained bytes also name the bucket table.
			// Allocator node overhead is not exposed by the standard container and
			// is deliberately not guessed.
			const size_t live = values.size() * sizeof(std::pair<const Key, Value>);
			const size_t retained = values.bucket_count() * sizeof(void *) + live;
			return {live, retained};
		}

		void Add(PhysicsMemoryBytes &total, PhysicsMemoryBytes part) {
			total.LiveBytes += part.LiveBytes;
			total.RetainedBytes += part.RetainedBytes;
		}
	}

	PhysicsWorld::PhysicsWorld(float cellSize)
		: DynamicIndex(InitialGridCellSize(cellSize)), StaticIndex(InitialGridCellSize(cellSize)),
		  // **A size at or below zero means "measure it"**, which is what
		  // `PreparePhysicsWorld`'s default already passed. Resolve that sentinel
		  // before the grids, where zero is invalid and correctly diagnostic. An
		  // unconfigured world starts at the default and sizes itself on its first
		  // sync; a configured one keeps what the author named.
		  MeasureCells(!(cellSize > 0.0f)) {}

	PhysicsMemoryStats PhysicsWorld::MemoryStats() const {
		PhysicsMemoryStats stats{};

		const spatial::HashGridStats dynamicGrid = DynamicIndex.Stats();
		stats.DynamicGrid = {dynamicGrid.LiveBytes, dynamicGrid.RetainedBytes};
		const spatial::HashGridStats staticGrid = StaticIndex.Stats();
		stats.StaticGrid = {staticGrid.LiveBytes, staticGrid.RetainedBytes};
		const spatial::DynamicBvhStats dynamicTree = DynamicTree.Stats();
		stats.DynamicTree = {dynamicTree.LiveBytes, dynamicTree.RetainedBytes};

		Add(stats.BroadphaseBuffers, VectorBytes(DynamicBounds));
		Add(stats.BroadphaseBuffers, VectorBytes(PreviousDynamicBounds));
		Add(stats.BroadphaseBuffers, VectorBytes(PreviousDynamicOwners));
		Add(stats.BroadphaseBuffers, VectorBytes(DynamicProxies));
		Add(stats.BroadphaseBuffers, VectorBytes(DynamicRecords));
		Add(stats.BroadphaseBuffers, VectorBytes(StaticRecords));
		Add(stats.BroadphaseBuffers, VectorBytes(DynamicShapes));
		Add(stats.BroadphaseBuffers, VectorBytes(StaticShapes));
		Add(stats.BroadphaseBuffers, VectorBytes(PairList));
		Add(stats.BroadphaseBuffers, VectorBytes(PairSourceList));
		Add(stats.BroadphaseBuffers, VectorBytes(SourcedPairList));
		Add(stats.BroadphaseBuffers, VectorBytes(SourcedPairSortScratch));
		Add(stats.BroadphaseBuffers, NestedVectorBytes(SourcedPairBatches));
		Add(stats.BroadphaseBuffers, VectorBytes(SourcedPairOverflow));
		Add(stats.BroadphaseBuffers, VectorBytes(CandidateBuffer));

		Add(stats.Solver, VectorBytes(ManifoldList));
		Add(stats.Solver, NestedVectorBytes(ManifoldBatches));
		Add(stats.Solver, VectorBytes(BodyList));
		Add(stats.Solver, UnorderedMapBytes(BodyIndexByOwner));
		Add(stats.Solver, VectorBytes(SolverBodyLoaded));
		Add(stats.Solver, VectorBytes(RowList, SolverRowCount));
		Add(stats.Solver, VectorBytes(BodyOwners));
		Add(stats.Solver, VectorBytes(BodyOwnerSortScratch));
		Add(stats.Solver, VectorBytes(ManifoldBodies));
		const spatial::ChunkMapStats chunks = SolverChunks.Stats();
		Add(stats.Solver, {chunks.LiveBytes, chunks.RetainedBytes});
		Add(stats.Solver, VectorBytes(SolverPoints));
		Add(stats.Solver, VectorBytes(GroupOfManifold));
		Add(stats.Solver, VectorBytes(GroupRowStart));
		Add(stats.Solver, VectorBytes(GroupRowCursor));
		Add(stats.Solver, VectorBytes(RowStartOfManifold));
		Add(stats.Solver, VectorBytes(ImpulseStartOfManifold));
		Add(stats.Solver, VectorBytes(SolverGroups));
		Add(stats.Solver, VectorBytes(SolverColors));
		Add(stats.Solver, VectorBytes(SolverTopology));
		Add(stats.Solver, VectorBytes(SolverTopologyScratch));
		Add(stats.Solver, VectorBytes(SolverColorOfManifold));
		Add(stats.Solver, VectorBytes(SolverColorClaims));
		Add(stats.Solver, VectorBytes(SolverComponentParents));
		Add(stats.Solver, VectorBytes(SolverIslandOfManifold));
		Add(stats.Solver, VectorBytes(SolverIslandOfBody));
		Add(stats.Solver, VectorBytes(SolverIslandRows));
		Add(stats.Solver, VectorBytes(SolverIslandRowOrderScratch));

		Add(stats.Persistent, VectorBytes(EventList));
		Add(stats.Persistent, VectorBytes(PersistentManifolds));
		Add(stats.Persistent, VectorBytes(PersistentNext));
		Add(stats.Persistent, NestedVectorBytes(PersistentManifoldBatches));
		Add(stats.Persistent, VectorBytes(PersistentCandidates));
		Add(stats.Persistent, VectorBytes(PersistentCandidateNext));
		Add(stats.Persistent, NestedVectorBytes(PersistentCandidateBatches));
		Add(stats.Persistent, VectorBytes(PersistentManifoldBatchStats));
		Add(stats.Persistent, VectorBytes(ImpulseCache));
		Add(stats.Persistent, VectorBytes(ImpulseNext));
		Add(stats.Persistent, VectorBytes(RestingList));
		Add(stats.Persistent, VectorBytes(RestingNext));
		Add(stats.Persistent, VectorBytes(WeldPoses));
		Add(stats.Persistent, VectorBytes(WeldPosesNext));
		Add(stats.Persistent, VectorBytes(RigidEdges));
		Add(stats.Persistent, VectorBytes(RigidNodes));
		Add(stats.Persistent, VectorBytes(TouchingLast));
		Add(stats.Persistent, VectorBytes(TouchingNow));
		return stats;
	}

	bool PhysicsWorld::Sleeping(ecs::Entity entity) const {
		// The list is kept sorted by entity so this is a binary search rather
		// than a walk, and so that carrying it from one tick to the next is a
		// merge. A hash map would answer the same question and would also make
		// the tick's iteration order the allocator's, which §2.4 refuses.
		const RestingBody probe{entity, 0.0f, false};
		const auto found = std::lower_bound(RestingList.begin(), RestingList.end(), probe);
		return found != RestingList.end() && found->Owner == entity && found->Asleep;
	}

	bool PhysicsWorld::Wake(ecs::Entity entity) {
		const RestingBody probe{entity, 0.0f, false};
		const auto found = std::lower_bound(RestingList.begin(), RestingList.end(), probe);
		if (found == RestingList.end() || found->Owner != entity) {
			return false;
		}

		const bool was = found->Asleep;

		// **Erased rather than reset in place**, which keeps the list's
		// invariant trivially: it is sorted by owner and holds only bodies with
		// rest accumulated, so a body with none has no row. The next tick that
		// finds it still puts it back.
		RestingList.erase(found);
		return was;
	}

	size_t PhysicsWorld::SleepingBodies() const {
		size_t count = 0;
		for (const RestingBody &body : RestingList) {
			count += body.Asleep ? 1 : 0;
		}
		return count;
	}

	bool PhysicsWorld::RigidlyConnected(ecs::Entity first, ecs::Entity second) const {
		const auto node = [this](ecs::Entity part) {
			return std::lower_bound(
				RigidNodes.begin(), RigidNodes.end(), part, [](const RigidNode &entry, ecs::Entity wanted) {
					return entry.Part.Id < wanted.Id;
				}
			);
		};
		const auto a = node(first);
		const auto b = node(second);
		return a != RigidNodes.end() && b != RigidNodes.end() && a->Part == first && b->Part == second &&
			   a->Root != ecs::NULL_ENTITY && a->Root == b->Root;
	}
}
