#pragma once

// Decides whether ECS source changes require copied draw rows or only pose work.

#include <engine/render/WorldPresentation.hpp>

#include <cstddef>

namespace engine::ecs {
	class Store;
}

namespace engine::render {

	struct PresentationSourceDamage {
		bool Pose = false;
		bool Full = false;
	};

	PresentationSourceDamage CollectPresentationSourceDamage(
		ecs::Store &store, DrawList &drawList, size_t matching, size_t skeletons, size_t bones
	);
}
