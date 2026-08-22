// Walking up from the executable until something recognisable is underfoot.

#include "Repository.hpp"

#include <engine/core/Paths.hpp>

#include <system_error>

namespace engine::control {

	namespace {
		// The first directory at or above `from` for which `matches` is true.
		//
		// Bounded by `root_path` rather than by a hop count: a build tree three
		// directories deep and one eight deep are both ordinary, and a wrong
		// bound would fail on somebody's layout and nowhere else.
		template <class Predicate>
		std::filesystem::path WalkUp(std::filesystem::path from, Predicate matches) {
			std::error_code ignored;
			for (; !from.empty() && from != from.root_path(); from = from.parent_path()) {
				if (matches(from, ignored)) {
					return from;
				}
			}
			return {};
		}
	}

	const std::filesystem::path &RepositoryRoot() {
		static const std::filesystem::path root =
			WalkUp(core::Paths::Base(), [](const std::filesystem::path &at, std::error_code &error) {
				return std::filesystem::exists(at / "AGENTS.md", error) &&
					   std::filesystem::is_directory(at / "mono.engine", error);
			});
		return root;
	}

	const std::filesystem::path &BuildDirectory() {
		static const std::filesystem::path directory =
			WalkUp(core::Paths::Base(), [](const std::filesystem::path &at, std::error_code &error) {
				return std::filesystem::exists(at / "target-graph.json", error);
			});
		return directory;
	}
}
