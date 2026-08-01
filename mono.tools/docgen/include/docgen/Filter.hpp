#pragma once

// Doxygen reads marked comments. This engine writes plain ones.
//
// Doxygen only sees a comment written `///`, `//!`, `/**` or `/*!`. Every
// comment in this repository is a plain `//`, because CODE_FORMAT.md asks for
// prose that reads as prose and a marker earns its place by doing something.
//
// Rather than settle that by editing twenty-seven headers, the filter runs
// between the source and the parser: Doxygen's INPUT_FILTER hands it a file and
// reads the rewritten text back on stdout. The sources on disk never change.
//
//     // A stable name, and a cheap handle for it.   ->   /// A stable name, ...
//
// **Line count is preserved exactly.** Doxygen numbers the source listing from
// the filtered text and the browser links from the original, so a filter that
// adds or drops a line sends every link on the page one line out. That is the
// invariant the tests are mostly about, and it is why `@file` is written over a
// blank line that was already there rather than inserted.

#include <string>
#include <string_view>

namespace docgen {

	// The filtered form of one translation unit, with the same number of lines.
	//
	// Three rewrites, and nothing else:
	//
	// - A comment that starts a line becomes `///`. Indentation is kept, so a
	//   member's comment still lines up under the member.
	// - A comment that *follows* code on a line becomes `///<`, which is how
	//   Doxygen spells "this documents the thing to my left". Promoting it to
	//   plain `///` instead would attach it to the *next* member, silently
	//   documenting the wrong field.
	// - The blank line beside the leading comment block becomes `/// @file`,
	//   which is what makes a file's opening prose the file's documentation
	//   instead of the first declaration's.
	//
	// A comment already written `///`, `//!`, `/**` or `/*!` is left exactly as
	// it is. Someone who reached for a marker meant it.
	std::string Promote(std::string_view source);
}
