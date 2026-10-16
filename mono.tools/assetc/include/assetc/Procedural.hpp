#pragma once

// Filesystem adapter for accepted canonical regions and ordinary baked assets.
// Assetc validates the bytes and scope; only the ECS host can attest that a
// particular RGA1 artifact was accepted for publication.

#include <engine/bake/Procedural.hpp>
#include <engine/procedural/Artifact.hpp>

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace assetc {
	// Pure in-memory cook shared by the CLI and Studio's accepted-publication path.
	// Refusal leaves out untouched. This never invokes the generator.
	bool CookProceduralArtifact(
		std::span<const uint8_t> artifactBytes,
		engine::procedural::RegionScope expectedScope,
		engine::bake::ProceduralBake &out,
		std::string &failure
	);

	// Reads at most one canonical artifact from inside inputRoot. A source symlink
	// escaping that root is refused. Refusal leaves out untouched.
	bool ReadProceduralArtifactFile(
		const std::filesystem::path &inputRoot,
		std::string_view sourceName,
		std::vector<uint8_t> &out,
		std::string &failure
	);

	// Confines every output under outputRoot and checks the complete mesh closure.
	// Immutable content-addressed meshes publish first; the PRD1 description is
	// swapped last, so a refusal never replaces the previous live description.
	bool PublishProceduralBake(
		const std::filesystem::path &outputRoot,
		std::string_view descriptionName,
		const engine::bake::ProceduralBake &baked,
		std::string &failure
	);
}
