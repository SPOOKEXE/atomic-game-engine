#pragma once

#include <engine/control/HookRegistry.hpp>
#include <engine/control/features/AudioObservation.hpp>
#include <engine/control/features/RigExport.hpp>
#include <engine/control/features/TemporalSample.hpp>
#include <engine/control/features/VisibilityObservation.hpp>

#include <functional>
#include <memory>
#include <string>

namespace engine::script {
	class DataAudioObservationBridge;
}

namespace engine::render {
	class Renderer;
}

namespace engine::world {
	class DataFactorySession;
	class Universe;
}

namespace client {

	// Produces the control-owned copy of the renderer's latest visibility snapshot.
	engine::control::features::VisibilitySnapshotReply
	VisibilityObservationSnapshot(const engine::render::Renderer &renderer);

	// Activates the renderer-owned visibility reader for this client process.
	engine::control::HookLease ActivateVisibilityObservationHook(
		engine::control::HookRegistry &hooks,
		std::function<engine::control::features::VisibilitySnapshotReply()> snapshot,
		std::string &failure
	);

	// Activates session-fenced temporal pose sampling for this data-factory client.
	engine::control::HookLease ActivateTemporalSampleHook(
		engine::control::HookRegistry &hooks,
		engine::world::Universe &universe,
		engine::world::DataFactorySession &session,
		std::string &failure
	);

	// Activates data-rig export for this data-factory client.
	engine::control::HookLease ActivateRigExportHook(
		engine::control::HookRegistry &hooks,
		engine::world::Universe &universe,
		engine::world::DataFactorySession &session,
		std::string &failure
	);

	// Activates the bridge-owned audio metadata and readers as one provider transaction.
	engine::control::HookLease ActivateDataAudioObservationHook(
		engine::control::HookRegistry &hooks,
		engine::world::Universe &universe,
		std::shared_ptr<engine::script::DataAudioObservationBridge> bridge,
		engine::world::DataFactorySession &session,
		std::string &failure
	);
}
