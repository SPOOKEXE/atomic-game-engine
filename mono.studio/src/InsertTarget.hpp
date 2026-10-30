#pragma once

#include <engine/world/World.hpp>

namespace studio {
	inline engine::world::WorldId InsertTargetWorld(
		engine::world::WorldId viewport,
		engine::world::WorldId selectedWorld,
		engine::world::WorldId selectionWorld,
		bool hasSelection
	) {
		if (hasSelection) return selectionWorld;
		if (selectedWorld.IsValid()) return selectedWorld;
		return viewport;
	}
}
