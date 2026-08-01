#pragma once

// Where the program's own files are.
//
// A program stages into a directory that is runnable as it stands — binary,
// shared libraries, shaders. Everything here resolves relative to that
// directory rather than to the working directory, because the working directory
// is whatever the person who launched the game happened to be in.
//
// @tier L0 · shared

#include <filesystem>
#include <string_view>

namespace engine::core {

	// Resolves runtime assets relative to the running executable or an override.
	class Paths {
	  public:
		// The directory the running binary sits in. Resolved once, on first
		// call, and cached — the answer cannot change while the process lives.
		// Falls back to the working directory when the platform cannot report the
		// executable path.
		static const std::filesystem::path &Base();

		// Where staged data is read from. Base() unless overridden.
		//
		// The override exists so that a developer can point a release binary at
		// a working tree without reinstalling, and so that a test can run
		// against a fixture directory. Set it during startup, before anything reads
		// a file, so previously resolved paths cannot disagree with later ones.
		static const std::filesystem::path &Assets();

		// Replaces the assets directory; an empty path clears the override.
		static void SetAssetsOverride(const std::filesystem::path &directory);

		// <assets>/shaders/<module>/. A module stages its own SPIR-V under its
		// own name so that two modules cannot collide on fullscreen.vert.
		static std::filesystem::path Shaders(std::string_view module);
	};
}
