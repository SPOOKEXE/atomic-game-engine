#pragma once

#include <engine/ecs/Entity.hpp>
#include <engine/scene/DrawInstance.hpp>

#include <vector>

namespace engine::ecs {
	class Store;
}

namespace engine::render {

	// Collects drawable descendants owned by one ViewportFrame.
	void CollectViewportInstances(
		const ecs::Store &store, ecs::Entity viewport, std::vector<scene::DrawInstance> &out
	);
}
