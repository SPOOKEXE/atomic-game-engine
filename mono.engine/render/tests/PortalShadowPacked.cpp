#include <engine/render/PortalShadowPacked.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <bit>

TEST_SUITE_ID("engine.render.portalshadowpacked")

namespace {
	using namespace engine::render;
	constexpr size_t SAMPLE_COUNT = PORTAL_SHADOW_BYTES / 4;

	void WriteSample(std::vector<std::byte> &depth, size_t sample, uint32_t word) {
		for (size_t byte = 0; byte < 4; ++byte)
			depth[sample * 4 + byte] = std::byte((word >> (byte * 8)) & 0xff);
	}
	template <class Sample> std::vector<std::byte> DepthOf(Sample sample) {
		std::vector<std::byte> depth(PORTAL_SHADOW_BYTES);
		for (size_t index = 0; index < SAMPLE_COUNT; ++index)
			WriteSample(depth, index, sample(index));
		return depth;
	}
	std::vector<uint32_t> Pack(const std::vector<std::byte> &depth) {
		std::vector<uint32_t> words;
		std::string error;
		const bool packed = PackPortalShadow(depth, PORTAL_SHADOW_PACKED_MAX_BYTES, words, error);
		INFO(error);
		REQUIRE(packed);
		REQUIRE(error.empty());
		REQUIRE(ValidPortalShadowPacked(words));
		return words;
	}
	void CheckRoundtrip(const std::vector<std::byte> &depth, const std::vector<uint32_t> &words) {
		std::vector<std::byte> decoded;
		std::string error;
		const bool unpacked = UnpackPortalShadow(words, PORTAL_SHADOW_BYTES, decoded, error);
		INFO(error);
		REQUIRE(unpacked);
		CHECK(error.empty());
		CHECK(decoded == depth);
		CHECK(Pack(decoded) == words);
	}
	std::vector<uint32_t> FirstBlock(uint32_t width) {
		std::vector<uint32_t> words(PORTAL_SHADOW_PACKED_DESCRIPTOR_WORDS + 2 * width);
		for (size_t block = 0; block < PORTAL_SHADOW_PACKED_BLOCK_COUNT; ++block)
			words[block * 2 + 1] = static_cast<uint32_t>(words.size());
		words[1] = static_cast<uint32_t>(PORTAL_SHADOW_PACKED_DESCRIPTOR_WORDS) | (width << 26);
		return words;
	}
}

TEST_CASE("packed shadow constants require only descriptors", "[render][portal-shadow-packed]") {
	const uint32_t value = GENERATE(0u, 1u, 0x3f800000u);
	CAPTURE(value);
	const auto depth = DepthOf([&](size_t) { return value; });
	const auto words = Pack(depth);
	REQUIRE(words.size() == PORTAL_SHADOW_PACKED_DESCRIPTOR_WORDS);
	CHECK(words.front() == value);
	CHECK(words[1] == PORTAL_SHADOW_PACKED_DESCRIPTOR_WORDS);
	CHECK(words.back() == PORTAL_SHADOW_PACKED_DESCRIPTOR_WORDS);
	CheckRoundtrip(depth, words);
}

TEST_CASE("packed shadow ramps cover every valid delta width", "[render][portal-shadow-packed]") {
	const auto depth = DepthOf([](size_t index) {
		const size_t block = index / PORTAL_SHADOW_PACKED_BLOCK_SAMPLES;
		const size_t sample = index % PORTAL_SHADOW_PACKED_BLOCK_SAMPLES;
		const uint32_t width = block % 31;
		const uint32_t maximum = width == 30 ? 0x3f800000u : (1u << width) - 1;
		const uint32_t base = width == 30 ? 0 : 0x10000u + uint32_t(block % 512);
		return base + uint32_t(uint64_t(sample) * maximum / 63);
	});
	const auto words = Pack(depth);
	for (size_t block = 0; block < 31; ++block) {
		CAPTURE(block);
		CHECK((words[block * 2 + 1] >> 26) == block);
	}
	CheckRoundtrip(depth, words);
}

TEST_CASE("packed shadow block boundaries use absolute LSB offsets", "[render][portal-shadow-packed]") {
	const auto depth = DepthOf([](size_t index) {
		return index < PORTAL_SHADOW_PACKED_BLOCK_SAMPLES ? uint32_t(index % 2) * 7 : 0x3f800000u;
	});
	const auto words = Pack(depth);
	REQUIRE(words.size() == PORTAL_SHADOW_PACKED_DESCRIPTOR_WORDS + 6);
	CHECK(words[0] == 0);
	CHECK(words[1] == (PORTAL_SHADOW_PACKED_DESCRIPTOR_WORDS | (3u << 26)));
	CHECK(words[2] == 0x3f800000u);
	CHECK(words[3] == words.size());
	CHECK(words[PORTAL_SHADOW_PACKED_DESCRIPTOR_WORDS] == 0x38e38e38u);
	CHECK(words.back() == 0xe38e38e3u);
	CheckRoundtrip(depth, words);
}

TEST_CASE("packed shadow preserves random valid float bits", "[render][portal-shadow-packed]") {
	uint32_t random = 0x719acd35u;
	const auto depth = DepthOf([&](size_t) {
		random ^= random << 13;
		random ^= random >> 17;
		random ^= random << 5;
		return random % 0x3f800001u;
	});
	const auto words = Pack(depth);
	// Valid positive D32 samples need at most 30 delta bits, including zero to one.
	CHECK(words.size() <= PORTAL_SHADOW_PACKED_DESCRIPTOR_WORDS + PORTAL_SHADOW_PACKED_BLOCK_COUNT * 60);
	CheckRoundtrip(depth, words);
}

TEST_CASE("packed shadow refuses noncanonical layouts transactionally", "[render][portal-shadow-packed]") {
	auto words = FirstBlock(3);
	words[PORTAL_SHADOW_PACKED_DESCRIPTOR_WORDS] = 7;
	REQUIRE(ValidPortalShadowPacked(words));
	SECTION("offset into descriptors") {
		words[1] -= 1;
	}
	SECTION("payload gap") {
		words[3] += 1;
	}
	SECTION("aliased block payload") {
		words[3] -= 1;
	}
	SECTION("width above storage word") {
		words[1] = (words[1] & PORTAL_SHADOW_PACKED_OFFSET_MASK) | (33u << 26);
	}
	SECTION("descriptor truncation") {
		words.resize(PORTAL_SHADOW_PACKED_DESCRIPTOR_WORDS - 1);
	}
	SECTION("payload truncation") {
		words.pop_back();
	}
	SECTION("trailing words") {
		words.push_back(0);
	}
	SECTION("nonminimal delta width") {
		words[PORTAL_SHADOW_PACKED_DESCRIPTOR_WORDS] = 1;
	}
	SECTION("base is not minimum") {
		words = FirstBlock(1);
		words[PORTAL_SHADOW_PACKED_DESCRIPTOR_WORDS] = 0xffffffffu;
		words[PORTAL_SHADOW_PACKED_DESCRIPTOR_WORDS + 1] = 0xffffffffu;
	}
	SECTION("invalid constant base") {
		words[2] = 0x80000000u;
	}
	SECTION("wide delta cannot overflow into valid range") {
		words = FirstBlock(32);
		words[0] = 1;
		words[PORTAL_SHADOW_PACKED_DESCRIPTOR_WORDS] = 0xffffffffu;
	}
	CHECK_FALSE(ValidPortalShadowPacked(words));
	std::vector<std::byte> output{std::byte{0x5a}};
	const auto previous = output;
	const auto previousCapacity = output.capacity();
	std::string error;
	CHECK_FALSE(UnpackPortalShadow(words, PORTAL_SHADOW_BYTES, output, error));
	CHECK_FALSE(error.empty());
	CHECK(output == previous);
	CHECK(output.capacity() == previousCapacity);
}

TEST_CASE("packed shadow rejects invalid depth samples", "[render][portal-shadow-packed]") {
	const uint32_t invalid = GENERATE(0x80000000u, 0xbf000000u, 0x3f800001u, 0x7f800000u, 0x7fc00000u);
	CAPTURE(invalid);
	auto depth = DepthOf([](size_t) { return 0u; });
	WriteSample(depth, SAMPLE_COUNT - 1, invalid);
	std::vector<uint32_t> output{0x12345678u};
	const auto previous = output;
	const auto previousCapacity = output.capacity();
	std::string error;
	CHECK_FALSE(PackPortalShadow(depth, PORTAL_SHADOW_PACKED_MAX_BYTES, output, error));
	CHECK_FALSE(error.empty());
	CHECK(output == previous);
	CHECK(output.capacity() == previousCapacity);
}

TEST_CASE("packed shadow admission accounts exact output bytes", "[render][portal-shadow-packed]") {
	const auto depth = DepthOf([](size_t index) { return uint32_t(index % 2) * 0x3f800000u; });
	const auto words = Pack(depth);
	const auto byteCount = words.size() * 4;
	std::vector<uint32_t> packed{77};
	std::string error;
	CHECK_FALSE(PackPortalShadow(depth, byteCount - 1, packed, error));
	CHECK(packed == std::vector<uint32_t>{77});
	REQUIRE(PackPortalShadow(depth, byteCount, packed, error));
	CHECK(packed == words);
	std::vector<std::byte> decoded{std::byte{77}};
	CHECK_FALSE(UnpackPortalShadow(words, PORTAL_SHADOW_BYTES - 1, decoded, error));
	CHECK(decoded == std::vector<std::byte>{std::byte{77}});
	CHECK_FALSE(PackPortalShadow(std::span(depth).first(depth.size() - 1), byteCount, packed, error));
	CHECK(packed == words);
}
