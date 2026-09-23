#include <engine/control/features/DataFactory.hpp>

namespace engine::control::features {

	// A product explicitly opts into the lifecycle vocabulary for the session it
	// owns. The feature retains no state beyond the Surface's tool closures.
	Feature DataFactory(world::DataFactorySession &session, DataFactoryToolSet tools) {
		return Feature{"data_factory", [&session, tools](Surface &surface) {
						   surface.AddDataFactoryTools(session, tools);
					   }};
	}
}
