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

		// A program's file name, with whatever this platform puts on the end.
		//
		// **The file name only, not a path**, because the callers disagree
		// about the directory and agree about nothing else: a supervisor wants
		// its own staged directory, a test wants a sibling program's. Joining
		// is left to them; the part that was being copied is the suffix.
		//
		// Named rather than spelled at each call site because it was spelled at
		// five of them, each behind its own `#if`, and a sixth caller would
		// have been one paste away from being the one that forgot.
		//
		// @param name The program's name, without a suffix.
		// @return `name` on Unix, `name.exe` on Windows.
		static std::filesystem::path Program(std::string_view name);

		// Where the vendored typefaces are staged.
		//
		// Under the assets root rather than beside the binary, so
		// `--override-assets-directory` moves the fonts with everything else —
		// a tree that had the shaders relocated and the fonts not would be half
		// an override.
		//
		// @return `Assets() / "fonts"`.
		static std::filesystem::path Fonts();
	};
}
