#pragma once

// Generator-free installation of a delivered PRD1 description and ordinary
// content-addressed .amesh payloads into an existing regional scene target.
// @tier L9 · shared

#include <engine/assets/ContentHash.hpp>
#include <engine/script/ProceduralScene.hpp>

#include <cstddef>
#include <span>
#include <string_view>

namespace engine::script {
	// Borrowed delivered bytes. The call verifies their content names and copies
	// validated geometry before returning; no pointer enters the world.
	struct BakedProceduralMesh {
		std::string_view Name{};
		std::span<const std::byte> Bytes{};
	};
	struct BakedProceduralRegion {
		ProceduralSceneTarget Target{};
		assets::ContentHash DescriptionHash{};
		std::span<const std::byte> DescriptionBytes{};
		std::span<const BakedProceduralMesh> Meshes{};
	};

	// Stages every region and commits all visible targets at one barrier.
	bool InstallBakedProceduralSceneBatch(
		ecs::Store &store, std::span<const BakedProceduralRegion> regions, procedural::Diagnostic &diagnostic
	);

	// Requires the exact mesh closure named by the canonical description. The
	// existing target and its last visible output survive every refusal. Neither
	// recipe authority nor the procedural generator is needed by this path.
	bool InstallBakedProceduralScene(
		ecs::Store &store,
		const ProceduralSceneTarget &target,
		assets::ContentHash expectedDescriptionHash,
		std::span<const std::byte> descriptionBytes,
		std::span<const BakedProceduralMesh> meshes,
		procedural::Diagnostic &diagnostic
	);
}
