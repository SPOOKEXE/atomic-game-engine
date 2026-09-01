#include <engine/assets/Animation.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

TEST_SUITE_ID("engine.assets.animation")

namespace {
	engine::assets::AnimationData Clip() {
		engine::assets::AnimationData clip;
		clip.Duration = 2.0f;
		clip.Channels = {{1, {{0.0f, {}}, {2.0f, engine::core::CFrame(engine::core::Vector3{4, 0, 0})}}}};
		return clip;
	}
}

TEST_CASE("animation clips round trip", "[assets][animation]") {
	const engine::assets::AnimationData source = Clip();
	engine::core::ByteWriter writer;
	REQUIRE(engine::assets::Animation::Write(writer, source));

	engine::assets::AnimationData read;
	engine::core::ByteReader reader(writer.Bytes());
	REQUIRE(engine::assets::Animation::Read(reader, read));
	REQUIRE(read.Channels.size() == 1);
	CHECK(read.Duration == 2.0f);
	CHECK(read.Channels[0].Joint == 1);
	CHECK(read.Channels[0].Keys[1].Transform.Position.X == 4.0f);
}

TEST_CASE("animation clips reject hostile counts and unordered keys", "[assets][animation]") {
	auto clip = Clip();
	clip.Channels[0].Keys[1].Time = 0.0f;
	CHECK_FALSE(clip.IsValid());

	engine::core::ByteWriter writer;
	writer.WriteUInt32(engine::assets::Animation::MAGIC);
	writer.WriteUInt16(engine::assets::Animation::VERSION);
	writer.WriteFloat(1.0f);
	writer.WriteUInt32(engine::assets::Animation::MAXIMUM_CHANNELS + 1);
	engine::assets::AnimationData read;
	engine::core::ByteReader reader(writer.Bytes());
	CHECK_FALSE(engine::assets::Animation::Read(reader, read));
}
