#include "SourcePathShiftKey.hpp"

#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <string>
TEST_SUITE_ID("engine.imagegraph.source_path_sequential_keys")
using namespace engine::imagegraph::detail;
TEST_CASE(
	"Source distance cache keys preserve official HTML5 integer and fixed2 equivalence",
	"[source_path_sequential]"
) {
	const auto text = [](double number) {
		const auto key = SourcePathDistanceKey(number);
		REQUIRE(key);
		return std::string(key->Text.data(), key->Size);
	};
	CHECK(text(0) == "0");
	CHECK(text(-0.) == "0");
	CHECK(text(.001) == "0.00");
	CHECK(text(.004) == "0.00");
	CHECK(text(-.001) == "-0.00");
	CHECK(text(.005) == "0.01");
	CHECK(text(.125) == "0.13");
	CHECK(text(-.125) == "-0.13");
	CHECK(text(1) == "1");
	CHECK(text(1.001) == "1.00");
	CHECK(text(1.005) == "1.00");
	CHECK(text(2147483647.) == "2147483647");
	CHECK(text(2147483648.) == "2147483648.00");
	CHECK(SourcePathDistanceKey(.001) == SourcePathDistanceKey(.004));
	CHECK(SourcePathDistanceKey(0) != SourcePathDistanceKey(.001));
	CHECK(SourcePathDistanceKey(1e21) != SourcePathDistanceKey(std::nextafter(1e21, INFINITY)));
}
