#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <linecount/Counter.hpp>
#include <string>

TEST_SUITE_ID("tools.linecount.counter")

using linecount::Count;
using linecount::Counts;
using linecount::LineKind;

// The invariant every number in the report rests on. The three totals partition
// the file, so a line that is classified twice or not at all shows up here
// rather than as a percentage that is quietly wrong.
TEST_CASE("the three totals are the file's line count", "[linecount]") {
	const std::string source = "// A thing.\n"
							   "\n"
							   "#include <string>\n"
							   "\n"
							   "int Answer() {\n"
							   "\treturn 42;  // why\n"
							   "}\n";

	REQUIRE(Count(source).Total() == 7);
	REQUIRE(Count(source).Comment == 1);
	REQUIRE(Count(source).Empty == 2);
	REQUIRE(Count(source).Code == 4);
}

TEST_CASE("an empty file is no lines at all", "[linecount]") {
	// Not one blank line. A file with nothing in it has no line in it either,
	// and counting one would put a blank into every module's total per file.
	REQUIRE(Count("").Total() == 0);
}

TEST_CASE("a file that does not end in a newline still ends in a line", "[linecount]") {
	REQUIRE(Count("int x = 1;").Code == 1);
	REQUIRE(Count("int x = 1;\nint y = 2;").Code == 2);
}

TEST_CASE("whitespace-only lines are empty however they are spelled", "[linecount]") {
	const std::string source = "\n"
							   "\t\n"
							   "    \n"
							   "\t  \t\n";

	REQUIRE(Count(source).Empty == 4);
	REQUIRE(Count(source).Total() == 4);
}

// A line of code that was explained is a line of code. Splitting it in half
// would make the comment share a function of how many lines somebody annotated
// rather than how much prose the file carries.
TEST_CASE("a trailing comment does not make the line a comment", "[linecount]") {
	const Counts counts = Count("\tCyclesPerSecond = 60;  // the tick rate\n");

	REQUIRE(counts.Code == 1);
	REQUIRE(counts.Comment == 0);
}

TEST_CASE("a block comment counts every line it spans", "[linecount]") {
	const std::string source = "/* A thing.\n"
							   " * And why.\n"
							   " */\n"
							   "int x = 1;\n";

	const Counts counts = Count(source);
	REQUIRE(counts.Comment == 3);
	REQUIRE(counts.Code == 1);
}

// The paragraph break inside a comment block is a blank line. Counting it as
// comment inflates the comment share by exactly how the prose was laid out.
TEST_CASE("a blank line inside a block comment is empty", "[linecount]") {
	const std::string source = "/* A thing.\n"
							   "\n"
							   "   And why. */\n";

	const Counts counts = Count(source);
	REQUIRE(counts.Empty == 1);
	REQUIRE(counts.Comment == 2);
}

TEST_CASE("code beside a block comment is code", "[linecount]") {
	REQUIRE(Count("/* why */ int x = 1;\n").Code == 1);
	REQUIRE(Count("int x = 1; /* why */\n").Code == 1);

	// The closing line of a block that then carries a statement. Both things
	// are true of it and code is the stronger claim.
	const std::string source = "/* A thing.\n"
							   " */ int x = 1;\n";
	const Counts counts = Count(source);
	REQUIRE(counts.Comment == 1);
	REQUIRE(counts.Code == 1);
}

TEST_CASE("a comment marker inside a string is not a comment", "[linecount]") {
	const std::string source = "const char *url = \"https://example.com\";\n"
							   "const char *block = \"/* not a comment\";\n";

	const Counts counts = Count(source);
	REQUIRE(counts.Code == 2);
	REQUIRE(counts.Comment == 0);
}

// The case docgen's filter carries the same machinery for: the shader tests
// hold GLSL in a raw string, and GLSL comments start with `//`. Counted as C++
// comments they would put a few hundred lines of shader source into the comment
// column of a module that has no such prose.
TEST_CASE("a raw string is code, whatever the language inside it is", "[linecount]") {
	const std::string source = "const char *shader = R\"(\n"
							   "// the GLSL comment\n"
							   "void main() {}\n"
							   ")\";\n";

	const Counts counts = Count(source);
	REQUIRE(counts.Comment == 0);
	REQUIRE(counts.Code == 4);
}

TEST_CASE("a delimited raw string closes on its own delimiter", "[linecount]") {
	// `)"` appears inside the body, so an undelimited scanner would leave the
	// literal early and read the rest of the file as code with comments in it.
	const std::string source = "const char *text = R\"glsl(\n"
							   "a )\" b\n"
							   ")glsl\";\n"
							   "// a real comment\n";

	const Counts counts = Count(source);
	REQUIRE(counts.Comment == 1);
	REQUIRE(counts.Code == 3);
}

TEST_CASE("a blank line inside a raw string is empty", "[linecount]") {
	const std::string source = "const char *text = R\"(\n"
							   "\n"
							   ")\";\n";

	REQUIRE(Count(source).Empty == 1);
}

TEST_CASE("an escaped quote does not end the string", "[linecount]") {
	const std::string source = "const char *quote = \"a \\\" // b\";\n";

	REQUIRE(Count(source).Comment == 0);
	REQUIRE(Count(source).Code == 1);
}

TEST_CASE("a marked comment is still a comment", "[linecount]") {
	// docgen promotes `//` to `///` on the way into Doxygen. Both spellings
	// exist in the tree - the second one where somebody meant a marker - and
	// this counts prose, not markers.
	const std::string source = "/// A documented thing.\n"
							   "//! Another.\n"
							   "/** And a third. */\n";

	REQUIRE(Count(source).Comment == 3);
}

TEST_CASE("a preprocessor directive is code", "[linecount]") {
	const std::string source = "#pragma once\n"
							   "#include <string>\n"
							   "#if defined(ENGINE_PROFILE)\n"
							   "#endif\n";

	REQUIRE(Count(source).Code == 4);
}

TEST_CASE("carriage returns do not become content", "[linecount]") {
	// A file with Windows endings must count the same as the same file without
	// them, or the report changes with whoever checked it out.
	const std::string windows = "// A thing.\r\n"
								"\r\n"
								"int x = 1;\r\n";
	const std::string unix = "// A thing.\n"
							 "\n"
							 "int x = 1;\n";

	REQUIRE(Count(windows).Empty == Count(unix).Empty);
	REQUIRE(Count(windows).Comment == Count(unix).Comment);
	REQUIRE(Count(windows).Code == Count(unix).Code);
}

TEST_CASE("classify says which line, not just how many", "[linecount]") {
	const std::string source = "// A thing.\n"
							   "\n"
							   "int x = 1;\n";

	const auto kinds = linecount::Classify(source);
	REQUIRE(kinds.size() == 3);
	REQUIRE(kinds[0] == LineKind::Comment);
	REQUIRE(kinds[1] == LineKind::Empty);
	REQUIRE(kinds[2] == LineKind::Code);
}

TEST_CASE("adding two files adds their columns", "[linecount]") {
	Counts total;
	total += Count("// A thing.\nint x = 1;\n");
	total += Count("\nint y = 2;\n");

	REQUIRE(total.Comment == 1);
	REQUIRE(total.Empty == 1);
	REQUIRE(total.Code == 2);
	REQUIRE(total.Total() == 4);
}
