// The test report, read back out of the document another program wrote.
//
// **This is the half of `test_run` that can be wrong in silence.** Everything
// else in that tool either works or refuses out loud: a missing build directory,
// a runner that will not start, a child that took a signal. The reader does not.
// It parses markdown `mono.tools/testrunner` writes, and the first draft looked
// for a lowercase `fail` in a report that writes `FAIL` - so a run with five
// failed suites came back with an empty `failed` list and a summary that read
// like a section total. A model asked whether the tests passed was told the wrong
// thing, which is the worst answer a tool can give.
//
// So the fixtures below are the real shape, including the row markers and the
// per-section headings, and there is a failing report as well as a passing one.

#include "Report.hpp"

#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <string>

TEST_SUITE_ID("engine.control.report")

using engine::control::Failures;
using engine::control::Summary;

namespace {
	// A report with everything green, trimmed to the shape that matters.
	const std::string GREEN =
		"# Test results\n"
		"\n"
		"**All suites green.** 4985 of 4985 case(s) passed across 362 suite(s) in 171.71 s.\n"
		"\n"
		"362 of 362 suite(s) green. 362 ran; 0 were unchanged and carried forward from the cascade "
		"cache.\n"
		"\n"
		"## cdn\n"
		"\n"
		"191 of 191 case(s) passed across 14 suite(s) in 4.51 s.\n"
		"\n"
		"### cdn.catalogue\n"
		"\n"
		"| Suite | Cases | Passed | Failed | Skipped | Assertions | Time | Slowest case | Result |\n"
		"|---|--:|--:|--:|--:|--:|--:|--:|---|\n"
		"| `cdn.catalogue` | 6 | 6 | 0 | 0 | 130 | 313 ms | 1.8 ms | pass |\n";

	// The same document with two suites red, which is what the marker looks
	// like: uppercase, and in the last column.
	const std::string RED =
		"# Test results\n"
		"\n"
		"**Failing.** 5054 of 5062 case(s) passed, 8 failed across 368 suite(s) in 193.83 s.\n"
		"\n"
		"363 of 368 suite(s) green. 368 ran; 0 were unchanged and carried forward from the cascade "
		"cache.\n"
		"\n"
		"## client\n"
		"\n"
		"135 of 136 case(s) passed, 1 failed across 14 suite(s) in 8.81 s.\n"
		"\n"
		"| `client.compositor` | 14 | 14 | 0 | 0 | 2363 | 447 ms | 1.6 ms | pass |\n"
		"| `client.replica.arrival` | 10 | 9 | 1 | 0 | 193 | 527 ms | 87 ms | FAIL |\n"
		"\n"
		"## studio\n"
		"\n"
		"| `studio.editstream` | 25 | 21 | 4 | 0 | 218 | 324 ms | 13 ms | FAIL |\n";
}

TEST_CASE("a green report names no failed suite", "[control][report]") {
	CHECK(Failures(GREEN).empty());

	// The summary stops before the first section, so `## cdn`'s own total is not
	// reported as the run's.
	const std::string summary = Summary(GREEN);
	CHECK(summary.find("All suites green") != std::string::npos);
	CHECK(summary.find("362 of 362 suite(s) green") != std::string::npos);
	CHECK(summary.find("191 of 191") == std::string::npos);
}

TEST_CASE("a failing report names every suite that failed", "[control][report]") {
	const std::vector<std::string> failed = Failures(RED);
	REQUIRE(failed.size() == 2);
	CHECK(failed[0] == "client.replica.arrival");
	CHECK(failed[1] == "studio.editstream");

	const std::string summary = Summary(RED);
	CHECK(summary.find("**Failing.**") != std::string::npos);
	CHECK(summary.find("8 failed") != std::string::npos);
}

TEST_CASE("a report that was never written is not a green run", "[control][report]") {
	// What `test_result` reads when the runner died before writing anything. It
	// has to come back as no summary rather than as no failures, because the
	// caller distinguishes the two by the exit code beside them.
	CHECK(Failures("").empty());
	CHECK(Summary("").empty());

	// A row that mentions a suite but is not a result row is not a failure. The
	// reader matches on the row's shape, so a heading naming a red suite in
	// prose does not become one.
	CHECK(Failures("A note about `studio.editstream` failing.\n").empty());
}
