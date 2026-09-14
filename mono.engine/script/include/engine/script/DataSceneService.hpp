#pragma once

// The VM-neutral data observation surface. It copies only scene facts that are
// already represented in the ECS; capture, checkpoint, and resource ownership
// remain at their respective boundaries.
// @tier L9 · shared

#include <engine/core/types/CFrame.hpp>
#include <engine/core/types/Vector3.hpp>
#include <engine/ecs/Entity.hpp>
#include <engine/script/Codec.hpp>
#include <engine/script/DataAudioObservationBridge.hpp>
#include <engine/script/EventNarratives.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string_view>

namespace engine::ecs {
	class Store;
}

namespace engine::script {
	class DataCaptureBridge;
	struct ServiceSurface;

	// A result has a machine-readable status because an unsupported observation
	// is useful data to a factory and must not look like an empty observation.
	struct DataSceneResult {
		const char *Status = "ok";
		ScriptValue Value;
	};

	// The explicit string attribute an exported entity must carry. Entity handles
	// are process-local, so no snapshot uses them as an identity.
	inline constexpr std::string_view DATA_SCENE_ID_ATTRIBUTE = "DataFactoryId";
	inline constexpr size_t MAX_DATA_SCENE_ENTITIES = 10'000;
	inline constexpr size_t MAX_CAMERA_OBJECT_OBSERVATIONS = 64;
	inline constexpr size_t MAX_DATA_SCENE_ID_BYTES = 256;
	inline constexpr size_t MAX_EVENT_NARRATIVES = 256;
	// The conservative compact-JSON budget shared by the renderer and the MCP
	// adapter. A sidecar admitted against this bound is representable by the
	// adapter without changing its response shape.
	inline constexpr size_t MAX_DATA_SCENE_JSON_RESPONSE_BYTES = 64u * 1024u;

	// Typed query requests shared by scripts and thin control-surface adapters.
	// Directions and rotations are normalized at this boundary before physics sees them.
	struct DataSceneRaycastRequest {
		core::Vector3 Origin;
		core::Vector3 Direction;
		float MaxDistanceMetres = 0;
	};
	struct DataSceneAabbRequest {
		core::Vector3 Minimum;
		core::Vector3 Maximum;
	};
	struct DataSceneObbRequest {
		core::CFrame Frame;
		core::Vector3 HalfExtent;
	};

	// A fixed-height bird's-eye grid over world-space collider contact. Rows run
	// from minimum Z to maximum Z; columns run from minimum X to maximum X.
	struct DataSceneColliderBevRequest {
		float MinimumXMetres = 0.0f;
		float MinimumZMetres = 0.0f;
		float MaximumXMetres = 0.0f;
		float MaximumZMetres = 0.0f;
		float MinimumYMetres = 0.0f;
		float MaximumYMetres = 0.0f;
		uint8_t Rows = 0;
		uint8_t Columns = 0;
	};

	// A bounded world-space voxel grid. A cell is filled only when one analytic
	// collider contains its complete AABB, never merely because it touches it.
	struct DataSceneFilledOccupancyRequest {
		core::Vector3 MinimumMetres;
		core::Vector3 MaximumMetres;
		uint8_t Columns = 0;
		uint8_t Rows = 0;
		uint8_t Layers = 0;
	};

	DataSceneResult GetCapabilities(const ecs::Store &store);
	// Computes a conservative upper bound for the compact JSON representation
	// used by the MCP adapter. Rejects values the adapter cannot serialize.
	bool DataSceneJsonResponseBudget(const ScriptValue &value, size_t &bytes);
	DataSceneResult GetSceneSnapshot(ecs::Store &store, size_t limit = MAX_DATA_SCENE_ENTITIES);
	DataSceneResult
	GetCameraRenderingData(ecs::Store &store, ecs::Entity camera, size_t observationLimit = 0);
	DataSceneResult GetEditableImageMetadata(const ecs::Store &store, ecs::Entity image);
	DataSceneResult GetCaptureChannels(const ecs::Store &store);
	DataSceneResult
	GetCaptureChannels(const ecs::Store &store, const std::shared_ptr<DataCaptureBridge> &bridge);
	DataSceneResult GetResources(const ecs::Store &store);
	// Audio capture is host-owned. This only describes whether an installed
	// copied-record bridge can supply audio_observation/v1 records.
	DataSceneResult
	GetAudioObservationCapabilities(const std::shared_ptr<DataAudioObservationBridge> &bridge);
	// Validate and canonicalize a bundle before script, MCP, or snapshot code retains it.
	// @param bundle Script-declared narrative data.
	// @param canonical Receives the bounded canonical bundle on success.
	// @return `true` when the bundle is valid for the current schema.
	bool CanonicalEventNarratives(const ScriptValue &bundle, ScriptValue &canonical);
	DataSceneResult SetEventNarratives(ecs::Store &store, const ScriptValue &bundle);
	DataSceneResult GetEventNarratives(const ecs::Store &store);

	// Query prepared collider geometry and return only stable authored identities.
	DataSceneResult Raycast(const ecs::Store &store, const DataSceneRaycastRequest &request);
	DataSceneResult OverlapAABB(const ecs::Store &store, const DataSceneAabbRequest &request);
	DataSceneResult OverlapOBB(const ecs::Store &store, const DataSceneObbRequest &request);

	DataSceneResult ColliderBev(ecs::Store &store, const DataSceneColliderBevRequest &request);
	DataSceneResult FilledOccupancy(ecs::Store &store, const DataSceneFilledOccupancyRequest &request);

	const ServiceSurface &DataSceneServiceSurface();
}
