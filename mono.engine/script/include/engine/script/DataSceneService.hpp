#pragma once

// The VM-neutral data observation surface. It copies only scene facts that are
// already represented in the ECS; capture, checkpoint, and resource ownership
// remain at their respective boundaries.
// @tier L9 · shared

#include <engine/ecs/Entity.hpp>
#include <engine/script/Codec.hpp>

#include <cstddef>
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
	inline constexpr size_t MAX_DATA_SCENE_ID_BYTES = 256;

	DataSceneResult GetCapabilities(const ecs::Store &store);
	DataSceneResult GetSceneSnapshot(ecs::Store &store, size_t limit = MAX_DATA_SCENE_ENTITIES);
	DataSceneResult GetCameraRenderingData(const ecs::Store &store, ecs::Entity camera);
	DataSceneResult GetEditableImageMetadata(const ecs::Store &store, ecs::Entity image);
	DataSceneResult GetCaptureChannels(const ecs::Store &store);
	DataSceneResult
	GetCaptureChannels(const ecs::Store &store, const std::shared_ptr<DataCaptureBridge> &bridge);
	DataSceneResult GetResources(const ecs::Store &store);

	const ServiceSurface &DataSceneServiceSurface();
}
