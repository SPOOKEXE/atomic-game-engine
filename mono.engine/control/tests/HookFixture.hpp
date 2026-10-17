#pragma once

// Test fixtures for the named control hooks used by product manifests.

#include <engine/control/Surface.hpp>
#include <engine/control/features/AudioObservation.hpp>
#include <engine/control/features/DataCapture.hpp>
#include <engine/control/features/DataScene.hpp>
#include <engine/control/features/RenderGraph.hpp>
#include <engine/control/features/VisibilityObservation.hpp>

#include <algorithm>
#include <functional>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace engine::control::test {
	struct Spec {
		std::string Name;
		std::function<void(Surface &)> Install;
	};

	inline void Install(Surface &surface, std::span<const Spec> specs) {
		// Leases are weak. Keep each test hook open without extending the surface lifetime.
		static thread_local std::vector<HookLease> leases;
		std::erase_if(leases, [](const HookLease &lease) { return !lease.IsValid(); });
		for (const Spec &spec : specs) {
			if (!spec.Install) continue;
			std::string failure;
			HookLease lease = surface.ActivateHook(
				{.Id = "builtin." + spec.Name,
				 .Revision = "v1",
				 .Purpose = "Built-in feature registration.",
				 .Dependencies = {},
				 .Limits = {}},
				[&](HookRegistration &) { spec.Install(surface); },
				failure
			);
			if (!failure.empty()) throw std::runtime_error(failure);
			leases.push_back(std::move(lease));
		}
	}

	inline Spec Custom(std::string name, std::function<void(Surface &)> install) {
		return {std::move(name), std::move(install)};
	}
	inline Spec Universe(world::Universe &universe, bool writable = true, bool includeEngineInfo = true) {
		return {"universe", [&universe, writable, includeEngineInfo](Surface &surface) {
					surface.AddUniverseTools(universe, writable, includeEngineInfo);
				}};
	}
	inline Spec Architecture() {
		return {"architecture", [](Surface &surface) { surface.AddArchitectureTools(); }};
	}
	inline Spec Script() {
		return {"script", [](Surface &surface) { surface.AddScriptTools(); }};
	}
	inline Spec Diagnostics(bool includeLogTail = true) {
		return {"diagnostics", [includeLogTail](Surface &surface) {
					surface.AddDiagnosticTools(includeLogTail);
				}};
	}
	inline Spec Build() {
		return {"build", [](Surface &surface) { surface.AddBuildTools(); }};
	}
	inline Spec Resources() {
		return {"resources", [](Surface &surface) { surface.AddStandardResources(); }};
	}
	inline Spec Prompts() {
		return {"prompts", [](Surface &surface) { surface.AddStandardPrompts(); }};
	}
	inline Spec Discovery() {
		return {"discovery", [](Surface &surface) { surface.AddDiscoveryTools(); }};
	}
	inline Spec RenderGraph() {
		return {"render_graph", [](Surface &surface) { features::RenderGraph(surface); }};
	}
	inline Spec DataFactory(world::DataFactorySession &session, DataFactoryToolSet tools = {}) {
		return {"data_factory", [&session, tools](Surface &surface) {
					surface.AddDataFactoryTools(session, tools);
				}};
	}
	inline Spec DataScene(
		world::Universe &universe,
		std::shared_ptr<script::DataCaptureBridge> bridge = {},
		world::DataFactorySession *session = nullptr,
		script::GltfMeshSource meshSource = {},
		script::GltfTextureSource textureSource = {}
	) {
		return {
			"data_scene",
			[&universe,
			 bridge = std::move(bridge),
			 session,
			 meshSource = std::move(meshSource),
			 textureSource = std::move(textureSource)](Surface &surface) {
				surface.AddDataSceneTools(universe, bridge, session, meshSource, textureSource);
			}
		};
	}
	inline Spec
	DataCapture(world::DataFactorySession &session, std::shared_ptr<script::DataCaptureBridge> bridge) {
		return {"data_capture", [&session, bridge = std::move(bridge)](Surface &surface) {
					surface.AddDataCaptureTools(session, bridge);
				}};
	}
	inline Spec DataAudioObservation(
		world::Universe &universe,
		std::shared_ptr<script::DataAudioObservationBridge> bridge,
		world::DataFactorySession *session = nullptr
	) {
		return {"audio_observation", [&universe, bridge = std::move(bridge), session](Surface &surface) {
					AddDataAudioObservationTools(surface, universe, bridge, session);
				}};
	}
	inline Spec TemporalSample(world::Universe &universe, world::DataFactorySession &session) {
		return {"temporal_sample", [&universe, &session](Surface &surface) {
					surface.AddTemporalSampleTools(universe, session);
				}};
	}
	inline Spec RigExport(world::Universe &universe, world::DataFactorySession *session = nullptr) {
		return {"rig-export", [&universe, session](Surface &surface) {
					surface.AddRigExportTools(universe, session);
				}};
	}
	inline Spec VisibilityObservations(std::function<features::VisibilitySnapshotReply()> snapshot) {
		return {"visibility-observations", [snapshot = std::move(snapshot)](Surface &surface) {
					surface.Add(features::VisibilityObservationsTool(snapshot));
				}};
	}
}
