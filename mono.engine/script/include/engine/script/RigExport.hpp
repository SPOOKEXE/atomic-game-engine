#pragma once

// A bounded, VM-neutral observation of skeleton rows in data-rig/v1 form.
// @tier L9 · shared

#include <engine/script/Codec.hpp>

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace engine::ecs {
	class Store;
}

namespace engine::script {
	inline constexpr size_t MAX_RIG_EXPORT_ENTITIES = 256;
	inline constexpr size_t MAX_RIG_EXPORT_BONES = 1024;
	inline constexpr size_t MAX_RIG_EXPORT_TOTAL_BONES = 1024;
	inline constexpr size_t MAX_RIG_EXPORT_KEYPOINTS = 1024;
	inline constexpr size_t MAX_RIG_EXPORT_TOTAL_KEYPOINTS = 1024;
	inline constexpr size_t MAX_RIG_EXPORT_ID_BYTES = 512;
	inline constexpr size_t MAX_RIG_EXPORT_ENTITY_ID_BYTES = 500;
	// Animation buffers can carry the asset format's multi-million-key limits.
	// Keep the observation below the data-rig transport budget instead.
	inline constexpr size_t MAX_RIG_EXPORT_CLIPS_PER_ENTITY = 64;
	inline constexpr size_t MAX_RIG_EXPORT_CHANNELS_PER_CLIP = 128;
	inline constexpr size_t MAX_RIG_EXPORT_KEYS_PER_CHANNEL = 4096;
	inline constexpr size_t MAX_RIG_EXPORT_TOTAL_ANIMATION_KEYS = 16384;
	inline constexpr size_t MAX_RIG_EXPORT_ANIMATION_BYTES = 1024u * 1024u;

	struct RigExportResult {
		const char *Status = "ok";
		ScriptValue Value;
	};

	// Copies skeleton facts whose instances carry DataFactoryId. Selection is by
	// those stable IDs; an empty selection asks for every identified skeleton.
	RigExportResult GetRigExport(
		ecs::Store &store,
		std::string_view exportId,
		const std::vector<std::string> &entityIds = {},
		size_t limit = MAX_RIG_EXPORT_ENTITIES
	);
}
