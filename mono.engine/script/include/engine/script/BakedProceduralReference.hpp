#pragma once

// A local world's demand for one published procedural region. The client owns
// delivery; this ECS row owns which signed content the world wants to display.
// @tier L9 · shared

#include <engine/core/Name.hpp>
#include <engine/ecs/Entity.hpp>
#include <engine/procedural/Region.hpp>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <string_view>
#include <vector>

namespace engine::ecs {
	class Store;
}

namespace engine::script {
	// Maximum number of per-world published-region demands retained by the client.
	inline constexpr size_t MAXIMUM_BAKED_PROCEDURAL_REFERENCES = 64;
	// Bounds one authored document packet, including names and document-local IDs.
	inline constexpr size_t MAXIMUM_BAKED_PROCEDURAL_DOCUMENT_BYTES =
		8 + MAXIMUM_BAKED_PROCEDURAL_REFERENCES * (512 + 64);
	// ECS-owned demand for one signed description in a local world.
	struct BakedProceduralReference {
		// Stable name of the signed procedural description requested for installation.
		core::Name Description{};
		// Region scope whose generated scene output is requested.
		procedural::RegionScope Scope{};
		// Mesh entity whose visible output receives the delivered placements.
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
	// Encodes authored demands in document-ID order. Unmapped meshes and exact
	// default rows are omitted; no configured demands produces an empty packet.
	bool EncodeBakedProceduralReferences(
		const ecs::Store &store,
		const std::function<uint32_t(ecs::Entity)> &documentId,
		std::vector<uint8_t> &out,
		procedural::Diagnostic &diagnostic
	);
	// Validates and remaps a complete packet before restoring demands and fresh
	// local mesh bindings. Refusal preserves every active demand and visual.
	bool DecodeBakedProceduralReferences(
		ecs::Store &store,
		std::span<const uint8_t> bytes,
		const std::function<ecs::Entity(uint32_t)> &instance,
		procedural::Diagnostic &diagnostic
	);
	// Registers the ECS component used to store baked procedural demands.
	void RegisterBakedProceduralReferenceComponents();
}
