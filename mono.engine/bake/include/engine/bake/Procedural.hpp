#pragma once

// Pure conversion of regional geometry to ordinary mesh assets and a bounded
// placement description. Publication and filesystem access belong to the host.
// @tier L9 · shared

#include <engine/procedural/Geometry.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace engine::bake {

	// One ordinary .amesh payload keyed by its content address. Equal generated
	// objects share this asset while keeping separate placement records.
	struct ProceduralMeshAsset {
		std::string Name{};				// Content-addressed asset name.
		std::vector<std::byte> Bytes{}; // assets::Mesh v2 bytes.
	};

	// Enough metadata to instantiate an ordinary MeshPart after loading the
	// named .amesh, without retaining or running the procedural generator.
	struct ProceduralPlacement {
		std::string Id{};			   // Stable generated object identity.
		std::string Kind{};			   // Stable named geometry category.
		std::string MeshName{};		   // Corresponding ordinary asset name.
		procedural::Point Origin{};	   // Exact global fixed-point X/Z origin.
		uint16_t Material = 0;		   // Generator material choice.
		uint16_t Variant = 0;		   // Generator variant choice.
		std::array<float, 3> Centre{}; // Object-local mesh bounds centre.
		std::array<float, 3> Size{};   // Object-local mesh bounds size.
		std::array<float, 4> Colour{}; // Linear RGBA applied once as MeshPart tint.
	};

	// One complete bake transaction, ordered by stable object identity.
	struct ProceduralBake {
		std::vector<ProceduralMeshAsset> Meshes{};
		std::vector<ProceduralPlacement> Placements{};
		std::vector<std::byte> DescriptionBytes{}; // Canonical PRD1 placement bytes.
	};

	// Converts shared geometry objects to ordinary .amesh payloads, deduplicates
	// equal payloads, then writes a canonical bounded description. Refusal leaves out
	// untouched. The host verifies provenance and publishes at its own barrier.
	bool BakeProceduralGeometry(
		const procedural::RegionGeometry &geometry, ProceduralBake &out, std::string &failure
	);

	// Strictly decodes canonical PRD1 bytes without invoking any generator.
	// Refusal leaves out untouched. Mesh bytes are loaded separately by
	// assets::Mesh::Read under their content-addressed names.
	bool DecodeProceduralDescription(
		std::span<const std::byte> bytes, std::vector<ProceduralPlacement> &out, std::string &failure
	);
}
