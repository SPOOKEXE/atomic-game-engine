#include "UniversePaths.hpp"

#include <engine/game/Game.hpp>

#include <algorithm>
#include <system_error>

namespace engine::game::detail {
	namespace {
		bool ResolveContainedReference(
			const std::filesystem::path &base,
			std::string_view text,
			std::filesystem::path &relative,
			std::filesystem::path &absolute,
			std::string &error
		) {
			relative = std::filesystem::path(text).lexically_normal();
			if (relative.empty() || relative.is_absolute() || relative.has_root_name()) {
				error = "invalid universe-relative path '" + std::string(text) + "'";
				return false;
			}
			for (const std::filesystem::path &part : relative) {
				if (part == "..") {
					error = "universe-relative path leaves the universe directory";
					return false;
				}
			}

			absolute = base / relative;
			std::filesystem::path current = base;
			for (const std::filesystem::path &part : relative) {
				current /= part;
				std::error_code statusError;
				const std::filesystem::file_status status =
					std::filesystem::symlink_status(current, statusError);
				if (!statusError && std::filesystem::is_symlink(status)) {
					error = "universe-relative path crosses a symbolic link";
					return false;
				}
			}
			return true;
		}
	}

	std::string SafeWorldFileStem(core::Name name) {
		std::string stem;
		if (name.IsValid()) {
			for (const unsigned char character : std::string_view(name.Text())) {
				const bool letter =
					(character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z');
				const bool digit = character >= '0' && character <= '9';
				stem.push_back(
					letter || digit || character == '-' || character == '_' ? static_cast<char>(character)
																			: '_'
				);
			}
		}
		return stem.empty() ? "World" : stem;
	}

	bool ResolveWorldReference(
		const std::filesystem::path &base,
		std::string_view text,
		std::filesystem::path &relative,
		std::filesystem::path &absolute,
		std::string &error
	) {
		if (!ResolveContainedReference(base, text, relative, absolute, error)) {
			return false;
		}
		if (relative.extension() != WORLD_EXTENSION) {
			error = "invalid world reference '" + std::string(text) + "'";
			return false;
		}
		return true;
	}

	bool ResolveDirectoryReference(
		const std::filesystem::path &base,
		std::string_view text,
		std::filesystem::path &relative,
		std::filesystem::path &absolute,
		std::string &error
	) {
		return ResolveContainedReference(base, text, relative, absolute, error);
	}

	bool DiscoverWorldReferences(
		const std::filesystem::path &base, std::vector<std::filesystem::path> &discovered, std::string &error
	) {
		discovered.clear();
		std::error_code statusError;
		if (std::filesystem::is_symlink(std::filesystem::symlink_status(base, statusError))) {
			error = "recursive world discovery will not walk a symbolic link";
			return false;
		}

		std::error_code walkError;
		std::filesystem::recursive_directory_iterator iterator(
			base, std::filesystem::directory_options::none, walkError
		);
		if (walkError) {
			error = "could not scan the universe directory";
			return false;
		}

		const std::filesystem::recursive_directory_iterator end;
		while (iterator != end) {
			const std::filesystem::directory_entry &entry = *iterator;
			std::error_code entryError;
			const std::filesystem::file_status status = entry.symlink_status(entryError);
			if (entryError) {
				error = "could not inspect a universe directory entry";
				return false;
			}
			if (std::filesystem::is_symlink(status)) {
				iterator.disable_recursion_pending();
			} else if (std::filesystem::is_regular_file(status) &&
					   entry.path().extension() == WORLD_EXTENSION) {
				discovered.push_back(entry.path().lexically_relative(base));
			}

			iterator.increment(walkError);
			if (walkError) {
				error = "could not finish scanning the universe directory";
				return false;
			}
		}

		std::sort(discovered.begin(), discovered.end());
		return true;
	}
}
