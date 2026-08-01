#include "../Executable.hpp"

#include <system_error>

namespace engine::core::platform {

	std::filesystem::path ExecutablePath() {
		// /proc/self/exe is a symlink to the binary, already resolved through
		// any symlink used to launch it.
		std::error_code error;
		auto resolved = std::filesystem::read_symlink("/proc/self/exe", error);
		if (error) {
			return {};
		}
		return resolved;
	}
}
