#include "ContentSequence.hpp"

#include <engine/core/Bytes.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>

TEST_SUITE_ID("client.contentsequence")
TEST_DEPENDS("engine.assets.texturesequence")

TEST_CASE(
	"content admission distinguishes a 257-frame visual sequence from joint animation", "[client][sequence]"
) {
	engine::assets::TextureSequenceData authored;
	authored.Width = 1;
	authored.Height = 1;
	for (size_t frame = 0; frame < 257; ++frame) {
		authored.FrameDurations.push_back((frame & 1) == 0 ? 0.04f : 0.11f);
		authored.Pixels.insert(
			authored.Pixels.end(),
			{static_cast<std::byte>(frame & 255), std::byte{0}, std::byte{0}, std::byte{255}}
		);
	}
	engine::core::ByteWriter writer;
	REQUIRE(engine::assets::TextureSequence::Write(writer, authored));
	engine::assets::TextureSequenceData decoded;
	engine::scene::FlipbookFacts facts;
	CHECK_FALSE(client::ReadSequenceContent("joint.aanim", writer.Bytes(), decoded, facts));
	CHECK_FALSE(client::ReadSequenceContent("frames.aseq", writer.Bytes().first(20), decoded, facts));
	REQUIRE(client::ReadSequenceContent("frames.aseq", writer.Bytes(), decoded, facts));
	CHECK(facts.Side == 0);
	CHECK(facts.Frames == 257);
	CHECK(facts.FrameDurations == authored.FrameDurations);
	CHECK(std::ranges::equal(decoded.FramePixels(256), authored.FramePixels(256)));
	const auto prior = decoded.Pixels;
	CHECK_FALSE(client::ReadSequenceContent("frames.aseq", writer.Bytes().first(20), decoded, facts));
	CHECK(decoded.Pixels == prior);
	CHECK(facts.Frames == 257);
}
