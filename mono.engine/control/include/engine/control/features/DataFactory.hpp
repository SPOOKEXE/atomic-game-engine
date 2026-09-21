#pragma once

// Registers the data-factory lifecycle vocabulary for one host-owned session.

#include <engine/control/Surface.hpp>

namespace engine::control::features {

	// A product explicitly opts into the lifecycle vocabulary for the session it
	// owns. The feature retains no state beyond the Surface's tool closures.
	Feature DataFactory(world::DataFactorySession &session, DataFactoryToolSet tools = {});
}
