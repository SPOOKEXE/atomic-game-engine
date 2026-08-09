#include "../Executable.hpp"

#define WIN32_LEAN_AND_MEAN
#include <vector>
#include <windows.h>

namespace engine::core::platform {

	std::filesystem::path ExecutablePath() {
		// GetModuleFileNameW truncates rather than reporting the size it
		// needed, and signals that by filling the buffer exactly. Grow until it
		// does not.
		std::vector<wchar_t> buffer(MAX_PATH);
		for (;;) {
			const DWORD written =
				GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
			if (written == 0) {
				return {};
			}
			if (written < buffer.size()) {
				return std::filesystem::path(std::wstring(buffer.data(), written));
			}
			if (buffer.size() >= 32768) {
				// The longest path Windows will produce. Anything past here is
				// a bug in the loop, not a longer path.
				return {};
			}
			buffer.resize(buffer.size() * 2);
		}
	}

	std::string_view ProgramSuffix() {
		return ".exe";
	}
}
