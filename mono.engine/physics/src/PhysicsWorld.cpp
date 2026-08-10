#include <engine/ecs/Entity.hpp>
#include <engine/physics/PhysicsWorld.hpp>
#include <engine/spatial/HashGrid.hpp>

#include <algorithm>
#include <cstddef>

namespace engine::physics {

	PhysicsWorld::PhysicsWorld(float cellSize)
		: DynamicIndex(cellSize), StaticIndex(cellSize),
		  // **A size at or below zero means "measure it"**, which is what
		  // `PreparePhysicsWorld`'s default already passed and what
		  // `spatial::HashGrid`'s constructor already treats as "use the
		  // default". The two readings agree: an unconfigured world starts at
		  // the default and then sizes itself on its first sync, and a
		  // configured one keeps what the author named.
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

	size_t PhysicsWorld::SleepingBodies() const {
		size_t count = 0;
		for (const RestingBody &body : RestingList) {
			count += body.Asleep ? 1 : 0;
		}
		return count;
	}
}
