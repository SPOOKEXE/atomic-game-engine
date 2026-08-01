#pragma once

// A private header, so it is unreachable from outside `core`. It names no
// operating system; the implementations under src/platform/<os>/ do, and the
// build picks exactly one of them.

#include <filesystem>

namespace engine::core::platform {

	// The absolute path of the running binary, or empty if the OS would not
	// say.
	std::filesystem::path ExecutablePath();
}
