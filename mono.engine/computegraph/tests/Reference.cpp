#include <engine/computegraph/Reference.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <numeric>

TEST_SUITE_ID("engine.computegraph.reference")
TEST_DEPENDS("engine.computegraph.document")

using namespace engine::computegraph;

TEST_CASE(
	"compaction and consuming double preserve stable order at scan boundaries", "[computegraph][reference]"
) {
	for (const uint32_t count : {0u, 1u, 63u, 64u, 65u, 257u, 4097u, 65537u, 1024u * 1024u}) {
		CAPTURE(count);
		std::vector<uint32_t> input(count);
		std::vector<uint32_t> expected;
		for (uint32_t index = 0; index < count; ++index) {
			input[index] = index % 3 == 0 ? 0 : index + 1;
			if (input[index] != 0) expected.push_back(input[index] * 2);
		}
		std::vector<uint32_t> actual{99};
		Diagnostic diagnostic;
		REQUIRE(CompactDoubleU32(input, count, count, actual, diagnostic) == Status::Ok);
		CHECK(actual == expected);
		CHECK(diagnostic.Code == Status::Ok);
	}
}

TEST_CASE(
	"compaction chain preserves accepted output on excess input capacity or arithmetic",
	"[computegraph][reference]"
) {
	std::vector<uint32_t> output{99, 101};
	const auto previous = output;
	Diagnostic diagnostic;
	const std::array input{3u, 0u, 7u, 0u, 9u};
	CHECK(CompactDoubleU32(input, 4, 5, output, diagnostic) == Status::LimitExceeded);
	CHECK(output == previous);
	CHECK(CompactDoubleU32(input, 5, 2, output, diagnostic) == Status::OutputOverflow);
	CHECK(output == previous);
	const std::array overflowing{3u, UINT32_MAX};
	CHECK(CompactDoubleU32(overflowing, 2, 2, output, diagnostic) == Status::OutputOverflow);
	CHECK(output == previous);
	CHECK(DoubleU32(input, 6, 6, output, diagnostic) == Status::InvalidCount);
	CHECK(output == previous);
	CHECK(DoubleU32(input, 1, 1, output, diagnostic) == Status::Ok);
	CHECK(output == std::vector<uint32_t>{6});
}
