#pragma once

// Authored weather properties use one validated write boundary.

#include <engine/ecs/Classes.hpp>

namespace engine::scene {
	void RegisterWeatherComputeProperties(ecs::ClassId owner);
}
