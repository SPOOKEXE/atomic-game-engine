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

namespace engine::control {
	class Surface;
}

namespace engine::render {
	class Renderer;
	class ScriptDataCaptureBridge;
}

namespace engine::world {
	class DataFactorySession;
	class Universe;
}

namespace engine::graph {
	class PipelineDocument;
}

namespace client {
	// Makes the renderer-owned snapshot callback an explicit hook dependency.
	struct VisibilityObservationHookContext {
		engine::control::HookRegistry &Hooks;
		std::function<engine::control::features::VisibilitySnapshotReply()> Snapshot;
	};

	// Both readers are fenced to the same factory session and universe.
	struct FactoryReadHookContext {
		engine::control::HookRegistry &Hooks;
		engine::world::Universe &Universe;
		engine::world::DataFactorySession &Session;
	};

	// The bridge owns copied audio records and its metadata resource.
	struct DataAudioObservationHookContext {
		engine::control::HookRegistry &Hooks;
		engine::world::Universe &Universe;
		std::shared_ptr<engine::script::DataAudioObservationBridge> Bridge;
		engine::world::DataFactorySession &Session;
	};

	// Captures client-owned state needed to install ticketed data capture tools.
	struct DataCaptureHookContext {
		engine::control::Surface &Surface;
		engine::world::DataFactorySession &Session;
		std::shared_ptr<engine::render::ScriptDataCaptureBridge> Bridge;
	};

	// Produces the control-owned copy of the renderer's latest visibility snapshot.
	engine::control::features::VisibilitySnapshotReply
	VisibilityObservationSnapshot(const engine::render::Renderer &renderer);

	// Activates the renderer-owned visibility reader for this client process.
	engine::control::HookLease
	ActivateVisibilityObservationHook(VisibilityObservationHookContext context, std::string &failure);

	// Activates session-fenced temporal pose sampling for this data-factory client.
	engine::control::HookLease
	ActivateTemporalSampleHook(FactoryReadHookContext context, std::string &failure);

	// Activates data-rig export for this data-factory client.
	engine::control::HookLease ActivateRigExportHook(FactoryReadHookContext context, std::string &failure);

	// Activates the bridge-owned audio metadata and readers as one provider transaction.
	engine::control::HookLease
	ActivateDataAudioObservationHook(DataAudioObservationHookContext context, std::string &failure);

	// Activates the client capture provider and drains its tickets on hook close.
	engine::control::HookLease ActivateDataCaptureHook(DataCaptureHookContext context, std::string &failure);

	// Preserves renderer admission details in the existing render-graph tool error.
	bool RenderGraphAdmissionFailure(
		const engine::render::Renderer &renderer,
		const engine::graph::PipelineDocument &document,
		std::string &failure
	);
}
