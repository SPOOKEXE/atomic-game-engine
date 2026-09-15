#pragma once

// Explicit gameplay meaning an author assigns to a part.
//
// @tier L7 · shared

#include <engine/core/Name.hpp>

#include <cstdint>

namespace engine::scene {
	// The finite semantics the data-scene export may report. A collider, mesh,
	// or surface never implies one of these meanings.
	enum class AuthoredAffordanceKind : uint8_t { None, Walkable, Climbable, Interactable, Cover };

	// One authored affordance record on a BasePart.
	struct AuthoredAffordance {
		core::Name Id;
		AuthoredAffordanceKind Kind = AuthoredAffordanceKind::None;
		bool Enabled = false;
		uint8_t Reserved[2]{};
	};
}
