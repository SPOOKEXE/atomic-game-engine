#include <engine/core/Name.hpp>
#include <engine/ecs/Classes.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/scene/BreakGroup.hpp>
#include <engine/scene/Components.hpp>

#include <vector>

namespace engine::scene {

	size_t ReleaseBreakGroup(ecs::Store &store, ecs::Entity group) {
		const ecs::ClassId breakGroup = ecs::Classes::Find(core::Name("BreakGroup"));
		const ecs::ClassId basePart = ecs::Classes::Find(core::Name("BasePart"));
		if (!breakGroup.IsValid() || !basePart.IsValid() || !store.IsA(group, breakGroup)) {
			return 0;
		}

		// Collect first. Releasing a part changes archetypes, and the structural
		// work remains outside the hierarchy walk that selected the pieces.
		std::vector<ecs::Entity> released;
		store.EachDescendant(group, [&](ecs::Entity descendant) {
			if (store.IsA(descendant, basePart) && !store.Has<Simulated>(descendant)) {
				released.push_back(descendant);
			}
		});

		for (const ecs::Entity part : released) {
			// This is the same state transition `Anchored = false` performs. The
			// group names release directly, so it does not need a second spelling of
			// Anchored's inverted public polarity.
			store.Set(part, Simulated{});
			store.Set(part, Motion{});
		}
		return released.size();
	}
}
