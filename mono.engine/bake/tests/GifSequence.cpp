#include <engine/bake/GifSequence.hpp>
#include <engine/bake/Image.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

TEST_SUITE_ID("engine.bake.gifsequence")
TEST_DEPENDS("engine.assets.texturesequence")

namespace {
	std::vector<std::byte> AnimatedGif(const std::vector<uint16_t> &delays) {
		std::vector<uint8_t> bytes{
			'G',
			'I',
			'F',
			'8',
			'9',
			'a',
			0x02,
			0x00,
			0x01,
			0x00,
			0x80,
			0x00,
			0x00,
			0xFF,
			0x00,
			0x00,
			0x00,
			0x00,
			0xFF
		};
		for (const uint16_t delay : delays) {
			bytes.insert(
				bytes.end(),
				{0x21,
				 0xF9,
				 0x04,
				 0x00,
				 static_cast<uint8_t>(delay & 0xFF),
				 static_cast<uint8_t>(delay >> 8),
				 0x00,
				 0x00}
			);
			bytes.insert(
				bytes.end(),
				{0x2C, 0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x01, 0x00, 0x00, 0x02, 0x02, 0x44, 0x0A, 0x00}
			);
		}
		bytes.push_back(0x3B);
		std::vector<std::byte> result;
		result.reserve(bytes.size());
		for (uint8_t byte : bytes)
			result.push_back(static_cast<std::byte>(byte));
		return result;
	}
}

TEST_CASE("the 257th GIF frame and every delay enter a sequence", "[bake][sequence]") {
	std::vector<uint16_t> delays;
	for (size_t index = 0; index < 257; ++index)
		delays.push_back((index & 1) == 0 ? 4 : 11);
	const auto gif = AnimatedGif(delays);
	engine::assets::TextureData atlas;
	std::string failure;
	CHECK_FALSE(engine::bake::ReadImage(gif, atlas, failure));
	CHECK(failure.find("256") != std::string::npos);
	engine::assets::TextureSequenceData sequence;
	failure.clear();
	REQUIRE(engine::bake::ReadGifSequence(gif, sequence, failure));
	CHECK(failure.empty());
	CHECK(sequence.Width == 2);
	CHECK(sequence.Height == 1);
	REQUIRE(sequence.FrameDurations.size() == 257);
	CHECK(sequence.FrameDurations[0] == 0.04f);
	CHECK(sequence.FrameDurations[1] == 0.11f);
	CHECK(sequence.FrameDurations[256] == 0.04f);
	CHECK(sequence.FramePixels(256).size() == 8);
	CHECK(sequence.IsValid());
}

TEST_CASE("a truncated sequence GIF leaves its prior result intact", "[bake][sequence]") {
	const auto whole = AnimatedGif({4, 11});
	engine::assets::TextureSequenceData held;
	std::string failure;
	REQUIRE(engine::bake::ReadGifSequence(whole, held, failure));
	const auto before = held.Pixels;
	const auto durations = held.FrameDurations;
	CHECK_FALSE(
		engine::bake::ReadGifSequence(
			std::span<const std::byte>(whole).first(whole.size() / 2), held, failure
		)
	);
	CHECK(held.Pixels == before);
	CHECK(held.FrameDurations == durations);
	CHECK_FALSE(
		engine::bake::ReadGifSequence(
			std::span<const std::byte>(whole).first(whole.size() - 1), held, failure
		)
	);
	CHECK(failure.find("trailer") != std::string::npos);
	CHECK(held.Pixels == before);
}
