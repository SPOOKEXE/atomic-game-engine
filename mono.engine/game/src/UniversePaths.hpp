#pragma once

// Filesystem boundaries for multi-file universe documents.

#include <engine/core/Name.hpp>

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace engine::game::detail {

	std::string SafeWorldFileStem(core::Name name);

	bool ResolveWorldReference(
		const std::filesystem::path &base,
		std::string_view text,
		std::filesystem::path &relative,
		std::filesystem::path &absolute,
		std::string &error
	);

	bool ResolveDirectoryReference(
		const std::filesystem::path &base,
		std::string_view text,
		std::filesystem::path &relative,
		std::filesystem::path &absolute,
		std::string &error
	);

	bool DiscoverWorldReferences(
		const std::filesystem::path &base, std::vector<std::filesystem::path> &discovered, std::string &error
	);
}
