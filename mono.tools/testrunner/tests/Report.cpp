#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <fstream>
#include <random>
#include <testrunner/Report.hpp>

TEST_SUITE_ID("tools.testrunner.report")

namespace fs = std::filesystem;
using testrunner::CaseReport;
using testrunner::SuiteReport;

namespace {

	// A directory that cleans itself up, the same shape as the one in
	// Cascade.cpp. The report writes real files, so it is tested on real files.
	struct Scratch {
		fs::path Root;

		Scratch() {
			std::random_device device;
			Root = fs::temp_directory_path() /
				   ("testreport-" + std::to_string(device()) + std::to_string(device()));
			fs::create_directories(Root);
		}
		~Scratch() {
			std::error_code error;
			fs::remove_all(Root, error);
		}
	};

	std::string Read(const fs::path &path) {
		std::ifstream file(path);
		return std::string((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
	}

	// A suite with `passed` green cases and `failed` red ones, each costing a
	// millisecond, and the suite as a whole costing ten more than its cases -
	// which is the shape a real one has, process startup being nobody's case.
	SuiteReport Make(std::string id, unsigned passed, unsigned failed = 0, bool ran = true) {
		SuiteReport report;
		report.Id = std::move(id);
		report.Ran = ran;
		report.Passed = failed == 0;
		report.CasesPassed = passed;
		report.CasesFailed = failed;
		report.AssertionsPassed = passed * 2;

		for (unsigned index = 0; index < passed + failed; index++) {
			CaseReport entry;
			entry.Name = "case " + std::to_string(index);
			entry.Failed = index >= passed;
			entry.Microseconds = 1'000;
			report.Cases.push_back(std::move(entry));
		}
		report.Microseconds = (passed + failed) * 1'000ULL + 10'000ULL;

		// A cached suite has its total and none of the breakdown, because
		// smart-tests.txt is one line per suite.
		if (!ran) {
			report.Cases.clear();
		}
		return report;
	}
}

// ---------------------------------------------------------------------------
// Reading what a test binary reported
// ---------------------------------------------------------------------------

TEST_CASE("a report's counts come back out of it", "[report]") {
	SuiteReport report;
	REQUIRE(
		testrunner::ParseSuiteReport(
			"# mono test report v2\n"
			"case\tpass\t3\t0\t1500\tit adds up\n"
			"case\tskip\t0\t0\t4\tnot on this platform\n"
			"total\t1\t0\t1\t3\t0\n",
			report
		)
	);

	REQUIRE(report.CasesPassed == 1);
	REQUIRE(report.CasesFailed == 0);
	REQUIRE(report.CasesSkipped == 1);
	REQUIRE(report.AssertionsPassed == 3);
	REQUIRE(report.CaseCount() == 2);
}

TEST_CASE("every case comes back with what it cost", "[report]") {
	SuiteReport report;
	REQUIRE(
		testrunner::ParseSuiteReport(
			"# mono test report v2\n"
			"case\tpass\t3\t0\t1500\tit adds up\n"
			"case\tskip\t0\t0\t4\tnot on this platform\n"
			"total\t1\t0\t1\t3\t0\n",
			report
		)
	);

	REQUIRE(report.Cases.size() == 2);
	REQUIRE(report.Cases.front().Name == "it adds up");
	REQUIRE(report.Cases.front().Microseconds == 1500);
	REQUIRE(report.Cases.back().Skipped);
	REQUIRE(report.Cases.back().Microseconds == 4);

	// The suite's own duration is measured by the runner, around the process.
	// A report file cannot know it, so parsing must not invent one.
	REQUIRE(report.Microseconds == 0);
	REQUIRE(report.CaseMicroseconds() == 1504);
}

TEST_CASE("a failing case is marked, and a passing one is not", "[report]") {
	SuiteReport report;
	REQUIRE(
		testrunner::ParseSuiteReport(
			"# mono test report v2\n"
			"case\tpass\t1\t0\t20\tthis one is fine\n"
			"case\tfail\t0\t1\t900\ta column keeps its order\n"
			"total\t1\t1\t0\t1\t1\n",
			report
		)
	);

	REQUIRE_FALSE(report.Cases.front().Failed);
	REQUIRE(report.Cases.back().Failed);
	REQUIRE(report.Cases.back().Name == "a column keeps its order");
}

TEST_CASE("a report from another version is refused, not misread", "[report]") {
	SuiteReport report;
	// Numbers gathered differently do not mean the same thing, and a report is
	// held to the same standard as the cache. v1 had no timings, so its `case`
	// lines would parse with the name where the duration goes.
	REQUIRE_FALSE(testrunner::ParseSuiteReport("# mono test report v1\ntotal\t9\t0\t0\t9\t0\n", report));
	REQUIRE(report.CasesPassed == 0);
}

TEST_CASE("a run that never finished is not a result", "[report]") {
	SuiteReport report;

	// A binary that died halfway leaves a file whose every line parses. The
	// totals line is what says the run got to the end, so its absence is the
	// signal - the alternative is a crash reported as a suite of three cases
	// that all passed.
	REQUIRE_FALSE(
		testrunner::ParseSuiteReport(
			"# mono test report v2\n"
			"case\tpass\t1\t0\t10\tone\n"
			"case\tpass\t1\t0\t10\ttwo\n",
			report
		)
	);
}

TEST_CASE("a missing report file is refused rather than throwing", "[report]") {
	SuiteReport report;
	REQUIRE_FALSE(testrunner::ReadSuiteReport("/nonexistent/report.tsv", report));
}

TEST_CASE("a report file round-trips through the disk", "[report]") {
	Scratch scratch;
	const fs::path path = scratch.Root / "report.tsv";
	{
		std::ofstream file(path);
		file << "# mono test report v2\ncase\tpass\t2\t0\t70\tone\ntotal\t1\t0\t0\t2\t0\n";
	}

	SuiteReport report;
	REQUIRE(testrunner::ReadSuiteReport(path, report));
	REQUIRE(report.CasesPassed == 1);
	REQUIRE(report.AssertionsPassed == 2);
	REQUIRE(report.CaseMicroseconds() == 70);
}

// ---------------------------------------------------------------------------
// Durations
// ---------------------------------------------------------------------------

TEST_CASE("a duration is read in the unit it belongs in", "[report]") {
	using testrunner::FormatDuration;

	REQUIRE(FormatDuration(0) == "0 \xc2\xb5s");
	REQUIRE(FormatDuration(870) == "870 \xc2\xb5s");
	REQUIRE(FormatDuration(4'200) == "4.2 ms");
	REQUIRE(FormatDuration(310'000) == "310 ms");
	REQUIRE(FormatDuration(1'240'000) == "1.24 s");
}

TEST_CASE("a duration does not depend on how long the run was", "[report]") {
	// Every boundary from one unit to the next, so that a suite does not read
	// as "1000 ms" on one machine and "1.00 s" on another.
	REQUIRE(testrunner::FormatDuration(999) == "999 \xc2\xb5s");
	REQUIRE(testrunner::FormatDuration(1'000) == "1.0 ms");
	REQUIRE(testrunner::FormatDuration(9'999) == "10.0 ms");
	REQUIRE(testrunner::FormatDuration(10'000) == "10 ms");
	REQUIRE(testrunner::FormatDuration(999'999) == "1000 ms");
	REQUIRE(testrunner::FormatDuration(1'000'000) == "1.00 s");
}

// ---------------------------------------------------------------------------
// The documents
// ---------------------------------------------------------------------------

TEST_CASE("the sections are the identifier, one level at a time", "[report]") {
	const std::string page = testrunner::RenderMarkdown(
		{Make("engine.core.arguments", 4), Make("engine.ecs.store", 3), Make("tools.docgen.filter", 2)}
	);

	// A. Two top-level headings, because there are two first components.
	REQUIRE(page.find("\n## engine\n") != std::string::npos);
	REQUIRE(page.find("\n## tools\n") != std::string::npos);

	// A.B. The second component groups within the first, rather than every
	// suite becoming its own heading.
	REQUIRE(page.find("\n### engine.core\n") != std::string::npos);
	REQUIRE(page.find("\n### engine.ecs\n") != std::string::npos);

	// A.B.C. The suite itself is a row, whatever its depth.
	REQUIRE(page.find("`engine.core.arguments`") != std::string::npos);
}

TEST_CASE("a suite deeper than three components still lands in a section", "[report]") {
	const std::string page = testrunner::RenderMarkdown({Make("engine.ecs.column.core", 5)});

	REQUIRE(page.find("\n## engine\n") != std::string::npos);
	REQUIRE(page.find("\n### engine.ecs\n") != std::string::npos);
	REQUIRE(page.find("`engine.ecs.column.core`") != std::string::npos);
}

TEST_CASE("a suite shorter than the section depth is not dropped", "[report]") {
	// Nothing declares a one-component identifier today. If something does, it
	// belongs in the report rather than in a section that never renders.
	const std::string page = testrunner::RenderMarkdown({Make("standalone", 1)});

	REQUIRE(page.find("\n## standalone\n") != std::string::npos);
	REQUIRE(page.find("`standalone`") != std::string::npos);
}

TEST_CASE("the totals are the sum of the rows", "[report]") {
	const std::string page =
		testrunner::RenderMarkdown({Make("engine.core.arguments", 4), Make("engine.ecs.store", 3)});

	REQUIRE(page.find("7 of 7 case(s) passed") != std::string::npos);
	REQUIRE(page.find("All suites green") != std::string::npos);
}

TEST_CASE("a failure is stated in the summary, not only in a row", "[report]") {
	auto red = Make("engine.ecs.store", 3, 1);
	red.Cases.back().Name = "a column keeps its order";

	const std::string page = testrunner::RenderMarkdown({Make("engine.core.arguments", 4), red});

	REQUIRE(page.find("**Failing.**") != std::string::npos);
	REQUIRE(page.find("1 failed") != std::string::npos);
	REQUIRE(page.find("a column keeps its order") != std::string::npos);
}

TEST_CASE("a row from the cache says so", "[report]") {
	const std::string page = testrunner::RenderMarkdown({Make("engine.core.arguments", 4, 0, false)});

	// A green row a person cannot tell from a row nothing re-checked is a green
	// row that lies by omission.
	REQUIRE(page.find("pass (cached)") != std::string::npos);
	REQUIRE(page.find("1 were unchanged") != std::string::npos);
}

// ---------------------------------------------------------------------------
// Timings
// ---------------------------------------------------------------------------

TEST_CASE("every row carries what its suite cost", "[report]") {
	// Make() gives four cases of a millisecond each plus ten of overhead.
	const std::string page = testrunner::RenderMarkdown({Make("engine.core.arguments", 4)});

	REQUIRE(page.find("| Time |") != std::string::npos);
	REQUIRE(page.find("| 14 ms |") != std::string::npos);
}

TEST_CASE("a row names the cost of its worst case, not only its own", "[report]") {
	auto suite = Make("engine.core.arguments", 4);
	suite.Cases.front().Microseconds = 900'000;
	suite.Microseconds = 910'000;

	// A suite that is slow because one case is pathological and a suite that is
	// slow because two hundred are ordinary want different fixes, and the two
	// numbers side by side are what tells them apart.
	const std::string page = testrunner::RenderMarkdown({suite});

	REQUIRE(page.find("| Slowest case |") != std::string::npos);
	REQUIRE(page.find("| 910 ms | 900 ms |") != std::string::npos);
}

TEST_CASE("a cached suite claims no slowest case", "[report]") {
	const std::string page = testrunner::RenderMarkdown({Make("engine.core.arguments", 4, 0, false)});

	// It has a total and no breakdown. Zero would be a claim, and a false one.
	REQUIRE(page.find("| - |") != std::string::npos);
}

TEST_CASE("the summary says how long the whole run took", "[report]") {
	const std::string page =
		testrunner::RenderMarkdown({Make("engine.core.arguments", 4), Make("engine.ecs.store", 3)});

	// 14 ms + 13 ms.
	REQUIRE(page.find("in 27 ms.") != std::string::npos);
}

// ---------------------------------------------------------------------------
// The flamegraph
// ---------------------------------------------------------------------------

TEST_CASE("the flamegraph is a box per section, suite and case", "[report]") {
	const std::string html =
		testrunner::RenderHtml({Make("engine.core.arguments", 2), Make("tools.docgen.filter", 1)});

	REQUIRE(html.find("Where the time went") != std::string::npos);

	// Root, two firsts, two seconds, two suites, three cases.
	size_t boxes = 0;
	for (size_t at = html.find("class=\"box\""); at != std::string::npos;
		 at = html.find("class=\"box\"", at + 1)) {
		boxes++;
	}
	REQUIRE(boxes == 10);
}

TEST_CASE("a frame is as wide as its share of its parent", "[report]") {
	// One suite of 12 ms and one of 11 ms under the same section: the first is
	// 12/23 of it. Width is the only thing a flamegraph means.
	const std::string html =
		testrunner::RenderHtml({Make("engine.core.arguments", 2), Make("engine.core.paths", 1)});

	REQUIRE(html.find("width:100.0000%") != std::string::npos);
	REQUIRE(html.find("width:52.1739%") != std::string::npos);
	REQUIRE(html.find("width:47.8261%") != std::string::npos);
}

TEST_CASE("the widest frame comes first", "[report]") {
	const std::string html =
		testrunner::RenderHtml({Make("engine.core.paths", 1), Make("engine.core.arguments", 9)});

	// Whichever order they arrive in, the expensive one is on the left, where
	// somebody reading a flamegraph already looks.
	REQUIRE(html.find("engine.core.arguments") < html.find("engine.core.paths"));
}

TEST_CASE("a cached suite is a leaf of the flamegraph", "[report]") {
	const std::string html = testrunner::RenderHtml({Make("engine.core.arguments", 4, 0, false)});

	// Root, first, second, suite - and no cases, because smart-tests.txt keeps
	// a suite's total and not its breakdown.
	size_t boxes = 0;
	for (size_t at = html.find("class=\"box\""); at != std::string::npos;
		 at = html.find("class=\"box\"", at + 1)) {
		boxes++;
	}
	REQUIRE(boxes == 4);

	// And it is drawn washed out, so the gap is visible rather than implied.
	REQUIRE(html.find("22% 58%)") != std::string::npos);
}

TEST_CASE("a box keeps its colour between runs", "[report]") {
	// Hue comes from the label, so a person can follow one suite across two
	// reports. A colour drawn from position would move whenever anything else
	// got slower.
	const std::string first =
		testrunner::RenderHtml({Make("engine.core.arguments", 2), Make("engine.core.paths", 1)});
	const std::string second = testrunner::RenderHtml({Make("engine.core.arguments", 2)});

	const size_t at = first.find("background:hsl(");
	REQUIRE(at != std::string::npos);
	REQUIRE(second.find(first.substr(at, 20)) != std::string::npos);
}

TEST_CASE("a run that recorded no time draws no flamegraph", "[report]") {
	SuiteReport instant;
	instant.Id = "engine.core.arguments";
	instant.Ran = true;
	instant.Passed = true;

	// Every width would be a division by zero. A missing section is honest;
	// a graph of undefined proportions is not.
	const std::string html = testrunner::RenderHtml({instant});

	REQUIRE(html.find("Where the time went") == std::string::npos);
	REQUIRE(html.find("engine.core.arguments") != std::string::npos);
}

TEST_CASE("the two documents agree about the numbers", "[report]") {
	const std::vector<SuiteReport> suites{Make("engine.core.arguments", 4), Make("engine.ecs.store", 3)};

	const std::string markdown = testrunner::RenderMarkdown(suites);
	const std::string html = testrunner::RenderHtml(suites);

	const std::string sentence = "7 of 7 case(s) passed";
	REQUIRE(markdown.find(sentence) != std::string::npos);
	REQUIRE(html.find(sentence) != std::string::npos);
}

TEST_CASE("the same suites render the same bytes twice", "[report]") {
	// No timestamp, so a diff of the checked-out file shows what moved rather
	// than that it was written again.
	const std::vector<SuiteReport> suites{Make("engine.core.arguments", 4), Make("engine.ecs.store", 3)};

	REQUIRE(testrunner::RenderMarkdown(suites) == testrunner::RenderMarkdown(suites));
	REQUIRE(testrunner::RenderHtml(suites) == testrunner::RenderHtml(suites));
}

TEST_CASE("the order of the suites does not reach the document", "[report]") {
	const std::vector<SuiteReport> forwards{Make("engine.core.arguments", 4), Make("engine.ecs.store", 3)};
	const std::vector<SuiteReport> backwards{Make("engine.ecs.store", 3), Make("engine.core.arguments", 4)};

	REQUIRE(testrunner::RenderMarkdown(forwards) == testrunner::RenderMarkdown(backwards));
}

TEST_CASE("the page needs nothing off the machine that wrote it", "[report]") {
	const std::string html = testrunner::RenderHtml({Make("engine.core.arguments", 4)});

	// A report that needs a network to render is a report that does not render
	// on a build machine.
	REQUIRE(html.find("http://") == std::string::npos);
	REQUIRE(html.find("https://") == std::string::npos);
	REQUIRE(html.find("<script") == std::string::npos);
}

TEST_CASE("a test name is text in the page, not markup", "[report]") {
	auto red = Make("engine.core.arguments", 0, 1);
	red.Cases.back().Name = "a <script> tag & an ampersand";

	const std::string html = testrunner::RenderHtml({red});

	REQUIRE(html.find("&lt;script&gt; tag &amp; an ampersand") != std::string::npos);
	REQUIRE(html.find("<script>") == std::string::npos);
}

TEST_CASE("a test name is text in the flamegraph too", "[report]") {
	auto suite = Make("engine.core.arguments", 1);
	suite.Cases.front().Name = "quotes \" and <angles>";

	// The label goes in an element and the same string goes in a title
	// attribute. The attribute is the one that would break out of its quotes.
	const std::string html = testrunner::RenderHtml({suite});

	REQUIRE(html.find("title=\"quotes \" and") == std::string::npos);
	REQUIRE(html.find("&lt;angles&gt;") != std::string::npos);
}

// ---------------------------------------------------------------------------
// The hover readout
// ---------------------------------------------------------------------------

TEST_CASE("every box carries a readout", "[report]") {
	const std::string html = testrunner::RenderHtml({Make("engine.core.arguments", 2)});

	size_t boxes = 0;
	size_t tips = 0;
	for (size_t at = html.find("class=\"box\""); at != std::string::npos;
		 at = html.find("class=\"box\"", at + 1)) {
		boxes++;
	}
	for (size_t at = html.find("class=\"tip\""); at != std::string::npos;
		 at = html.find("class=\"tip\"", at + 1)) {
		tips++;
	}

	REQUIRE(boxes > 0);
	REQUIRE(tips == boxes);
}

TEST_CASE("a readout says more than the box had room for", "[report]") {
	auto suite = Make("engine.core.arguments", 3);
	suite.Cases.front().Microseconds = 900'000;
	suite.Microseconds = 950'000;

	const std::string html = testrunner::RenderHtml({suite});

	// The counts, the worst case, and the time no case accounts for - which is
	// the number a box cannot show and the reason to hover one.
	REQUIRE(html.find("3 case(s)") != std::string::npos);
	REQUIRE(html.find("6 assertion(s)") != std::string::npos);
	REQUIRE(html.find("slowest case 900 ms") != std::string::npos);
	REQUIRE(html.find("48 ms outside any case") != std::string::npos);
}

TEST_CASE("a readout says what share of its parent a frame is", "[report]") {
	const std::string html =
		testrunner::RenderHtml({Make("engine.core.arguments", 2), Make("engine.core.paths", 1)});

	// The width is the fact a flamegraph is made of, so it is also stated in
	// words - 12 ms of the section's 23 ms.
	REQUIRE(html.find("52.2% of engine.core") != std::string::npos);

	// The root is a share of nothing, so its own readout does not claim to be
	// one - even though its children all say "of all suites".
	const std::string opening = "class=\"tip\"><b>all suites</b><span>";
	const size_t at = html.find(opening);
	REQUIRE(at != std::string::npos);

	const size_t from = at + opening.size();
	const std::string readout = html.substr(from, html.find("</span>", from) - from);
	REQUIRE(readout.find("% of") == std::string::npos);
}

TEST_CASE("a case's readout names the suite it came from", "[report]") {
	auto suite = Make("engine.ecs.store", 1);
	suite.Cases.front().Name = "a column keeps its order";

	// The bottom row is names a person wrote, and two suites may reasonably
	// have written the same one.
	const std::string html = testrunner::RenderHtml({suite});

	REQUIRE(html.find("a column keeps its order") != std::string::npos);
	REQUIRE(html.find("in engine.ecs.store") != std::string::npos);
}

TEST_CASE("a readout appears on hover without a script", "[report]") {
	const std::string html = testrunner::RenderHtml({Make("engine.core.arguments", 2)});

	// Hovering a case must not also light up its suite's readout. The child
	// boxes live in .row, a sibling of this box, so the general-sibling
	// combinator is what gets the nesting right.
	REQUIRE(html.find(".flame .box:hover ~ .tip { display: block; }") != std::string::npos);
	REQUIRE(html.find("<script") == std::string::npos);
}

TEST_CASE("a readout cannot hang off the edge of the page", "[report]") {
	const std::string html = testrunner::RenderHtml({Make("engine.core.arguments", 2)});

	// A box one pixel wide at the right-hand edge would carry a panel that
	// overflows the window, and CSS cannot measure the edge to avoid it.
	// Fixed to the viewport, every readout is the same size in the same place.
	REQUIRE(html.find(".flame .tip { display: none; position: fixed;") != std::string::npos);
}

TEST_CASE("an empty run is a document rather than a crash", "[report]") {
	const std::string page = testrunner::RenderMarkdown({});

	REQUIRE(page.find("# Test results") != std::string::npos);
	REQUIRE(page.find("0 of 0 case(s) passed") != std::string::npos);
}

TEST_CASE("both documents land where they were asked for", "[report]") {
	Scratch scratch;
	const fs::path directory = scratch.Root / "nested" / "report";

	// The directory does not exist yet, on purpose: .cache/ is not there on a
	// fresh clone either.
	REQUIRE(testrunner::WriteReports(directory, {Make("engine.core.arguments", 4)}));

	REQUIRE(fs::exists(directory / "test-output.md"));
	REQUIRE(fs::exists(directory / "test-output.html"));
	REQUIRE(Read(directory / "test-output.md").find("`engine.core.arguments`") != std::string::npos);
	REQUIRE(Read(directory / "test-output.html").find("<!doctype html>") != std::string::npos);
}
