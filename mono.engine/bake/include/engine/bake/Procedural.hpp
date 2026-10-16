#pragma once

// Pure conversion of regional geometry to ordinary mesh assets and a bounded
// placement description. Publication and filesystem access belong to the host.
// @tier L9 · shared

#include <engine/assets/ProceduralDescription.hpp>
#include <engine/procedural/Geometry.hpp>

#include <cstddef>
#include <string>
#include <vector>

namespace engine::bake {

	// One ordinary .amesh payload keyed by its content address. Equal generated
	// objects share this asset while keeping separate placement records.
	struct ProceduralMeshAsset {
		std::string Name{};				// Content-addressed asset name.
		std::vector<std::byte> Bytes{}; // assets::Mesh v2 bytes.
	};

	// One complete bake transaction, ordered by stable object identity.
	struct ProceduralBake {
		std::vector<ProceduralMeshAsset> Meshes{};
		std::vector<assets::ProceduralPlacement> Placements{};
		std::vector<std::byte> DescriptionBytes{}; // Canonical PRD1 placement bytes.
	};

	// Converts shared geometry objects to ordinary .amesh payloads, deduplicates
	// equal payloads, then writes a canonical bounded description. Refusal leaves out
	// untouched. The host verifies provenance and publishes at its own barrier.
	bool BakeProceduralGeometry(
		const procedural::RegionGeometry &geometry, ProceduralBake &out, std::string &failure
	);
}
