#include "../src/TextOps.hpp"

#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <string>
#include <vector>

TEST_SUITE_ID("engine.imagegraph.text_ops")

namespace ops = engine::imagegraph::detail;

TEST_CASE("Text count and replacement use non-overlapping matches", "[imagegraph]") {
	CHECK(ops::CountText("aaaa", "aa") == 2);
	CHECK(ops::CountText("abc", "") == 0);
	CHECK(ops::ReplaceText("aba aba", "aba", "x", false, 7) == "x aba");
	CHECK(ops::ReplaceText("aba aba", "aba", "x", true, 7) == "x x");
	CHECK_FALSE(ops::ReplaceText("abc", "", "x", true, 10).has_value());
	CHECK_FALSE(ops::ReplaceText("aaaa", "a", "xx", true, 7).has_value());
}

TEST_CASE("Text merge and split enforce bounds without losing empty fields", "[imagegraph]") {
	const std::array<std::string, 3> parts{"one", "-", "two"};
	CHECK(ops::CombineText(parts, 7) == "one-two");
	CHECK_FALSE(ops::CombineText(parts, 6).has_value());
	CHECK(ops::SplitText("a,,b,", ",", 4) == std::vector<std::string>{"a", "", "b", ""});
	CHECK_FALSE(ops::SplitText("a,,b,", ",", 3).has_value());
	CHECK_FALSE(ops::SplitText("abc", "", 5).has_value());
}
