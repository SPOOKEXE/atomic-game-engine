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
	inline constexpr size_t MAX_RIG_EXPORT_ID_BYTES = 512;
	inline constexpr size_t MAX_RIG_EXPORT_ENTITY_ID_BYTES = 500;

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
