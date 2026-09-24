#include <engine/assets/TextureSequence.hpp>
#include <engine/core/Bytes.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

TEST_SUITE_ID("engine.assets.texturesequence")

TEST_CASE("a 257-frame sequence keeps frame order and exact authored durations", "[assets][sequence]") {
	engine::assets::TextureSequenceData source;
	source.Width = 2;
	source.Height = 1;
	for (uint32_t frame = 0; frame < 257; ++frame) {
		source.FrameDurations.push_back((frame & 1) == 0 ? 0.04f : 0.11f);
		for (uint32_t pixel = 0; pixel < 2; ++pixel) {
			source.Pixels.push_back(static_cast<std::byte>(frame & 255));
			source.Pixels.push_back(static_cast<std::byte>(pixel));
			source.Pixels.push_back(std::byte{0});
			source.Pixels.push_back(std::byte{255});
		}
	}
	REQUIRE(source.IsValid());
	engine::core::ByteWriter writer;
	REQUIRE(engine::assets::TextureSequence::Write(writer, source));
	engine::core::ByteReader reader(writer.Bytes());
	engine::assets::TextureSequenceData decoded;
	REQUIRE(engine::assets::TextureSequence::Read(reader, decoded));
	CHECK(reader.AtEnd());
	CHECK(decoded.FrameDurations == source.FrameDurations);
	CHECK(std::ranges::equal(decoded.FramePixels(0), source.FramePixels(0)));
	CHECK(std::ranges::equal(decoded.FramePixels(256), source.FramePixels(256)));
	CHECK(decoded.Pixels == source.Pixels);
}

TEST_CASE("bad sequence payloads leave the destination intact", "[assets][sequence]") {
	engine::assets::TextureSequenceData source;
	source.Width = 1;
	source.Height = 1;
	source.FrameDurations = {0.04f};
	source.Pixels = {std::byte{1}, std::byte{2}, std::byte{3}, std::byte{255}};
	engine::core::ByteWriter writer;
	REQUIRE(engine::assets::TextureSequence::Write(writer, source));
	engine::assets::TextureSequenceData held = source;
	const auto full = writer.Bytes();
	for (size_t length : {size_t(0), size_t(12), full.size() - 1}) {
		engine::core::ByteReader reader(full.first(length));
		CHECK_FALSE(engine::assets::TextureSequence::Read(reader, held));
		CHECK(held.Pixels == source.Pixels);
		CHECK(held.FrameDurations == source.FrameDurations);
	}
	engine::assets::TextureSequenceData invalid = source;
	invalid.FrameDurations = {0.0f};
	engine::core::ByteWriter refused;
	CHECK_FALSE(engine::assets::TextureSequence::Write(refused, invalid));
	CHECK(refused.Size() == 0);
}
