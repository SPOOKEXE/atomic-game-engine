#pragma once

// A minimal custom native render node.
//
// The example owns no GPU object, but still uses the lifecycle hooks so it
// demonstrates the complete registration path a device-backed node follows.

#include <cstddef>
#include <memory>

namespace engine::render {
	class Renderer;
}

namespace engine::render::examples {

	// Observable state shared by the example handler and its caller.
	struct CustomNodeState {
		// Device lifecycle state and completed execution count.
		//@{
		bool DeviceReady = false;
		size_t Executions = 0;
		//@}
	};

	// Registers `example-marker` and installs its native handler.
	//
	// @param renderer The backend that will run the node.
	// @return Shared example state, or null when installation was refused.
	std::shared_ptr<CustomNodeState> InstallCustomNode(Renderer &renderer);
}
