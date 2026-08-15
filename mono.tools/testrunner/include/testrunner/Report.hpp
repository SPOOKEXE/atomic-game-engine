#pragma once

// The test report, as a document rather than a scroll of terminal output.
//
// The runner already knows which suites it ran and whether each one came back
// green. What the terminal cannot give is the shape of the whole tree at once:
// how many cases a module holds, which corner of it is thin, what the last run
// of a suite that is being skipped today actually said. That is what
// `test-output.md` and `test-output.html` are for.
//
// Sections are the suite identifiers themselves. `engine.core.arguments` sits
// under `engine`, then under `engine.core` - no second taxonomy to keep in step
// with the first, because a report that groups by anything other than
// TEST_SUITE_ID would eventually disagree with the runner about what a module
// is.
//
// Counts come from the test binaries, through the `mono` Catch2 reporter in
// mono.build/testmain. Reading them out of console text would make the report a
// guess about the run rather than a record of it.

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace testrunner {

	// One test case, and what it cost.
	//
	// The finest grain the report has. Suites are the sections; cases are the
	// bottom row of the flamegraph and the only place a single slow test can be
	// seen as a single slow test rather than as a suite that is vaguely heavy.
	struct CaseReport {
		// The TEST_CASE name, as a person wrote it.
		std::string Name;

		// Whether it reported a failure.
		bool Failed = false;

		// Whether Catch2 skipped it. A skipped case has a duration close to
		// zero, which is not the same claim as a fast one.
		bool Skipped = false;

		// Wall-clock inside the test binary, from the case starting to the case
		// ending - every SECTION of it, since a case holding sections is entered
		// once per leaf and the interesting number is what the case cost.
		unsigned long long Microseconds = 0;
	};

	// What one suite amounted to, whether it ran this time or not.
	struct SuiteReport {
		// The TEST_SUITE_ID, and therefore the path through the sections.
		std::string Id;

		// False when the numbers were carried forward from the cache because
		// the cascade found nothing that could have affected this suite.
		//
		// Reported rather than hidden: a green row a person cannot tell from a
		// row nothing re-checked is a green row that lies by omission.
		bool Ran = false;

		// Whether the suite is green. Taken from the binary's exit status, not
		// derived from the counts - a suite can exit non-zero without any case
		// reporting a failure, and that is still a failure.
		bool Passed = false;

		// Test cases that passed.
		unsigned CasesPassed = 0;

		// Test cases that failed.
		unsigned CasesFailed = 0;

		// Test cases Catch2 skipped - a SKIP() the suite reached and declined.
		// Counted apart from a pass, because a case that did not run is not a
		// case that worked.
		unsigned CasesSkipped = 0;

		// Assertions that held. The second number in the report, and the one
		// that says whether a suite of four cases checks four things or four
		// hundred.
		unsigned AssertionsPassed = 0;

		// Assertions that did not hold. A failing case usually carries one; it
		// can carry more.
		unsigned AssertionsFailed = 0;

		// Wall-clock for the whole suite, measured by the runner around the
		// process rather than by the binary inside it.
		//
		// It is therefore larger than the sum of the cases, by the cost of
		// starting a process and running its static initialisers. That gap is
		// not noise to be hidden - it is the fixed price of a suite existing,
		// and on a tree of thirty of them it is worth being able to see.
		unsigned long long Microseconds = 0;

		// Every case the suite ran, in the order it ran them.
		//
		// Empty for a suite whose numbers came from the cache: smart-tests.txt
		// is one line per suite, and per-case detail does not fit that without
		// becoming a different file. Such a suite is a leaf of the flamegraph
		// rather than a branch, and the report says which suites those are.
		std::vector<CaseReport> Cases;

		// Every case the suite holds, whatever became of it. Counted rather than
		// taken from Cases.size(), which is empty for a cached suite.
		unsigned CaseCount() const {
			return CasesPassed + CasesFailed + CasesSkipped;
		}

		// Wall-clock the cases account for. The remainder of Microseconds is
		// what the process cost before and after any test ran.
		unsigned long long CaseMicroseconds() const {
			unsigned long long total = 0;
			for (const auto &entry : Cases) {
				total += entry.Microseconds;
			}
			return total;
		}
	};

	// Reads what `--reporter mono::out=PATH` wrote.
	//
	// Fills the counts and Cases and leaves Id, Ran, Passed and Microseconds
	// alone; those are the caller's to decide, and a report file knows none of
	// the four.
	//
	// @return False if the header is missing or unrecognised, or if the run
	//         never reached its totals line - a binary that crashed halfway
	//         leaves a file that parses field by field and is still not a
	//         result. The counts are then not to be trusted, and the caller
	//         has the exit status to fall back on.
	bool ParseSuiteReport(std::string_view text, SuiteReport &report);

	// The same, reading the file itself. False also when it cannot be opened.
	//
	// A differently named function rather than an overload: a std::string
	// converts implicitly to both std::string_view and std::filesystem::path, so
	// an overload pair would be ambiguous at every call that has one in hand,
	// and worse, unambiguous and wrong wherever it happened not to be.
	bool ReadSuiteReport(const std::filesystem::path &path, SuiteReport &report);

	// A duration a person reads: "1.24 s", "310 ms", "4.2 ms", "870 µs".
	//
	// Three significant figures at most, and the unit chosen so that the number
	// in front of it has a magnitude somebody can hold. Public because the
	// runner's own terminal output says the same things in the same words as the
	// documents it writes, and two spellings of a duration in one tool is one
	// too many.
	std::string FormatDuration(unsigned long long microseconds);

	// test-output.md - what a diff and a pull request read.
	//
	// Suites are grouped by identifier and ordered within a group, so two runs
	// that learned the same thing produce the same bytes. There is deliberately
	// no timestamp: a document that changes every run is a document whose diff
	// says nothing.
	std::string RenderMarkdown(const std::vector<SuiteReport> &suites);

	// test-output.html - the same numbers, for a person rather than a diff.
	//
	// One self-contained file. A report that needs a network to render is a
	// report that does not render on the machine that produced it.
	std::string RenderHtml(const std::vector<SuiteReport> &suites);

	// Writes <directory>/test-output.md and <directory>/test-output.html,
	// creating the directory if it is not there.
	//
	// @return False if either file could not be written. A report that failed
	//         to save costs a person a document and nothing else, so it is not
	//         a reason to fail the tests.
	bool WriteReports(const std::filesystem::path &directory, const std::vector<SuiteReport> &suites);
}
