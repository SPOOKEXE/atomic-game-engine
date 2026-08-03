#pragma once

// The counts, as a markdown document somebody can paste into a pull request.
//
// Kept apart from Counter.hpp because counting is a fact and presenting is a
// choice. The scanner decides what a line is; this decides what order the rows
// come in, how a share is rounded, and how deep the grouping goes — and none of
// those should be able to break the thing the tests are actually about.
//
//     std::cout << Markdown(files, {.GroupDepth = 2, .IncludeFiles = false});
//
// @tier L0 · shared

#include <cstddef>
#include <linecount/Counter.hpp>
#include <string>
#include <vector>

namespace linecount {

	// One file, and what was in it.
	struct FileCounts {
		// The path as it should be printed — relative, with `/` separators, so
		// that a report from Windows and one from Linux are the same document.
		std::string Path;
		// The three totals for that file.
		Counts Lines;
	};

	// What the document contains, and how coarse it is.
	struct ReportOptions {
		// How many leading path segments a directory row groups by.
		//
		// Two, so that a walk of a repository root groups by module —
		// `mono.engine/render` rather than one row each for its `src/`, its
		// `include/engine/render/` and its `tests/`. A file shallower than this
		// is grouped by whatever directory it is in.
		size_t GroupDepth = 2;
		// Whether to append a row per file. Off, because a repository has more
		// files than a document anybody reads, and the directory table is the
		// answer to the question that was asked.
		bool IncludeFiles = false;
	};

	// The whole document, ending in a newline.
	//
	// Directory rows are ordered by code lines, largest first, because the
	// first question of a breakdown is always which part is the big one. Ties
	// fall back to the path so that two runs over the same tree produce
	// byte-identical output — a report that reorders itself cannot be diffed.
	//
	// @param files Every file counted, in any order.
	// @param options Grouping depth, and whether to list files individually.
	// @return Markdown, or a one-line document saying nothing was found.
	std::string Markdown(const std::vector<FileCounts> &files, const ReportOptions &options = {});
}
