#include "PortalImageSamples.hpp"

#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <bit>
#include <cmath>
#include <limits>

TEST_SUITE_ID("engine.render.portalimagesamples")

namespace {
	using namespace engine::render;

	void Word(std::span<std::byte> bytes, size_t offset, uint32_t word) {
		for (size_t byte = 0; byte < 4; ++byte)
			bytes[offset + byte] = std::byte((word >> (byte * 8)) & 255);
	}

	void CheckWord(uint32_t word) {
		static_assert(sizeof(float) == 4 && std::numeric_limits<float>::is_iec559);
		const float value = std::bit_cast<float>(word);
		const bool finite = std::isfinite(value);
		std::array<std::byte, 17> storage{};
		const auto samples = std::span(storage).subspan(1);
		Word(samples, 0, word);
		CAPTURE(word);
		CHECK(ValidPortalDepthSamples(samples.first(4)) == (finite && !std::signbit(value)));
		CHECK(ValidPortalBaselineSamples(samples) == finite);
		CHECK(ValidPortalResponseSamples(samples) == (finite && !(value < 0)));
		Word(samples, 0, 0);
		Word(samples, 12, word);
		CHECK(ValidPortalResponseSamples(samples) == (finite && !(value < 0) && !(value > 1)));
	}
}

TEST_CASE("portal scalar bit scans match IEEE float classification", "[render][portalimagesamples]") {
	constexpr std::array mantissas{0u, 1u, 0x3fffffu, 0x400000u, 0x7fffffu};
	for (uint32_t sign : {0u, 0x80000000u})
		for (uint32_t exponent = 0; exponent < 256; ++exponent)
			for (uint32_t mantissa : mantissas)
				CheckWord(sign | (exponent << 23) | mantissa);
	uint32_t random = 0x71935a21u;
	for (size_t index = 0; index < 4096; ++index) {
		random ^= random << 13;
		random ^= random >> 17;
		random ^= random << 5;
		CheckWord(random);
	}
	for (uint32_t word : {0u, 0x80000000u, 0x3f7fffffu, 0x3f800000u, 0x3f800001u})
		CheckWord(word);
}

TEST_CASE("portal scalar scans validate complete unaligned planes", "[render][portalimagesamples]") {
	CHECK(ValidPortalDepthSamples({}));
	CHECK(ValidPortalBaselineSamples({}));
	CHECK(ValidPortalResponseSamples({}));
	std::array<std::byte, 67> storage{};
	for (size_t alignment = 0; alignment < 4; ++alignment) {
		const auto samples = std::span(storage).subspan(alignment, 64);
		CAPTURE(alignment);
		for (size_t size = 1; size < 64; ++size) {
			CHECK(ValidPortalDepthSamples(samples.first(size)) == (size % 4 == 0));
			CHECK(ValidPortalBaselineSamples(samples.first(size)) == (size % 16 == 0));
			CHECK(ValidPortalResponseSamples(samples.first(size)) == (size % 16 == 0));
		}
		for (size_t offset = 0; offset < samples.size(); offset += 4) {
			CAPTURE(offset);
			for (uint32_t invalid : {0x7f800000u, 0xff800000u, 0x7f800001u, 0xffc00001u}) {
				Word(samples, offset, invalid);
				CHECK_FALSE(ValidPortalDepthSamples(samples));
				CHECK_FALSE(ValidPortalBaselineSamples(samples));
				CHECK_FALSE(ValidPortalResponseSamples(samples));
			}
			Word(samples, offset, 0x80000000u);
			CHECK_FALSE(ValidPortalDepthSamples(samples));
			CHECK(ValidPortalBaselineSamples(samples));
			CHECK(ValidPortalResponseSamples(samples));
			Word(samples, offset, 0x80000001u);
			CHECK_FALSE(ValidPortalDepthSamples(samples));
			CHECK(ValidPortalBaselineSamples(samples));
			CHECK_FALSE(ValidPortalResponseSamples(samples));
			Word(samples, offset, 0x3f800001u);
			CHECK(ValidPortalResponseSamples(samples) == (offset % 16 != 12));
			Word(samples, offset, 0);
		}
	}
}
