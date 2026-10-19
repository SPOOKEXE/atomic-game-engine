#pragma once

// A setup job visits sorted pairs. Keep its cache position local to that fixed range.
#include <engine/physics/PhysicsWorld.hpp>

#include <algorithm>
#include <cstddef>
#include <span>

namespace engine::physics {
	struct ImpulseCacheCursor {
		std::span<const ContactImpulse> Cache;
		size_t Next = 0;
		bool Started = false;

		// Calls must follow ascending pair order; features are searched within the returned run.
		std::span<const ContactImpulse> Pair(ecs::Entity first, ecs::Entity second) {
			const ContactImpulse key{first, second, 0, 0.0f, {0.0f, 0.0f}};
			if (!Started) {
				Next = static_cast<size_t>(std::lower_bound(Cache.begin(), Cache.end(), key) - Cache.begin());
				Started = true;
			} else {
				while (Next < Cache.size() && Cache[Next] < key)
					Next++;
			}
			size_t end = Next;
			while (end < Cache.size() && Cache[end].A == first && Cache[end].B == second)
				end++;
			return Cache.subspan(Next, end - Next);
		}
	};
}
