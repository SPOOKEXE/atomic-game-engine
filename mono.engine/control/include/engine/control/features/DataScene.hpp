#pragma once

// Registers bounded MCP scene observations, exports, and physics queries.
// @tier L13 · shared

#include <engine/control/Surface.hpp>
#include <engine/script/DataCaptureBridge.hpp>
#include <engine/script/DataSceneService.hpp>
#include <engine/script/GltfSceneExport.hpp>

#include <cstddef>
#include <memory>
#include <nlohmann/json_fwd.hpp>
#include <string>

namespace engine::world {
	class DataFactorySession;
	class Universe;
}

namespace engine::control {
	namespace data_scene_detail {
		// Maximum serialized JSON bytes returned by one data-scene call.
		inline constexpr size_t MAXIMUM_RESULT_BYTES = script::MAX_DATA_SCENE_JSON_RESPONSE_BYTES;

		// Converts a script return value into bounded JSON without lossy integer coercion.
		bool JsonValue(
			const script::ScriptValue &source, nlohmann::json &destination, size_t depth, size_t &bytes
		);
		// Converts a script data-scene result into a bounded host reply and propagates script failure text.
		nlohmann::json Result(const script::DataSceneResult &result, std::string &failure);
	}

	namespace features {
		// Builds the camera calibration row for a host that owns scene rendering.
		Tool CameraRenderingDataTool(world::Universe &universe, world::DataFactorySession *session = nullptr);

		// Registers read-only scene inspection, export, and bounded physics-query tools for one universe.
		Feature DataScene(
			world::Universe &universe,
			std::shared_ptr<script::DataCaptureBridge> bridge = {},
			world::DataFactorySession *session = nullptr,
			script::GltfMeshSource meshSource = {},
			script::GltfTextureSource textureSource = {}
		);
	}
}
