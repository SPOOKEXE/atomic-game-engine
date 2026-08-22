#pragma once

// Where this program was built from, when it was built from a checkout.
//
// **Two of the tools this module registers only exist inside a repository.** A
// test run needs the sources it signs suites against, and the `AGENTS.md`
// resources are files in a working tree. Neither is a property of the engine, so
// neither is answered by compiling something in: they are answered by finding
// the checkout this executable was staged into, or by admitting there is not
// one.
//
// **Found by walking up from the executable rather than from the current
// directory**, which is the difference between a tool that works and one that
// works when somebody remembered to `cd` first. An editor is started by a
// person, a launcher or a client's MCP configuration, and none of the three
// agrees about where the shell was standing.
//
// Both answers are computed once and kept. A build tree does not move while a
// program runs, and a filesystem walk per tool call would be a syscall storm
// inside a frame.

#include <filesystem>

namespace engine::control {

	// The checkout this executable was staged into.
	//
	// Recognised by a directory holding both `AGENTS.md` and `mono.engine`,
	// which is a pair no subdirectory of this repository has.
	//
	// @return The root, or an empty path when this is a staged release rather
	//         than a build tree.
	const std::filesystem::path &RepositoryRoot();

	// The configured build directory this executable came out of.
	//
	// Recognised by `target-graph.json`, which `CMakeLists.txt` writes into
	// `CMAKE_BINARY_DIR` at the end of every configure. That makes the marker a
	// fact about the build rather than a guess about the preset: an executable
	// under `.cache/build/release` finds the release tree and not the dev one.
	//
	// @return The directory, or an empty path when there is no build tree above
	//         this executable.
	const std::filesystem::path &BuildDirectory();
}
