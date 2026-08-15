#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// Source Map v3, read for one question: which line of the file an author wrote
// did this generated line come from?
//
// **The engine runs transpiled JavaScript and an author debugs TypeScript.**
// `mono.engine/examples/CMakeLists.txt` strips types with `tsc`, and stripping
// does not preserve line numbers - a multi-line type annotation collapses into
// one line and everything below it shifts up. So a stack frame naming
// `Mirrors-4-worlds.js:118` points into a file nobody has open.
//
// **Only the line is recovered, and the column is deliberately dropped.** A
// column would need the full segment search per frame and buys nothing here: a
// stack frame is read to find a line in an editor. Recovering the wrong column
// beside a right line is worse than no column at all.
//
// This is not a general source-map library and should not become one. Nothing
// here reads `names`, `sourcesContent` or an index map with `sections`, because
// nothing in this repository emits them.
namespace engine::script {

	// One generated file's line-for-line origin.
	struct SourceMap {
		// The file the map names, resolved against the directory the map is in
		// so it can be opened rather than only printed. Empty when the map named
		// no source.
		std::string Source;

		// Generated line to source line, both 1-based, indexed by
		// `generated - 1`. A zero means that generated line maps to nothing -
		// blank lines and lines `tsc` emitted on its own - and the frame is then
		// left as it was rather than pointed at line zero.
		std::vector<uint32_t> SourceLines;

		// The source line a generated line came from.
		//
		// @param generated The 1-based line in the generated file.
		// @return The 1-based source line, or zero when it maps to nothing.
		uint32_t LineFor(uint32_t generated) const;
	};

	// Reads a `.js.map` from disk.
	//
	// **Every failure is the same answer - `std::nullopt` - and that is the
	// point.** A missing map is the ordinary case for hand-written JavaScript, a
	// malformed one is a toolchain fault nobody can act on from inside a stack
	// trace, and treating either as fatal would turn "your script threw" into
	// "the engine could not read a file you have never heard of".
	//
	// @param path The map file.
	// @return The map, or nothing when it is absent or unreadable.
	std::optional<SourceMap> LoadSourceMap(const std::filesystem::path &path);

	// Rewrites the `file:line` frames in a VM's stack text through the map
	// beside each file.
	//
	// **A frame whose map is missing comes back exactly as it was**, which is
	// what makes this safe to run over every exception: hand-written JavaScript,
	// Luau, and a chunk named after an instance rather than a path all fall
	// through untouched.
	//
	// @param text The exception text, stack included.
	// @return The text with mapped frames rewritten.
	std::string MapStackFrames(std::string_view text);

}
