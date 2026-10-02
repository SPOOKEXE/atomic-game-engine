#include <engine/bake/GifSequence.hpp>
#include <engine/bake/GifWrite.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>

TEST_SUITE_ID("engine.bake.gifwrite")
using namespace engine::bake;
TEST_CASE("native GIF round trips palette colours transparency and unequal delays", "[bake][gif_write]") {
	std::vector<uint8_t> first(401 * 4), second(401 * 4);
	for (size_t i = 0; i < 401; ++i) {
		first[i * 4] = 255;
		first[i * 4 + 3] = 255;
		second[i * 4 + 2] = 255;
		second[i * 4 + 3] = 255;
	}
	second[3] = 0;
	std::array<GifFrame, 2> frames{{{401, 1, first, 3}, {401, 1, second, 7}}};
	std::vector<std::byte> bytes, repeat;
	std::string error;
	REQUIRE(WriteGif(frames, 3, 1024 * 1024, bytes, error));
	REQUIRE(WriteGif(frames, 3, 1024 * 1024, repeat, error));
	CHECK(bytes == repeat);
	engine::assets::TextureSequenceData decoded;
	const bool ok = ReadGifSequence(bytes, decoded, error);
	INFO(error);
	REQUIRE(ok);
	REQUIRE(decoded.FrameDurations.size() == 2);
	CHECK(decoded.FrameDurations[0] == 0.03f);
	CHECK(decoded.FrameDurations[1] == 0.07f);
	CHECK(decoded.FramePixels(0)[0] == std::byte{255});
	CHECK(decoded.FramePixels(1)[3] == std::byte{0});
	CHECK(decoded.FramePixels(1)[6] == std::byte{255});
	CHECK(decoded.FramePixels(1)[7] == std::byte{255});
	// Three LZW clear intervals and more than one sub-block were decoded above.
	CHECK(bytes[29] == std::byte{0});
	CHECK(bytes[30] == std::byte{0});
}
TEST_CASE("native GIF quality preserves exact small palettes at highest quality", "[bake][gif_write]") {
	const std::array<uint8_t, 8> pixels{31, 127, 191, 255, 200, 100, 10, 255};
	const std::array<GifFrame, 1> frames{{{2, 1, pixels, 1}}};
	std::string error;
	std::vector<std::byte> bytes;
	for (uint8_t quality = 0; quality < 4; ++quality) {
		REQUIRE(WriteGif(frames, quality, 65536, bytes, error));
		engine::assets::TextureSequenceData decoded;
		REQUIRE(ReadGifSequence(bytes, decoded, error));
		if (quality == 3)
			for (size_t i = 0; i < pixels.size(); ++i)
				CHECK(decoded.Pixels[i] == static_cast<std::byte>(pixels[i]));
		else
			CHECK(decoded.Pixels[0] != static_cast<std::byte>(pixels[0]));
	}
	std::vector<uint8_t> varied(300 * 4);
	for (size_t i = 0; i < 300; ++i) {
		varied[i * 4] = static_cast<uint8_t>(i);
		varied[i * 4 + 1] = static_cast<uint8_t>(i >> 8);
		varied[i * 4 + 3] = 255;
	}
	const std::array<GifFrame, 1> many{{{300, 1, varied, 2}}};
	REQUIRE(WriteGif(many, 3, 65536, bytes, error));
	engine::assets::TextureSequenceData decoded;
	REQUIRE(ReadGifSequence(bytes, decoded, error));
	CHECK(decoded.Pixels.size() == varied.size());
	const std::array<uint8_t, 4> transparent{200, 100, 10, 0};
	const std::array<GifFrame, 1> blank{{{1, 1, transparent, 1}}};
	REQUIRE(WriteGif(blank, 3, 65536, bytes, error));
	REQUIRE(ReadGifSequence(bytes, decoded, error));
	CHECK(decoded.Pixels[3] == std::byte{0});
}
TEST_CASE(
	"native GIF rejects invalid dimensions delays quality and byte caps atomically", "[bake][gif_write]"
) {
	const std::array<uint8_t, 4> pixels{0, 0, 0, 255};
	std::array<GifFrame, 1> frames{{{1, 1, pixels, 1}}};
	std::vector<std::byte> output{std::byte{42}};
	const auto old = output;
	std::string error;
	CHECK_FALSE(WriteGif(frames, 3, 1, output, error));
	CHECK(output == old);
	CHECK_FALSE(WriteGif(frames, 4, 65536, output, error));
	CHECK(output == old);
	frames[0].DelayCentiseconds = 0;
	CHECK_FALSE(WriteGif(frames, 3, 65536, output, error));
	CHECK(output == old);
	frames[0].DelayCentiseconds = 1;
	frames[0].Width = 65536;
	CHECK_FALSE(WriteGif(frames, 3, 65536, output, error));
	CHECK(output == old);
	frames[0].Width = 2;
	CHECK_FALSE(WriteGif(frames, 3, 65536, output, error));
	CHECK(output == old);
}
