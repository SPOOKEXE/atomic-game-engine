#include <engine/assets/TexturePixel.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <span>
#include <vector>

TEST_SUITE_ID("engine.assets.texturepixel")
using namespace engine::assets;

TEST_CASE("typed pixel decoding reads each format from the final pixel", "[assets][texturepixel]") {
	struct Fixture {
		TextureFormat Format;
		std::vector<std::byte> Bytes;
		std::array<float, 4> Expected;
	};
	const std::array fixtures{
		Fixture{
			TextureFormat::RGBA8,
			{std::byte{9},
			 std::byte{8},
			 std::byte{7},
			 std::byte{6},
			 std::byte{0},
			 std::byte{64},
			 std::byte{128},
			 std::byte{255}},
			{0, 64.0f / 255, 128.0f / 255, 1}
		},
		Fixture{
			TextureFormat::RGBA8_LINEAR,
			{std::byte{9},
			 std::byte{8},
			 std::byte{7},
			 std::byte{6},
			 std::byte{1},
			 std::byte{2},
			 std::byte{3},
			 std::byte{4}},
			{1.0f / 255, 2.0f / 255, 3.0f / 255, 4.0f / 255}
		},
		Fixture{
			TextureFormat::R8, {std::byte{17}, std::byte{128}}, {128.0f / 255, 128.0f / 255, 128.0f / 255, 1}
		},
		Fixture{
			TextureFormat::RGBA4_UNORM,
			{std::byte{0}, std::byte{0}, std::byte{0x34}, std::byte{0x12}},
			{4.0f / 15, 3.0f / 15, 2.0f / 15, 1.0f / 15}
		},
		Fixture{
			TextureFormat::RGBA4_SRGB,
			{std::byte{0}, std::byte{0}, std::byte{0x34}, std::byte{0x12}},
			{4.0f / 15, 3.0f / 15, 2.0f / 15, 1.0f / 15}
		},
		Fixture{
			TextureFormat::RGBA16_FLOAT,
			{std::byte{0},
			 std::byte{0},
			 std::byte{0},
			 std::byte{0},
			 std::byte{0},
			 std::byte{0},
			 std::byte{0},
			 std::byte{0},
			 std::byte{0},
			 std::byte{0x40},
			 std::byte{0},
			 std::byte{0x3c},
			 std::byte{0},
			 std::byte{0xc0},
			 std::byte{0},
			 std::byte{0x38}},
			{1, 1, 0, .5f}
		},
		Fixture{
			TextureFormat::RGBA32_FLOAT,
			{std::byte{0}, std::byte{0},	std::byte{0},	 std::byte{0},	  std::byte{0},
			 std::byte{0}, std::byte{0},	std::byte{0},	 std::byte{0},	  std::byte{0},
			 std::byte{0}, std::byte{0},	std::byte{0},	 std::byte{0},	  std::byte{0},
			 std::byte{0}, std::byte{0},	std::byte{0},	 std::byte{0x80}, std::byte{0x3f},
			 std::byte{0}, std::byte{0},	std::byte{0x80}, std::byte{0xbf}, std::byte{0},
			 std::byte{0}, std::byte{0x80}, std::byte{0x3f}, std::byte{0},	  std::byte{0},
			 std::byte{0}, std::byte{0x3f}},
			{1, 0, 1, .5f}
		},
		Fixture{
			TextureFormat::R16_FLOAT,
			{std::byte{0}, std::byte{0}, std::byte{0}, std::byte{0xc0}},
			{0, 0, 0, 1}
		},
		Fixture{
			TextureFormat::R32_FLOAT,
			{std::byte{0},
			 std::byte{0},
			 std::byte{0},
			 std::byte{0},
			 std::byte{0},
			 std::byte{0},
			 std::byte{0x80},
			 std::byte{0x3f}},
			{1, 1, 1, 1}
		},
	};
	for (const Fixture &fixture : fixtures) {
		const size_t stride = BytesPerPixel(fixture.Format);
		REQUIRE(fixture.Bytes.size() == 2 * stride);
		std::array<float, 4> actual{};
		REQUIRE(LoadTexturePixelForDisplay(
			fixture.Format, std::span<const std::byte>(fixture.Bytes).subspan(stride, stride), actual
		));
		for (size_t channel = 0; channel < actual.size(); channel++)
			CHECK(std::abs(actual[channel] - fixture.Expected[channel]) < 1e-6f);
	}
}

TEST_CASE("typed pixels retain signed HDR until the display boundary", "[assets][texturepixel]") {
	TexturePixel half{};
	const std::array<std::byte, 8> halfBytes{
		std::byte{0},
		std::byte{0xc0},
		std::byte{0},
		std::byte{0x40},
		std::byte{0},
		std::byte{0x44},
		std::byte{0},
		std::byte{0x3c}
	};
	REQUIRE(LoadTexturePixel(TextureFormat::RGBA16_FLOAT, halfBytes, half));
	CHECK((half == TexturePixel{-2, 2, 4, 1}));
	std::array<float, 4> display{};
	REQUIRE(LoadTexturePixelForDisplay(TextureFormat::RGBA16_FLOAT, halfBytes, display));
	CHECK((display == std::array<float, 4>{0, 1, 1, 1}));

	TexturePixel single{};
	const std::array<std::byte, 2> rHalf{std::byte{0}, std::byte{0xc0}};
	REQUIRE(LoadTexturePixel(TextureFormat::R16_FLOAT, rHalf, single));
	CHECK((single == TexturePixel{-2, 0, 0, 1}));

	std::array<std::byte, 2> packed{};
	const TexturePixel ties{.5, .5, .5, .5};
	REQUIRE(StoreTexturePixel(TextureFormat::RGBA4_UNORM, ties, packed));
	CHECK((packed == std::array<std::byte, 2>{std::byte{0x88}, std::byte{0x88}}));
	CHECK_FALSE(StoreTexturePixel(TextureFormat::RGBA4_UNORM, ties, std::span<std::byte>(packed).first(1)));
}
