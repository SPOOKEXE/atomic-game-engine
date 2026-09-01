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
	}

	PhysicsWorld::PhysicsWorld(float cellSize)
		: DynamicIndex(InitialGridCellSize(cellSize)), StaticIndex(InitialGridCellSize(cellSize)),
		  // **A size at or below zero means "measure it"**, which is what
		  // `PreparePhysicsWorld`'s default already passed. Resolve that sentinel
		  // before the grids, where zero is invalid and correctly diagnostic. An
		  // unconfigured world starts at the default and sizes itself on its first
		  // sync; a configured one keeps what the author named.
		  MeasureCells(!(cellSize > 0.0f)) {}

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
				RigidNodes.begin(), RigidNodes.end(), part,
				[](const RigidNode &entry, ecs::Entity wanted) { return entry.Part.Id < wanted.Id; }
			);
		};
		const auto a = node(first);
		const auto b = node(second);
		return a != RigidNodes.end() && b != RigidNodes.end() && a->Part == first && b->Part == second &&
			a->Root != ecs::NULL_ENTITY && a->Root == b->Root;
	}
}
