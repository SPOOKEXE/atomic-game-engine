#include <engine/core/Log.hpp>
#include <engine/core/Metrics.hpp>
#include <engine/core/Profiling.hpp>

#include <cdn/ContentRoot.hpp>
#include <optional>
#include <system_error>
#include <utility>

namespace cdn {

	namespace {
		// The decision itself, with no counting and no span around it.
		//
		// Split out so that Resolve has exactly one place that records what it
		// decided. Counting at each `return std::nullopt` was the alternative,
		// and a refusal path added later is a refusal path that forgets.
		std::optional<std::filesystem::path>
		Permit(const std::filesystem::path &base, std::string_view name) {
			if (name.empty()) {
				return std::nullopt;
			}

			const std::filesystem::path requested(name);
			if (requested.is_absolute() || requested.has_root_name() || requested.has_root_directory()) {
				return std::nullopt;
			}

			// Component by component, before the filesystem is touched at all.
			// `..` is the obvious one. `.` is refused with it so that a name has
			// exactly one spelling - `a/./b` and `a/b` naming one file is two
			// keys for one piece of content, and the manifest keys on the name.
			for (const auto &component : requested) {
				if (component == ".." || component == ".") {
					return std::nullopt;
				}
			}

			std::error_code failure;
			// weakly_canonical rather than canonical: a name that does not exist
			// yet still has to be resolvable, because deciding whether it *may*
			// be served is a separate question from whether it is there.
			// Symlinks in the part that does exist are still followed, which is
			// the half the component check above cannot do.
			const std::filesystem::path candidate =
				std::filesystem::weakly_canonical(base / requested, failure);
			if (failure) {
				return std::nullopt;
			}

			// lexically_relative gives an empty path when there is no route at
			// all, "." when the two are the same, and a path opening with ".."
			// when the candidate sits outside. All three are refusals, and the
			// root itself is a refusal because a directory is not content.
			const std::filesystem::path relative = candidate.lexically_relative(base);
			if (relative.empty() || relative == "." || *relative.begin() == "..") {
				return std::nullopt;
			}

			return candidate;
		}
	}

	ContentRoot::ContentRoot(std::filesystem::path directory) : Base(std::move(directory)) {}

	std::optional<ContentRoot> ContentRoot::Mount(const std::filesystem::path &directory) {
		ENGINE_PROFILE_CAT("ContentRoot::Mount", engine::core::ProfileCategory::Assets);

		if (directory.empty()) {
			ENGINE_ERROR("cdn: no content root given");
			return std::nullopt;
		}

		// The error_code overloads throughout. A missing root is a configuration
		// mistake worth naming, and an exception crossing this boundary would
		// reach a main that can only report it as text anyway.
		std::error_code failure;
		const std::filesystem::path resolved = std::filesystem::canonical(directory, failure);
		if (failure) {
			ENGINE_ERROR(
				"cdn: content root '{}' cannot be resolved: {}", directory.string(), failure.message()
			);
			return std::nullopt;
		}

		if (!std::filesystem::is_directory(resolved, failure)) {
			ENGINE_ERROR("cdn: content root '{}' is not a directory", resolved.string());
			return std::nullopt;
		}

		return ContentRoot(resolved);
	}

	std::optional<std::filesystem::path> ContentRoot::Resolve(std::string_view name) const {
		ENGINE_PROFILE_CAT("ContentRoot::Resolve", engine::core::ProfileCategory::Assets);

		std::optional<std::filesystem::path> permitted = Permit(Base, name);

		// A refusal is a security signal, not a miss, so it is counted apart
		// from a name that is simply not there. A refusal rate that climbs is
		// somebody walking the origin, and that is worth seeing on a dashboard
		// rather than reconstructing from logs after the fact.
		engine::core::Metrics::Count(permitted ? "cdn.resolve.served" : "cdn.resolve.refused", 1.0);
		return permitted;
	}

	bool ContentRoot::Exists(std::string_view name) const {
		ENGINE_PROFILE_CAT("ContentRoot::Exists", engine::core::ProfileCategory::Assets);

		const std::optional<std::filesystem::path> path = Resolve(name);
		if (!path) {
			return false;
		}

		std::error_code failure;
		return std::filesystem::is_regular_file(*path, failure);
	}
}
