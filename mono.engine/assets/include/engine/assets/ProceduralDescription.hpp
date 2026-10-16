#pragma once

// Canonical placement description for ordinary procedural mesh assets.
// A runtime decodes this format without loading the generator or a baker.
// @tier L8 · shared

#include <engine/procedural/Region.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace engine::assets {
	inline constexpr size_t MAXIMUM_PROCEDURAL_DESCRIPTION_BYTES = 1024 * 1024;
	struct MeshData;

	// One stable placement referencing an ordinary, content-addressed .amesh.
	struct ProceduralPlacement {
		std::string Id{};			   // Stable generated object identity.
		std::string Kind{};			   // Named geometry category.
		std::string MeshName{};		   // Corresponding ordinary asset name.
		procedural::Point Origin{};	   // Exact global fixed-point X/Z origin.
		uint16_t Material = 0;		   // Generator material choice.
		uint16_t Variant = 0;		   // Generator variant choice.
		std::array<float, 3> Centre{}; // Object-local mesh bounds centre.
		std::array<float, 3> Size{};   // Object-local mesh bounds size.
		std::array<float, 4> Colour{}; // Linear RGBA applied once as MeshPart tint.
	};

	// Emits bounded, ordered PRD1 bytes. Refusal leaves out untouched.
	bool EncodeProceduralDescription(
		std::span<const ProceduralPlacement> placements, std::vector<std::byte> &out, std::string &failure
	);

	// Strictly decodes canonical PRD1 bytes. Refusal leaves out untouched.
	// The caller obtains each referenced .amesh through signed delivery.
	bool DecodeProceduralDescription(
		std::span<const std::byte> bytes, std::vector<ProceduralPlacement> &out, std::string &failure
	);

	// Verifies that an ordinary mesh has the exact local frame and single white,
	// unskinned run that a PRD1 placement describes. The asset name's hash and
	// signed delivery provenance are checked by the caller, which owns the bytes.
	bool ValidateProceduralPlacementMesh(
		const ProceduralPlacement &placement, const MeshData &mesh, std::string &failure
	);
}
