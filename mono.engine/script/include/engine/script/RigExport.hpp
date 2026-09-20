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
	// Maximum scene entities represented in one rig-export reply.
	inline constexpr size_t MAX_RIG_EXPORT_ENTITIES = 256;
	// Maximum bones retained for one exported entity.
	inline constexpr size_t MAX_RIG_EXPORT_BONES = 1024;
	// Maximum bones retained across the complete export.
	inline constexpr size_t MAX_RIG_EXPORT_TOTAL_BONES = 1024;
	// Maximum semantic keypoints retained for one exported entity.
	inline constexpr size_t MAX_RIG_EXPORT_KEYPOINTS = 1024;
	// Maximum semantic keypoints retained across the complete export.
	inline constexpr size_t MAX_RIG_EXPORT_TOTAL_KEYPOINTS = 1024;
	// Four influence records carry stable joint IDs. 1024 vertices leaves room
	// for their worst-case 512-byte IDs under the control surface's 4 MiB cap.
	inline constexpr size_t MAX_RIG_EXPORT_SKIN_VERTICES = 1024;
	// Maximum skinning vertices retained across the complete export.
	inline constexpr size_t MAX_RIG_EXPORT_TOTAL_SKIN_VERTICES = 1024;
	// Maximum UTF-8 bytes in one stable joint or keypoint id.
	inline constexpr size_t MAX_RIG_EXPORT_ID_BYTES = 512;
	// Maximum UTF-8 bytes in one exported entity id.
	inline constexpr size_t MAX_RIG_EXPORT_ENTITY_ID_BYTES = 500;
	// Animation buffers can carry the asset format's multi-million-key limits.
	// Keep the observation below the data-rig transport budget instead.
	inline constexpr size_t MAX_RIG_EXPORT_CLIPS_PER_ENTITY = 64;
	// Maximum animated transform channels retained for one clip.
	inline constexpr size_t MAX_RIG_EXPORT_CHANNELS_PER_CLIP = 128;
	// Maximum keyframes retained for one animated channel.
	inline constexpr size_t MAX_RIG_EXPORT_KEYS_PER_CHANNEL = 4096;
	// Maximum keyframes retained across all exported clips.
	inline constexpr size_t MAX_RIG_EXPORT_TOTAL_ANIMATION_KEYS = 16384;
	// Maximum encoded animation bytes retained in one reply.
	inline constexpr size_t MAX_RIG_EXPORT_ANIMATION_BYTES = 1024u * 1024u;

	// Bounded script-visible result of a rig capture request.
	struct RigExportResult {
		// Stable operation outcome consumed by control adapters.
		const char *Status = "ok";
		// Captured rig tree encoded as a VM-neutral script value.
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
