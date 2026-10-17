#include <engine/core/Float16.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <bit>
#include <cmath>
#include <cstdint>
#include <limits>

TEST_SUITE_ID("engine.core.float16")
using namespace engine::core;

TEST_CASE("binary16 finite patterns round trip and ties round to even", "[float16]") {
	bool complete = true;
	for (uint32_t bits = 0; bits <= UINT16_MAX; bits++) {
		if ((bits & 0x7c00) == 0x7c00) continue;
		complete &= EncodeFloat16(DecodeFloat16(uint16_t(bits))) == bits;
	}
	CHECK(complete);
	CHECK(EncodeFloat16(1.0f + std::ldexp(1.0f, -11)) == 0x3c00);
	CHECK(EncodeFloat16(1.0f + 3.0f * std::ldexp(1.0f, -11)) == 0x3c02);
	CHECK(EncodeFloat16(std::ldexp(1.0f, -24)) == 1);
	CHECK(EncodeFloat16(std::ldexp(1.0f, -25)) == 0);
	CHECK(EncodeFloat16(65504.0f) == 0x7bff);
	CHECK(EncodeFloat16(-0.0f) == 0x8000);
	CHECK(std::signbit(DecodeFloat16(0x8000)));
	CHECK(std::isinf(DecodeFloat16(0x7c00)));
	CHECK(std::isnan(DecodeFloat16(0x7e00)));
	CHECK(std::bit_cast<uint32_t>(DecodeFloat16(0x0001)) == std::bit_cast<uint32_t>(std::ldexp(1.0f, -24)));
}
