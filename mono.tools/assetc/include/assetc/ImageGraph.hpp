#pragma once

#include <engine/assets/ContentPolicy.hpp>
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
	// Publishes complete bytes via a same-directory rename. Failure preserves the old file.
	bool PublishImageGraphTexture(
		const std::filesystem::path &path, std::span<const std::byte> bytes, std::string &failure
	);
}
