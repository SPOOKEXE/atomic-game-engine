#pragma once

// A private header, so it is unreachable from outside `core`. It names no
// operating system; the implementations under src/platform/<os>/ do, and the
// build picks exactly one of them.

#include <filesystem>
#include <string_view>

namespace engine::core::platform {

	// The absolute path of the running binary, or empty if the OS would not
	// say.
	std::filesystem::path ExecutablePath();

	// What this platform puts on the end of a program's file name.
	//
	// `.exe` on Windows and nothing anywhere else. Here rather than in a
	// `#if` beside each caller, because there were five callers and each was
	// one edit away from being the one that forgot.
	//
	// @return The suffix, including its dot, or empty when there is none.
	std::string_view ProgramSuffix();
}
