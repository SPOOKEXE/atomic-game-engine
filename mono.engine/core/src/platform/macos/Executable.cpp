#include "../Executable.hpp"

#include <mach-o/dyld.h>
#include <system_error>
#include <vector>

namespace engine::core::platform {

	std::filesystem::path ExecutablePath() {
		// _NSGetExecutablePath reports the size it needs when the buffer is too
		// small, so this needs at most two calls.
		uint32_t size = 0;
		_NSGetExecutablePath(nullptr, &size);

		std::vector<char> buffer(size);
		if (_NSGetExecutablePath(buffer.data(), &size) != 0) {
			return {};
		}

		// The path may run through a symlink or contain ".." components; an app
		// bundle's Contents/MacOS is one level down from Resources.
		std::error_code error;
		auto canonical = std::filesystem::canonical(std::filesystem::path(buffer.data()), error);
		if (error) {
			return std::filesystem::path(buffer.data());
		}
		return canonical;
	}
}
