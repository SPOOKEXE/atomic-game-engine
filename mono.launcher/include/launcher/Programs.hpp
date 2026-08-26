#pragma once

// Where the programs this launcher can start actually are.
//
// **The staged tree is the discovery rule, and there is deliberately no
// setting for it.** `mono_add_program` puts every program in
// `<stage>/<name>/<name>` with its shared libraries beside it and an rpath of
// `$ORIGIN`, so a launcher staged as
// `<stage>/launcher/launcher` finds the client at `../client/client` and is
// right by construction, in a build tree and in a shipped copy alike.
//
// A path a person configures is a path that goes stale the first time a build
// moves, and a launcher pointed at last week's client is the one bug this
// program must not have: it would start something, it would look like it
// worked, and the thing that ran would be the wrong build.
//
// **A missing program is an ordinary answer here, not a failure.** The `server`
// preset stages no client and the `cdn` preset stages neither, so a launcher
// built beside them has modes it cannot offer. It says so on the button rather
// than refusing to open.
//
// @tier L13 · client
// @since v0.18

#include <filesystem>
#include <string>
#include <string_view>

namespace launcher {

	// The staged tree the running program belongs to.
	//
	// @param selfDirectory The directory this program's binary is in.
	// @return Its parent, which is the directory every program is staged under.
	std::filesystem::path StageRoot(const std::filesystem::path &selfDirectory);

	// Where a named program is staged, whether or not it is there.
	//
	// Returned even when the file is absent, so a caller can say *which* path it
	// looked at. A launcher reporting "no client" and not saying where it looked
	// is a launcher nobody can debug from a screenshot.
	//
	// The platform's suffix comes from `core::Paths::Program`, which is where it
	// already lived - its own comment predicted that a sixth caller would paste
	// the `#ifdef` instead of finding it, and this was very nearly the sixth.
	//
	// @param stageRoot The staged tree - `StageRoot`.
	// @param program   The program's name, as `mono_add_program` declared it.
	// @return The path it would be at.
	std::filesystem::path ProgramPath(const std::filesystem::path &stageRoot, std::string_view program);

	// Whether that path is a file this process could run.
	//
	// @param program The path from `ProgramPath`.
	// @return `true` when it exists and is a regular file.
	bool ProgramPresent(const std::filesystem::path &program);
}
