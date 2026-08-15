#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <docgen/Filter.hpp>
#include <string>

TEST_SUITE_ID("tools.docgen.filter")

using docgen::Promote;

namespace {

	size_t LineCount(std::string_view text) {
		return static_cast<size_t>(std::count(text.begin(), text.end(), '\n'));
	}

	bool Contains(std::string_view haystack, std::string_view needle) {
		return haystack.find(needle) != std::string_view::npos;
	}
}

// The invariant everything else rests on. Doxygen numbers its source listing
// from the filtered text and links to it by line, so a filter that adds or
// drops one line sends every link on the page off by one - a failure that looks
// like working documentation.
TEST_CASE("the line count never changes", "[docgen]") {
	const std::string source = "#pragma once\n"
							   "\n"
							   "// A thing.\n"
							   "//\n"
							   "// And why it is that way.\n"
							   "\n"
							   "#include <string>\n"
							   "\n"
							   "namespace engine {\n"
							   "\tclass Thing {};\n"
							   "}\n";

	REQUIRE(LineCount(Promote(source)) == LineCount(source));
}

TEST_CASE("a file's opening prose documents the file", "[docgen]") {
	const std::string source = "#pragma once\n"
							   "\n"
							   "// A stable name, and a cheap handle for it.\n"
							   "\n"
							   "#include <string>\n";

	const std::string filtered = Promote(source);

	// The blank line after `#pragma once` carries it, so the block below stays
	// contiguous and the count is untouched.
	REQUIRE(
		filtered == "#pragma once\n"
					"/// @file\n"
					"/// A stable name, and a cheap handle for it.\n"
					"\n"
					"#include <string>\n"
	);
}

TEST_CASE("a file with no opening prose gets no @file", "[docgen]") {
	const std::string source = "#pragma once\n"
							   "\n"
							   "#include <string>\n";

	REQUIRE(Promote(source) == source);
}

TEST_CASE("prose on the first line is still the file's", "[docgen]") {
	// No `#pragma once`, so there is no blank line above the block. The one
	// below it carries the command instead.
	const std::string source = "// What this file is.\n"
							   "\n"
							   "#include <string>\n";

	REQUIRE(
		Promote(source) == "/// What this file is.\n"
						   "/// @file\n"
						   "#include <string>\n"
	);
}

TEST_CASE("a comment owning its line is promoted, indentation kept", "[docgen]") {
	const std::string source = "class Name {\n"
							   "\t// Interns. Cheap to repeat.\n"
							   "\texplicit Name(std::string_view text);\n"
							   "};\n";

	REQUIRE(Contains(Promote(source), "\t/// Interns. Cheap to repeat.\n"));
}

// A trailing comment promoted to plain `///` would bind to the *next* member,
// so the documentation would appear against the wrong field and read as if it
// belonged there.
TEST_CASE("a comment after code documents what is to its left", "[docgen]") {
	const std::string source = "struct Camera {\n"
							   "\tfloat FieldOfViewRadians = 1.22f;  // 70 degrees\n"
							   "\tfloat NearPlane = 0.1f;\n"
							   "};\n";

	const std::string filtered = Promote(source);
	REQUIRE(Contains(filtered, "= 1.22f;  ///< 70 degrees\n"));
	REQUIRE_FALSE(Contains(filtered, "///  70 degrees"));
}

TEST_CASE("an empty comment line survives as an empty doc line", "[docgen]") {
	const std::string source = "// One.\n"
							   "//\n"
							   "// Two.\n"
							   "struct Thing {};\n";

	REQUIRE(Contains(Promote(source), "/// One.\n///\n/// Two.\n"));
}

TEST_CASE("a marker somebody wrote on purpose is left alone", "[docgen]") {
	const std::string source = "/// Already Doxygen's.\n"
							   "//! Also already Doxygen's.\n"
							   "struct Thing {};\n";

	REQUIRE(Promote(source) == source);
}

TEST_CASE("slashes inside a string are not a comment", "[docgen]") {
	const std::string source = "const char *Url = \"https://example.com\";\n"
							   "const char *Both = \"// not a comment\";  // but this one is\n"
							   "const char Escaped = '\\\\';  // and so is this\n";

	const std::string filtered = Promote(source);
	REQUIRE(Contains(filtered, "\"https://example.com\";\n"));
	REQUIRE(Contains(filtered, "\"// not a comment\";  ///< but this one is\n"));
	REQUIRE(Contains(filtered, "'\\\\';  ///< and so is this\n"));
}

// The shader tests hold GLSL in a raw string, and GLSL comments start with two
// slashes like everybody else's.
TEST_CASE("slashes inside a raw string are not a comment", "[docgen]") {
	const std::string source =
		R"CPP(constexpr const char *Fragment = R"(#version 450
// a GLSL comment, several lines into a raw string
void main() {}
)";
)CPP";

	const std::string filtered = Promote(source);
	REQUIRE(Contains(filtered, "\n// a GLSL comment, several lines into a raw string\n"));
	REQUIRE(LineCount(filtered) == LineCount(source));
}

TEST_CASE("slashes inside a block comment are not a comment", "[docgen]") {
	const std::string source = "/* opened here\n"
							   "   // still inside the block\n"
							   "*/\n"
							   "// out here it counts\n"
							   "struct Thing {};\n";

	const std::string filtered = Promote(source);
	REQUIRE(Contains(filtered, "   // still inside the block\n"));
	REQUIRE(Contains(filtered, "/// out here it counts\n"));
}

TEST_CASE("an inline block comment does not swallow the rest of the line", "[docgen]") {
	const std::string source = "actions.HandleEvent(Key(F5, /*repeat=*/true));  // held\n";

	REQUIRE(Promote(source) == "actions.HandleEvent(Key(F5, /*repeat=*/true));  ///< held\n");
}

// Unescaped, Doxygen reads these as HTML tags and renders `/shaders//` - the
// placeholder disappears and the page does not say it did.
TEST_CASE("a path placeholder is not an HTML tag", "[docgen]") {
	const std::string source = "// <assets>/shaders/<module>/. A module stages its own SPIR-V.\n"
							   "static std::filesystem::path Shaders(std::string_view module);\n";

	REQUIRE(Contains(Promote(source), "/// &lt;assets>/shaders/&lt;module>/."));
}

TEST_CASE("angle brackets that were never tags are left alone", "[docgen]") {
	const std::string source = "// Included as <engine/core/Name.hpp>, and true when a < b.\n"
							   "// A std::map<K, V> is not a tag either.\n"
							   "struct Thing {};\n";

	const std::string filtered = Promote(source);
	REQUIRE(Contains(filtered, "<engine/core/Name.hpp>"));
	REQUIRE(Contains(filtered, "a < b"));
	REQUIRE(Contains(filtered, "std::map<K, V>"));
	REQUIRE_FALSE(Contains(filtered, "&lt;"));
}

TEST_CASE("line endings are preserved", "[docgen]") {
	const std::string source = "#pragma once\r\n\r\n// A thing.\r\n\r\n#include <string>\r\n";
	const std::string filtered = Promote(source);

	REQUIRE(Contains(filtered, "/// @file\r\n"));
	REQUIRE(Contains(filtered, "/// A thing.\r\n"));
	REQUIRE(filtered.find('\n') != std::string::npos);
	REQUIRE(
		std::count(filtered.begin(), filtered.end(), '\r') == std::count(source.begin(), source.end(), '\r')
	);
}

TEST_CASE("a file with no trailing newline keeps not having one", "[docgen]") {
	const std::string source = "// A thing.";
	REQUIRE(Promote(source) == "/// A thing.");
}

TEST_CASE("an empty file filters to an empty file", "[docgen]") {
	REQUIRE(Promote("").empty());
	REQUIRE(Promote("\n") == "\n");
}

TEST_CASE("a bold sentence's full stop moves outside the emphasis", "[docgen]") {
	// **The whole bug, in one character.** `JAVADOC_AUTOBRIEF` ends the brief at
	// the first sentence stop and does not care that the stop is inside
	// emphasis - so `**Sentence.**` leaves the brief holding an unclosed `**`
	// and the detail holding its stranded partner.
	const std::string filtered = Promote("// **A failure here is a decision.** And then some.\n");
	CHECK(Contains(filtered, "**A failure here is a decision**."));
	CHECK_FALSE(Contains(filtered, "decision.**"));
}

TEST_CASE("emphasis spanning a line break is left spanning it", "[docgen]") {
	// **This was the wrong suspect and the test says so.** Bold across a
	// newline is fine; what broke was the stop inside it. Joining the lines
	// would have been the obvious fix and would have broken the line count,
	// which every source link on the generated page depends on.
	const std::string source = "// **Twenty-eight and not thirty-two, which is worth pinning rather\n"
							   "// than leaving to the compiler.** The arithmetic settles it.\n"
							   "int Thing;\n";

	const std::string filtered = Promote(source);
	CHECK(Contains(filtered, "**Twenty-eight"));
	CHECK(Contains(filtered, "than leaving to the compiler**."));
	CHECK(LineCount(filtered) == LineCount(source));
}

TEST_CASE("a stop that is already outside is left alone", "[docgen]") {
	const std::string filtered = Promote("// **Already correct**. And then some.\n");
	CHECK(Contains(filtered, "**Already correct**. And then some."));
}

TEST_CASE("an unpaired marker leaves the block untouched", "[docgen]") {
	// Shuffling punctuation across a boundary that is not emphasis at all would
	// be a rewrite of prose nobody wrote.
	const std::string filtered = Promote("// A **dangling marker with a stop.\n");
	CHECK(Contains(filtered, "**dangling marker with a stop."));
}

TEST_CASE("markers inside a code span are code", "[docgen]") {
	// `` `a ** b` `` is a quoted expression, and pairing across it would make
	// the next real marker a closer.
	const std::string filtered = Promote("// Consider `a ** b` and `c ** d` in prose.\n");
	CHECK(Contains(filtered, "`a ** b`"));
	CHECK(Contains(filtered, "`c ** d`"));
}

TEST_CASE("emphasis does not pair across two comment blocks", "[docgen]") {
	// Two blocks are two comments, and a marker in one closing a marker in the
	// other would be the same bleed the original bug had.
	const std::string source = "// A **first thing.\n"
							   "int One;\n"
							   "\n"
							   "// A **second thing.\n"
							   "int Two;\n";

	const std::string filtered = Promote(source);
	CHECK(Contains(filtered, "**first thing."));
	CHECK(Contains(filtered, "**second thing."));
	CHECK(LineCount(filtered) == LineCount(source));
}

TEST_CASE("an ellipsis moves whole rather than one stop at a time", "[docgen]") {
	const std::string filtered = Promote("// **A trailing thought...** And more.\n");
	CHECK(Contains(filtered, "**A trailing thought**... And more."));
}
