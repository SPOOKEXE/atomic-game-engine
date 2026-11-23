#include <engine/graphcore/Document.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

TEST_SUITE_ID("engine.graphcore.document")
TEST_TIER("fast")

using namespace engine::graphcore::text;
using engine::core::Name;

TEST_CASE("quoted names round trip and refuse escapes nothing writes", "[graphcore][document]") {
	std::string out;
	AppendQuoted(out, "a\"b\\c\nd\te\r");
	std::string_view line = out;
	std::string back;
	REQUIRE(TakeQuoted(line, back));
	CHECK(back == "a\"b\\c\nd\te\r");
	CHECK(AtEnd(line));
	std::string_view bad = "\"a\\x\"";
	CHECK_FALSE(TakeQuoted(bad, back));
	std::string_view open = "\"never closed";
	CHECK_FALSE(TakeQuoted(open, back));

	std::string_view numbers = "12 -3 0.333 yes 4x";
	uint32_t whole = 0;
	int32_t signedWhole = 0;
	float fraction = 0.0f;
	bool flag = false;
	CHECK(TakeUnsigned(numbers, whole));
	CHECK(TakeInteger(numbers, signedWhole));
	CHECK(TakeFloat(numbers, fraction));
	CHECK(TakeFlag(numbers, flag));
	CHECK((whole == 12 && signedWhole == -3 && fraction == 0.333f && flag));
	CHECK_FALSE(TakeUnsigned(numbers, whole));
}

TEST_CASE("sections split a set and refuse lines outside one", "[graphcore][document]") {
	constexpr std::string_view headers[] = {"doc 2", "doc 1"};
	std::string_view text = "doc 1\r\npipeline \"b\"\nx\n\npipeline \"a\"\ny\nz\n";
	REQUIRE(TakeHeader(text, headers) == 1);
	std::vector<std::string> seen;
	std::string offender;
	REQUIRE(ReadSections(
		text,
		[&](std::string_view name) { seen.push_back("open " + std::string(name)); },
		[&](std::string_view line) {
			seen.emplace_back(line);
			return true;
		},
		offender
	));
	CHECK(seen == std::vector<std::string>{"open b", "x", "open a", "y", "z"});
	CHECK_FALSE(ReadSections(
		text = "orphan\n", [](std::string_view) {}, [](std::string_view) { return true; }, offender
	));
	CHECK(offender == "orphan");
	CHECK_FALSE(ReadSections(
		"pipeline \"\"\n", [](std::string_view) {}, [](std::string_view) { return true; }, offender
	));

	DocumentSet<int> set;
	CHECK(set.Set(Name("zeta"), 1));
	CHECK(set.Set(Name("alpha"), 2));
	CHECK(set.Set(Name("zeta"), 3));
	CHECK_FALSE(set.Set(Name(""), 4));
	REQUIRE(set.Count() == 2);
	CHECK(set.Names()[0] == Name("alpha"));
	CHECK(*set.Find(Name("zeta")) == 3);
	CHECK(set.Remove(Name("alpha")));
	CHECK(set.Find(Name("alpha")) == nullptr);
}
