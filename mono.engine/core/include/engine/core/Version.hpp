#pragma once

// What version this build was cut from.
//
// One string, and it is not a feature switch. Nothing in the engine branches on
// it: a version number is for the person reading a crash report, not for code
// deciding what it is allowed to do. Where two builds have to disagree about
// behaviour, that is a setting or a build option, and both of those say so at
// the place they are read.
//
// @tier L0 · shared

#include <string_view>

namespace engine::core {

	// The engine version, as `major.minor.patch`.
	//
	// Comes from `project(atomic VERSION ...)`, which reads the `VERSION` file
	// in the repository root - so a build reports the version its own tree
	// declared rather than one somebody remembered to update. Every program
	// prints it for `--version`.
	//
	// @return A view of a string literal, which outlives every caller.
	std::string_view Version();
}
