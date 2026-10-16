#pragma once

// A local world's demand for one published procedural region. The client owns
// delivery; this ECS row owns which signed content the world wants to display.
// @tier L9 · shared

#include <engine/core/Name.hpp>
#include <engine/ecs/Entity.hpp>
#include <engine/procedural/Region.hpp>

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace engine::ecs {
	class Store;
}

namespace engine::script {
	inline constexpr size_t MAXIMUM_BAKED_PROCEDURAL_REFERENCES = 64;
	struct BakedProceduralReference {
		core::Name Description{};
		procedural::RegionScope Scope{};
		ecs::Entity Part{};
		uint64_t Revision = 0; // Each explicit demand advances, even for identical content.
	};

	// Keeps the desired description on the EditableMesh row. Part is a handle in
	// the same store, never a cross-world pointer or a delivery request ticket.
	bool SetBakedProceduralReference(
		ecs::Store &store,
		procedural::RegionScope scope,
		ecs::Entity mesh,
		ecs::Entity part,
		std::string_view descriptionName,
		procedural::Diagnostic &diagnostic
	);
	void RegisterBakedProceduralReferenceComponents();
}
