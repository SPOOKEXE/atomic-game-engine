#pragma once

#include <engine/script/Codec.hpp>

#include <cstdint>

namespace engine::script {
	// Version required by the canonical event-narrative bundle stored in a world.
	inline constexpr uint32_t EVENT_NARRATIVE_SCHEMA_VERSION = 1;

	// The ECS serialization sweep writes a default-constructed resource, so its
	// default must already be a valid v1 bundle rather than an untyped nil.
	inline ScriptValue EmptyEventNarrativeBundle() {
		ScriptValue version(ValueTag::Number);
		version.Number = EVENT_NARRATIVE_SCHEMA_VERSION;
		ScriptValue records(ValueTag::Array);
		ScriptValue bundle(ValueTag::Map);
		bundle.Entries = {{"version", std::move(version)}, {"records", std::move(records)}};
		return bundle;
	}

	// Script-declared narrative facts retained per world. The payload remains a
	// VM-neutral ScriptValue after validation, so both language adapters and the
	// MCP boundary observe the same canonical record shape.
	struct EventNarratives {
		// Validated versioned map of narrative records retained with this world.
		ScriptValue Bundle = EmptyEventNarrativeBundle();
	};
}
