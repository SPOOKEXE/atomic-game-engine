#pragma once

#include <engine/assets/ContentPolicy.hpp>
#include <engine/bake/ImageGraph.hpp>
#include <engine/imagegraph/Document.hpp>

#include <cstddef>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>

namespace assetc {
	// Project and source files must remain inside the canonical allowed root.
	bool ReadImageGraphProject(
		const std::filesystem::path &allowedRoot,
		const std::filesystem::path &project,
		engine::imagegraph::Document &out,
		std::string &failure
	);
	bool ReadImageGraphSourceFile(
		const std::filesystem::path &allowedRoot,
		const std::filesystem::path &project,
		std::string_view reference,
		const engine::assets::ContentPolicy &policy,
		engine::imagegraph::Image &out,
		std::string &failure
	);
	// grug reads source inside root and keeps the node's colour/data choice.
	bool ReadImageGraphSourceFileTyped(
		const std::filesystem::path &allowedRoot,
		const std::filesystem::path &project,
		const engine::imagegraph::Source &source,
		const engine::assets::ContentPolicy &policy,
		engine::imagegraph::Image &out,
		std::string &failure
	);
	// same cooker for CLI and Studio. edited documents need not be saved first.
	bool CookImageGraphProject(
		const std::filesystem::path &allowedRoot,
		const std::filesystem::path &project,
		const engine::imagegraph::Document &document,
		std::string_view graphAssetName,
		const engine::assets::ContentPolicy &policy,
		engine::bake::CookedImageGraph &out,
		std::string &failure
	);
	// immutable dependencies first, graph last. refused writes keep old graph usable.
	bool PublishCookedImageGraph(
		const std::filesystem::path &outputRoot,
		const engine::bake::CookedImageGraph &cooked,
		std::string &failure
	);
	// Publishes complete bytes via a same-directory rename. Failure preserves the old file.
	bool PublishImageGraphTexture(
		const std::filesystem::path &path, std::span<const std::byte> bytes, std::string &failure
	);
}
