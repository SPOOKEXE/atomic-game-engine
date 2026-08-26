#pragma once

// Reading `mono.tools/testrunner`'s report back out.
//
// **Two string readers behind a header so a suite can reach them.** They parse a
// document another program writes, which makes them the part of `test_run` most
// likely to be quietly wrong: the first draft looked for a lowercase `fail` in a
// report that writes `FAIL`, and reported no failed suites over a run with five.
// That is worse than a crash - a model asked whether the tests passed and was
// told the wrong thing.
//
// Private to this module. `mono_add_tests` puts `src/` on the suite's include
// path for exactly this, so that testing something does not mean publishing it.

#include <string>
#include <vector>

namespace engine::control {

	// The report's opening paragraphs, which are the whole answer when
	// everything passed.
	//
	// Stops at the first per-section heading, so a section's own totals cannot
	// be read as the run's.
	//
	// @param report The generated markdown.
	// @return At most three lines, each ending in a newline.
	std::string Summary(const std::string &report);

	// Every suite whose row says it failed.
	//
	// @param report The generated markdown.
	// @return The suite identifiers, in the order the report lists them.
	std::vector<std::string> Failures(const std::string &report);
}
