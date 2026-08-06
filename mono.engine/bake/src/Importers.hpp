#pragma once

// The per-format entry points `Model.cpp` dispatches to.
//
// Private, for `Decoders.hpp`'s reason: which formats exist is `ModelFormat`'s
// business.

#include <engine/bake/Model.hpp>

#include <cstddef>
#include <span>
#include <string>

namespace engine::bake {

	// The ceiling every importer checks a declared count against.
	//
	// **One number rather than one per format**, because the question each of
	// them is asking is the same: is this count plausible for a mesh, or is it
	// four bytes an attacker chose. It matches `assets::Mesh::MAXIMUM_VERTICES`
	// deliberately — an importer that accepted more than the format can store
	// would do the whole parse and then fail at the write.
	constexpr uint64_t MAXIMUM_IMPORTED_VERTICES = 4u * 1024u * 1024u;

	// The same, for indices.
	constexpr uint64_t MAXIMUM_IMPORTED_INDICES = 16u * 1024u * 1024u;

	// Imports glTF 2.0, as `.glb` or as raw JSON.
	//
	// @param bytes   The file.
	// @param out     Filled on success, left alone on failure.
	// @param failure Set to why on failure.
	// @return `false` on anything malformed or unsupported.
	bool ReadGltf(std::span<const std::byte> bytes, ImportedModel &out, std::string &failure);

	// Imports a Wavefront OBJ.
	//
	// @param bytes   The file.
	// @param out     Filled on success, left alone on failure.
	// @param failure Set to why on failure.
	// @return `false` on anything malformed.
	bool ReadObj(std::span<const std::byte> bytes, ImportedModel &out, std::string &failure);

	// Imports a PMX model.
	//
	// @param bytes   The file.
	// @param out     Filled on success, left alone on failure.
	// @param failure Set to why on failure.
	// @return `false` on anything malformed or unsupported.
	bool ReadPmx(std::span<const std::byte> bytes, ImportedModel &out, std::string &failure);
}
