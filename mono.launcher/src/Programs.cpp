#include <engine/core/Paths.hpp>

#include <launcher/Programs.hpp>
#include <system_error>

namespace launcher {

	std::filesystem::path StageRoot(const std::filesystem::path &selfDirectory) {
		// **`parent_path` of a directory that may carry a trailing separator.**
		// `SDL_GetBasePath` returns one with a separator on every platform, and
		// `path("a/b/").parent_path()` is `a/b` rather than `a` - so the
		// separator is removed first and the answer is the same either way.
		std::filesystem::path directory = selfDirectory;
		if (directory.has_filename() && directory.filename().empty()) {
			directory = directory.parent_path();
		}
		if (!directory.has_filename()) {
			directory = directory.parent_path();
		}
		return directory.parent_path();
	}

	std::filesystem::path ProgramPath(const std::filesystem::path &stageRoot, std::string_view program) {
		return stageRoot / std::filesystem::path(program) / engine::core::Paths::Program(program);
	}

	bool ProgramPresent(const std::filesystem::path &program) {
		// The non-throwing overload: a path on a volume that has gone away
		// throws from the other one, and a launcher that aborts because a
		// network drive is slow is worse than one that greys out a button.
		std::error_code failure;
		return std::filesystem::is_regular_file(program, failure);
	}
}
