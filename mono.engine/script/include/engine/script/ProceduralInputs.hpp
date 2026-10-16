#pragma once

// ECS-owned road controls and sparse placement edits for procedural regions.
// @tier L9 · shared

#include <engine/procedural/Region.hpp>

#include <string_view>

namespace engine::ecs {
	class Store;
}

namespace engine::script {
	// Replaces a named road control after validating its source endpoints.
	bool SetProceduralSpline(
		ecs::Store &store, const procedural::AuthoredSpline &spline, procedural::Diagnostic &diagnostic
	);
	bool RemoveProceduralSpline(ecs::Store &store, std::string_view name, procedural::Diagnostic &diagnostic);

	// A replacement is complete: the stable target ID and all placement choices
	// are authored together. Delete has no replacement payload.
	bool SetProceduralOverride(
		ecs::Store &store,
		const procedural::PlacementOverride &overrideValue,
		procedural::Diagnostic &diagnostic
	);
	bool RemoveProceduralOverride(
		ecs::Store &store, std::string_view targetId, procedural::Diagnostic &diagnostic
	);
}
