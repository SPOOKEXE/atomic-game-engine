#include "FontNativeCase.hpp"

#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

TEST_SUITE_ID("engine.imagegraph.font_native_case")
using namespace engine::imagegraph;

TEST_CASE("full Unicode default case expands and preserves contextual sigma", "[font_native_case]") {
	struct Fixture {
		const char *Input, *Expected;
		uint8_t Choice;
	};
	const Fixture fixtures[] = {
		{"Straße", "STRASSE", 2},
		{"İ", "i̇", 1},
		{"ÉCOLE", "école", 1},
		{"ΟΣ", "ος", 1},
		{"ΟΣΑ", "οσα", 1},
		{"ΟΣ́", "ος́", 1},
		{"ΟΣ́Α", "οσ́α", 1},
		{"Σ", "σ", 1},
		{"ͅΣ", "ͅς", 1},
		{"ﬃ", "FFI", 2},
		{"𐐨", "𐐀", 2},
		{"éCOLE ßeta\télan 𐐨x", "ÉCOLE SSeta\télan 𐐀x", 3}
	};
	for (const auto &fixture : fixtures) {
		INFO(fixture.Input);
		std::string result = "previous", failure;
		REQUIRE(
			detail::NativeFontTextCase(
				fixture.Input, fixture.Choice, Limits::MaximumEvaluationBytes, result, failure
			) == Status::Ok
		);
		CHECK(result == fixture.Expected);
	}
}
TEST_CASE("native case refuses malformed Unicode and workspace growth atomically", "[font_native_case]") {
	std::string result = "previous", failure;
	const std::string invalid(1, char(0xc0));
	CHECK(
		detail::NativeFontTextCase(invalid, 2, Limits::MaximumEvaluationBytes, result, failure) ==
		Status::InvalidValue
	);
	CHECK(result == "previous");
	CHECK(detail::NativeFontTextCase("Straße", 2, 1, result, failure) == Status::LimitExceeded);
	CHECK(result == "previous");
	CHECK(
		detail::NativeFontTextCase("valid", 4, Limits::MaximumEvaluationBytes, result, failure) ==
		Status::InvalidValue
	);
	CHECK(result == "previous");
}

TEST_CASE(
	"native full-case expansion admits old and candidate backing before replacing aliased text",
	"[font_native_case][font_case_capacity]"
) {
	std::string input, expected;
	for (size_t index = 0; index < 64; ++index) {
		input += "ﬃ";
		expected += "FFI";
	}
	std::string output = input, failure;
	output.reserve(4096);
	const size_t priorCapacity = output.capacity();
	const uint64_t allowance = uint64_t(64) * 17 + 3 * sizeof(std::vector<uint32_t>) + 64 + priorCapacity + 1;
	CHECK(
		detail::NativeFontTextCase(std::string_view(output), 2, allowance - 1, output, failure) ==
		Status::LimitExceeded
	);
	CHECK(output == input);
	CHECK(output.capacity() == priorCapacity);
	REQUIRE(
		detail::NativeFontTextCase(std::string_view(output), 2, allowance, output, failure) == Status::Ok
	);
	CHECK(output == expected);
}
