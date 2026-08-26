#pragma once

#include <alpha/Health.hpp>

namespace alpha {

	inline void RegisterAlphaComponents() {
		engine::ecs::Components::Register<Health>("alpha.Health");
	}
}
