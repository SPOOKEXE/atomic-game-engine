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
		// Machine-readable operation status.
		const char *Status = "ok";
		// VM-neutral serialized value.
		ScriptValue Value;
	};

	// The explicit string attribute an exported entity must carry. Entity handles
	// are process-local, so no snapshot uses them as an identity.
	inline constexpr std::string_view DATA_SCENE_ID_ATTRIBUTE = "DataFactoryId";
	// Maximum entities emitted by one scene snapshot.
	inline constexpr size_t MAX_DATA_SCENE_ENTITIES = 10'000;
	// Maximum objects included in one camera rendering observation.
	inline constexpr size_t MAX_CAMERA_OBJECT_OBSERVATIONS = 64;
	// Maximum UTF-8 byte length of an exported DataFactoryId.
	inline constexpr size_t MAX_DATA_SCENE_ID_BYTES = 256;
	// Maximum canonical narrative records retained by one world.
	inline constexpr size_t MAX_EVENT_NARRATIVES = 256;
	// Maximum enabled authored affordances returned by one query.
	inline constexpr size_t MAX_AUTHORED_AFFORDANCES = 256;
	// The conservative compact-JSON budget shared by the renderer and the MCP
	// adapter. A sidecar admitted against this bound is representable by the
	// adapter without changing its response shape.
	inline constexpr size_t MAX_DATA_SCENE_JSON_RESPONSE_BYTES = 64u * 1024u;

	// Typed query requests shared by scripts and thin control-surface adapters.
	// Directions and rotations are normalized at this boundary before physics sees them.
	struct DataSceneRaycastRequest {
		// World-space query origin.
		core::Vector3 Origin;
		// Normalized world-space query direction.
		core::Vector3 Direction;
		// Furthest ray hit distance in metres.
		float MaxDistanceMetres = 0;
	};
	// Axis-aligned overlap bounds in world coordinates.
	struct DataSceneAabbRequest {
		// Inclusive query minimum.
		core::Vector3 Minimum;
		// Inclusive query maximum.
		core::Vector3 Maximum;
	};
	// Oriented overlap bounds in world coordinates.
	struct DataSceneObbRequest {
		// World-space transform.
		core::CFrame Frame;
		// Local half-extents measured from Frame's origin, in metres.
		core::Vector3 HalfExtent;
	};

	// A fixed-height bird's-eye grid over world-space collider contact. Rows run
	// from minimum Z to maximum Z; columns run from minimum X to maximum X.
	struct DataSceneColliderBevRequest {
		// Inclusive west edge of the grid, in world metres.
		float MinimumXMetres = 0.0f;
		// Inclusive north edge of the grid, in world metres.
		float MinimumZMetres = 0.0f;
		// Exclusive east edge of the grid, in world metres.
		float MaximumXMetres = 0.0f;
		// Exclusive south edge of the grid, in world metres.
		float MaximumZMetres = 0.0f;
		// Lowest sampled height, in world metres.
		float MinimumYMetres = 0.0f;
		// Highest sampled height, in world metres.
		float MaximumYMetres = 0.0f;
		// Grid row count.
		uint8_t Rows = 0;
		// Grid column count.
		uint8_t Columns = 0;
	};

	// A bounded world-space voxel grid. A cell is filled only when one analytic
	// collider contains its complete AABB, never merely because it touches it.
	struct DataSceneFilledOccupancyRequest {
		// Inclusive lower world-space corner, in metres.
		core::Vector3 MinimumMetres;
		// Exclusive upper world-space corner, in metres.
		core::Vector3 MaximumMetres;
		// Grid column count.
		uint8_t Columns = 0;
		// Grid row count.
		uint8_t Rows = 0;
		// Grid layer count.
		uint8_t Layers = 0;
	};

	// Samples are taken at voxel centres in deterministic y, z, x order. An
	// unavailable sample carries a reason instead of a guessed distance.
	struct DataSceneSignedDistanceFieldRequest {
		// Inclusive lower world-space corner, in metres.
		core::Vector3 MinimumMetres;
		// Exclusive upper world-space corner, in metres.
		core::Vector3 MaximumMetres;
		// Grid column count.
		uint8_t Columns = 0;
		// Grid row count.
		uint8_t Rows = 0;
		// Grid layer count.
		uint8_t Layers = 0;
	};

	// A bounded path request over explicit authored walkable surfaces. Endpoint
	// heights may differ from their supporting polygon by this tolerance only.
	struct DataSceneNavmeshPathRequest {
		// Requested path start in world metres.
		core::Vector3 StartMetres;
		// Requested path endpoint in world metres.
		core::Vector3 GoalMetres;
		// Maximum vertical distance between an endpoint and its supporting polygon.
		float VerticalToleranceMetres = 0.25f;
	};

	// Returns scene-query capabilities available from this store build.
	DataSceneResult GetCapabilities(const ecs::Store &store);
	// Computes a conservative upper bound for the compact JSON representation
	// used by the MCP adapter. Rejects values the adapter cannot serialize.
	bool DataSceneJsonResponseBudget(const ScriptValue &value, size_t &bytes);
	// Serializes up to limit exported entities into the VM-neutral scene schema.
	DataSceneResult GetSceneSnapshot(ecs::Store &store, size_t limit = MAX_DATA_SCENE_ENTITIES);
	// Returns the bounded rendering observation for one camera entity.
	DataSceneResult
	GetCameraRenderingData(ecs::Store &store, ecs::Entity camera, size_t observationLimit = 0);
	// Returns editable-image dimensions, revision, and packing metadata.
	DataSceneResult GetEditableImageMetadata(const ecs::Store &store, ecs::Entity image);
	// Returns capture channel names declared by the scene without a host bridge.
	DataSceneResult GetCaptureChannels(const ecs::Store &store);
	// Intersects scene channels with those supported by the installed capture bridge.
	DataSceneResult
	GetCaptureChannels(const ecs::Store &store, const std::shared_ptr<DataCaptureBridge> &bridge);
	// Lists snapshot-visible resources using stable names rather than entity handles.
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
	// Validates, canonicalizes, and stores a bounded event-narrative bundle.
	DataSceneResult SetEventNarratives(ecs::Store &store, const ScriptValue &bundle);
	// Returns the canonical event-narrative bundle retained by the world.
	DataSceneResult GetEventNarratives(const ecs::Store &store);
	// Returns only explicit, enabled affordance components in stable id order.
	DataSceneResult GetAuthoredAffordances(ecs::Store &store, size_t limit = MAX_AUTHORED_AFFORDANCES);

	// Query prepared collider geometry and return only stable authored identities.
	DataSceneResult Raycast(const ecs::Store &store, const DataSceneRaycastRequest &request);
	// Finds stable authored identities whose prepared colliders overlap these AABB bounds.
	DataSceneResult OverlapAABB(const ecs::Store &store, const DataSceneAabbRequest &request);
	// Finds stable authored identities whose prepared colliders overlap these OBB bounds.
	DataSceneResult OverlapOBB(const ecs::Store &store, const DataSceneObbRequest &request);

	// Rasterizes prepared collider contact into the requested bird's-eye grid.
	DataSceneResult ColliderBev(ecs::Store &store, const DataSceneColliderBevRequest &request);
	// Marks voxels fully contained by an analytic collider in the requested grid.
	DataSceneResult FilledOccupancy(ecs::Store &store, const DataSceneFilledOccupancyRequest &request);
	// Computes a signed distance field.
	DataSceneResult
	SignedDistanceField(ecs::Store &store, const DataSceneSignedDistanceFieldRequest &request);
	// Finds a path over explicit authored walkable surfaces between the endpoints.
	DataSceneResult FindAuthoredNavmeshPath(ecs::Store &store, const DataSceneNavmeshPathRequest &request);

	// Returns the VM-neutral method table exposed by the data-scene service.
	const ServiceSurface &DataSceneServiceSurface();
}
