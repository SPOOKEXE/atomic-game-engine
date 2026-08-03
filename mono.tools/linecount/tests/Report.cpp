#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <linecount/Report.hpp>
#include <string>
#include <vector>

TEST_SUITE_ID("tools.linecount.report")

using linecount::FileCounts;
using linecount::Markdown;
using linecount::ReportOptions;

namespace {

	bool Contains(const std::string &haystack, const std::string &needle) {
		return haystack.find(needle) != std::string::npos;
	}

	// The row order is the claim, so a test about ordering has to be able to
	// ask which of two things came first rather than whether both are present.
	bool Before(const std::string &text, const std::string &first, const std::string &second) {
		const size_t left = text.find(first);
		const size_t right = text.find(second);
		return left != std::string::npos && right != std::string::npos && left < right;
	}

	FileCounts Make(std::string path, size_t code, size_t comment, size_t empty) {
		return {std::move(path), {empty, comment, code}};
	}
}

TEST_CASE("nothing found is said, not implied", "[linecount]") {
	// An empty table renders as a heading with a rule under it and reads like a
	// tree with no code in it. A sentence is the difference between "counted
	// nothing" and "found nothing to count".
	const std::string report = Markdown({});

	REQUIRE(Contains(report, "No C or C++ sources were found."));
	REQUIRE(!Contains(report, "## By directory"));
}

TEST_CASE("the summary is the three kinds and their shares", "[linecount]") {
	const std::vector<FileCounts> files = {Make("a/b/One.cpp", 50, 30, 20)};

	const std::string report = Markdown(files);

	REQUIRE(Contains(report, "| Code | 50 | 50.0% |"));
	REQUIRE(Contains(report, "| Comment | 30 | 30.0% |"));
	REQUIRE(Contains(report, "| Empty | 20 | 20.0% |"));
	REQUIRE(Contains(report, "| **Total** | **100** | |"));
}

TEST_CASE("a thousand is grouped, and a hundred is not", "[linecount]") {
	// Off-by-one in the grouping is the sort of thing that renders and looks
	// deliberate. These are the boundaries it goes wrong at.
	REQUIRE(Contains(Markdown({Make("One.cpp", 1, 0, 0)}), "| Code | 1 |"));
	REQUIRE(Contains(Markdown({Make("One.cpp", 123, 0, 0)}), "| Code | 123 |"));
	REQUIRE(Contains(Markdown({Make("One.cpp", 1234, 0, 0)}), "| Code | 1,234 |"));
	REQUIRE(Contains(Markdown({Make("One.cpp", 12345, 0, 0)}), "| Code | 12,345 |"));
	REQUIRE(Contains(Markdown({Make("One.cpp", 1234567, 0, 0)}), "| Code | 1,234,567 |"));
}

TEST_CASE("a file with no lines takes no share", "[linecount]") {
	// Zero divided by zero is where a percentage column becomes `nan%` or,
	// worse, `0.0%` — which reads as a real measurement of a real total.
	const std::string report = Markdown({Make("Empty.cpp", 0, 0, 0)});

	REQUIRE(Contains(report, "| Code | 0 | — |"));
	REQUIRE(!Contains(report, "nan"));
}

TEST_CASE("directories group at the requested depth", "[linecount]") {
	const std::vector<FileCounts> files = {
		Make("mono.engine/render/src/Renderer.cpp", 10, 0, 0),
		Make("mono.engine/render/include/engine/render/Renderer.hpp", 5, 0, 0),
		Make("mono.engine/core/src/Name.cpp", 3, 0, 0),
	};

	ReportOptions options;
	options.GroupDepth = 2;
	const std::string report = Markdown(files, options);

	// Two rows, not three: the module is the unit somebody asks about, and
	// src/ against include/ within one module is a different question.
	REQUIRE(Contains(report, "| `mono.engine/render` | 2 | 15 |"));
	REQUIRE(Contains(report, "| `mono.engine/core` | 1 | 3 |"));
}

TEST_CASE("a deeper grouping splits the same files further", "[linecount]") {
	const std::vector<FileCounts> files = {
		Make("mono.engine/render/src/Renderer.cpp", 10, 0, 0),
		Make("mono.engine/render/tests/Renderer.cpp", 5, 0, 0),
	};

	ReportOptions options;
	options.GroupDepth = 3;
	const std::string report = Markdown(files, options);

	REQUIRE(Contains(report, "| `mono.engine/render/src` | 1 | 10 |"));
	REQUIRE(Contains(report, "| `mono.engine/render/tests` | 1 | 5 |"));
}

TEST_CASE("a file shallower than the depth is grouped by its own directory", "[linecount]") {
	const std::vector<FileCounts> files = {Make("src/Main.cpp", 7, 0, 0), Make("Main.cpp", 3, 0, 0)};

	const std::string report = Markdown(files);

	// Not a prefix of the filename, which is what a naive segment count would
	// produce for a path with fewer directories than the depth asked for.
	REQUIRE(Contains(report, "| `src` | 1 | 7 |"));
	REQUIRE(Contains(report, "| `.` | 1 | 3 |"));
}

TEST_CASE("the biggest directory is the first row", "[linecount]") {
	const std::vector<FileCounts> files = {
		Make("a/small/One.cpp", 1, 0, 0),
		Make("a/big/Two.cpp", 100, 0, 0),
	};

	const std::string report = Markdown(files);

	REQUIRE(Before(report, "`a/big`", "`a/small`"));
}

// A report that reorders itself between two runs over the same tree cannot be
// diffed, which is most of what a checked-in one is for.
TEST_CASE("two orderings of the same files produce the same document", "[linecount]") {
	const std::vector<FileCounts> forwards = {
		Make("a/One.cpp", 10, 1, 1),
		Make("b/Two.cpp", 10, 2, 2),
		Make("c/Three.cpp", 10, 3, 3),
	};
	const std::vector<FileCounts> backwards = {
		Make("c/Three.cpp", 10, 3, 3),
		Make("b/Two.cpp", 10, 2, 2),
		Make("a/One.cpp", 10, 1, 1),
	};

	REQUIRE(Markdown(forwards) == Markdown(backwards));
}

TEST_CASE("the per-file table is opt-in", "[linecount]") {
	const std::vector<FileCounts> files = {Make("a/b/One.cpp", 10, 5, 5)};

	REQUIRE(!Contains(Markdown(files), "## By file"));

	ReportOptions options;
	options.IncludeFiles = true;
	const std::string report = Markdown(files, options);

	REQUIRE(Contains(report, "## By file"));
	REQUIRE(Contains(report, "| `a/b/One.cpp` | 10 | 5 | 5 | 20 |"));
}

TEST_CASE("the directory total matches the summary", "[linecount]") {
	const std::vector<FileCounts> files = {
		Make("a/One.cpp", 10, 5, 5),
		Make("b/Two.cpp", 20, 10, 10),
	};

	const std::string report = Markdown(files);

	REQUIRE(Contains(report, "2 files, 60 lines."));
	REQUIRE(Contains(report, "| **Total** | 2 | 30 | 15 | 15 | 60 | 100.0% |"));
}
