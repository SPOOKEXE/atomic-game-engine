#pragma once

// Canonical bytes for one generated region's owned semantic output.
// Runtime work counters, tickets and source revisions are not artifact identity.
// @tier L3 · shared

#include <engine/procedural/Region.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace engine::procedural {
	// Reads only the fixed RGK2 scope header; this does not authenticate its
	// recipe or source witnesses. Refusal leaves out untouched.
	bool ReadRegionKeyScope(std::span<const uint8_t> key, RegionScope &out);

	// Hard ceilings for one portable regional artifact.
	struct ArtifactLimits {
		size_t MaximumKeyBytes = 262144; // Regional key payload ceiling.
		size_t MaximumPlacements = 4096; // Placement records per artifact.
		size_t MaximumRoads = 256;		 // Road spline records per artifact.
		size_t MaximumNameBytes = 128;	 // UTF-8 bytes in one stable name.
		size_t MaximumBytes = 1048576;	 // Entire encoded artifact ceiling.
	};

	// Emits RGA1 with fixed-width little-endian fields, stable named kinds and
	// sorted unique identities. Refusal preserves out.
	Status EncodeRegionResult(
		const RegionResult &result,
		std::vector<uint8_t> &out,
		Diagnostic &diagnostic,
		const ArtifactLimits &limits = {}
	);

	// Accepts only canonical RGA1 bytes and optionally refuses a stale regional
	// key. Without expectedKey, the host can load a baked artifact without the
	// generator. GenerationFacts reset because they measure runtime work only.
	// Refusal preserves out.
	Status DecodeRegionResult(
		std::span<const uint8_t> bytes,
		RegionResult &out,
		Diagnostic &diagnostic,
		std::span<const uint8_t> expectedKey = {},
		const ArtifactLimits &limits = {}
	);
}
