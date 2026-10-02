#include "../src/nodes/BoundedRegex.hpp"

#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <regex>
TEST_SUITE_ID("engine.imagegraph.bounded_regex")
namespace regex_native = engine::imagegraph::detail::bounded_regex;
TEST_CASE("Bounded regex captures match pinned C++ DLL algorithm on short inputs", "[imagegraph][regex]") {
	for (const std::string pattern : {"a",			"a*",	   "a+",
									  "a?",			"a{2,3}",  "a*?b",
									  "(a|ab)b",	"(ab|a)b", "(a*)b\\1",
									  "(?:a|b)+",	"(a)?(b)", "(?=a)a",
									  "(?!a)b",		"[a-z]+",  "[^a]+",
									  "[\\dA]+",	"^a+$",	   "\\bword\\b",
									  "(a(b)?)+",	"[]",	   "[^]",
									  "a|",			"(a*)*",   "(a|b)*c",
									  "((a)|(b))+", "(a?)*b",  "(?=(a+))a*b\\1"})
		for (const std::string text :
			 {"",
			  "a",
			  "b",
			  "ab",
			  "aab",
			  "abab",
			  "aaabaa",
			  "123AA",
			  "word!",
			  "xbwordy",
			  "abbc",
			  "aba",
			  "ba",
			  "aabaaa"}) {
			INFO(pattern);
			INFO(text);
			regex_native::Pattern compiled{pattern, {}};
			REQUIRE(compiled.Compile());
			regex_native::Machine machine{compiled, text};
			regex_native::State match;
			const bool found = machine.Search(0, match);
			CHECK_FALSE(machine.Limited);
			const std::regex reference(pattern);
			std::smatch expected;
			REQUIRE(found == std::regex_search(text, expected, reference));
			if (found)
				for (size_t i = 0; i < expected.size(); ++i) {
					const auto capture = match.Captures[i];
					CHECK(
						(capture.Matched ? text.substr(capture.Start, capture.End - capture.Start)
										 : std::string{}) == expected[i].str()
					);
				}
		}
}
TEST_CASE("Bounded regex stops exponential searches and rejects malformed syntax", "[imagegraph][regex]") {
	regex_native::Pattern hostile{"(a+)+$", {}};
	REQUIRE(hostile.Compile());
	const std::string text = std::string(100, 'a') + "!";
	regex_native::Machine machine{hostile, text};
	regex_native::State match;
	CHECK_FALSE(machine.Search(0, match));
	CHECK(machine.Limited);
	for (const std::string pattern :
		 {"(", "[z-a]", "a{5,2}", "\\9", "(?<x>a)", "a{9999999999999999999999999999999999999999}"}) {
		INFO(pattern);
		regex_native::Pattern compiled{pattern, {}};
		CHECK_FALSE(compiled.Compile());
	}
}
