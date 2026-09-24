#include "SequencePlayback.hpp"

#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

TEST_SUITE_ID("engine.render.sequenceplayback")
TEST_DEPENDS("engine.assets.texturesequence")

namespace {
	engine::assets::TextureSequenceData Sequence(uint8_t colour, size_t frames = 257) {
		engine::assets::TextureSequenceData data;
		data.Width = 1;
		data.Height = 1;
		for (size_t frame = 0; frame < frames; ++frame) {
			data.FrameDurations.push_back((frame & 1) == 0 ? 0.04f : 0.11f);
			data.Pixels.insert(
				data.Pixels.end(),
				{static_cast<std::byte>(colour),
				 static_cast<std::byte>(frame & 255),
				 std::byte{0},
				 std::byte{255}}
			);
		}
		return data;
	}
}

TEST_CASE("a 4096-frame sequence reaches its final logical frame", "[render][sequence]") {
	engine::render::SequencePlayback player;
	const engine::core::Name name("maximum.aseq"), owner("world-a");
	auto source = Sequence(7, 4096);
	float total = 0.0f;
	for (float duration : source.FrameDurations)
		total += duration;
	REQUIRE(player.Admit(name, owner, std::move(source)));
	uint32_t seen = 0;
	REQUIRE(player.Advance(total - 0.05, 4, [&](auto, auto, const auto &, uint32_t frame) {
		seen = frame;
		return true;
	}) == 1);
	CHECK(seen == 4095);
}

TEST_CASE("a 257-frame sequence selects exact delays and wraps", "[render][sequence]") {
	engine::render::SequencePlayback player;
	const engine::core::Name name("frames.aseq");
	const engine::core::Name owner("world-a");
	REQUIRE(player.Admit(name, owner, Sequence(7)));
	std::vector<uint32_t> seen;
	const auto publish = [&](auto, auto, const auto &, uint32_t frame) {
		seen.push_back(frame);
		return true;
	};
	CHECK(player.Advance(0.0, 4, publish) == 1);
	CHECK(player.Advance(0.03, 4, publish) == 0);
	CHECK(player.Advance(0.05, 4, publish) == 1);
	CHECK(player.Advance(0.16, 4, publish) == 1);
	CHECK(player.Advance(19.22, 4, publish) == 1);
	CHECK(player.Advance(19.26, 4, publish) == 1);
	CHECK(seen == std::vector<uint32_t>{0, 1, 2, 256, 0});
}

TEST_CASE("bounded upload work defers and eventually publishes each owner", "[render][sequence]") {
	engine::render::SequencePlayback player(16 * 1024);
	const engine::core::Name name("shared.aseq");
	const engine::core::Name owners[]{
		engine::core::Name("world-a"), engine::core::Name("world-b"), engine::core::Name("world-c")
	};
	for (const auto owner : owners)
		REQUIRE(player.Admit(name, owner, Sequence(3)));
	std::vector<uint32_t> seenOwners;
	const auto publish = [&](auto, engine::core::Name owner, const auto &, uint32_t) {
		seenOwners.push_back(owner.Id());
		return true;
	};
	CHECK(player.Advance(0.0, 8, publish) == 2);
	CHECK(player.Advance(0.0, 8, publish) == 1);
	CHECK(seenOwners.size() == 3);
	CHECK(seenOwners[0] != seenOwners[1]);
	CHECK(seenOwners[1] != seenOwners[2]);
	CHECK(player.Advance(0.0, 8, publish) == 0);
	CHECK(player.DropOwner(owners[1]) == 1);
	CHECK_FALSE(player.Contains(name, owners[1]));
	CHECK(player.Contains(name, owners[0]));
}

TEST_CASE("failed replacement keeps last published sequence and memory is bounded", "[render][sequence]") {
	engine::render::SequencePlayback player(7 * 1024);
	const engine::core::Name name("frames.aseq");
	const engine::core::Name owner("world-a");
	REQUIRE(player.Admit(name, owner, Sequence(1)));
	REQUIRE(player.Advance(0.0, 4, [](auto, auto, const auto &, uint32_t) { return true; }) == 1);
	const uint64_t previous = player.PublishedSignature();
	REQUIRE(player.Admit(name, owner, Sequence(2)));
	CHECK(player.Advance(0.05, 4, [](auto, auto, const auto &, uint32_t) { return false; }) == 0);
	CHECK(player.PublishedSignature() == previous);
	CHECK(player.RetainedBytes() > 0);
	CHECK_FALSE(player.Admit(engine::core::Name("other.aseq"), owner, Sequence(3)));
	CHECK(player.Advance(0.05, 4, [](auto, auto, const auto &, uint32_t) { return true; }) == 1);
	CHECK(player.PublishedSignature() != previous);
	CHECK(player.Drop(name, owner));
	CHECK(player.RetainedBytes() == 0);
}
