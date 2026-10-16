#pragma once

#include <engine/script/ComputeBridge.hpp>

#include <memory>

namespace engine::render {
	// One renderer-wide copied inbox and device executor pool, shared by all its VMs.
	class ScriptComputeBridge {
	  public:
		// Borrows the existing renderer device; construction does not create a device.
		explicit ScriptComputeBridge(void *device);
		~ScriptComputeBridge();
		ScriptComputeBridge(const ScriptComputeBridge &) = delete;
		ScriptComputeBridge &operator=(const ScriptComputeBridge &) = delete;
		// Creates a factory whose endpoints are scoped to one stable world and view.
		script::ComputeBridgeFactory ForWorld(std::string world, uint32_t view = UINT32_MAX);
		// Device owner thread only. Outside bytes are ordinary renderer graph buffers.
		void Pump(uint64_t outsideBytes);
		// Includes every installed and retired executor until its GPU jobs complete.
		uint64_t ReservedDeviceBytes() const;
		// Called by Renderer before destroying its device. May wait only at teardown.
		void Shutdown();

	  private:
		struct State;
		std::shared_ptr<State> Held;
	};
}
