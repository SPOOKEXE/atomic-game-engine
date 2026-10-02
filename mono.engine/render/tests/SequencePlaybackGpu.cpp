#include "RenderFixture.hpp"

#include <engine/render/TextureTable.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

TEST_SUITE_ID("engine.render.sequenceplayback_gpu")
TEST_DEPENDS("engine.render.fixtures")
TEST_DEPENDS("engine.render.sequenceplayback")

TEST_CASE(
	"large sequences publish authored frames in their owner's texture namespace", "[render][sequence][gpu]"
) {
	using namespace engine;
	render::test::FixtureDevice fixture;
	REQUIRE(fixture.VideoReady);
	REQUIRE(fixture.Render.Initialise(nullptr, 2, true));
	REQUIRE(fixture.Render.IsHeadless());
	const core::Name name("live-sequence.aseq"), owner("live-sequence-owner"), other("other-owner");
	assets::TextureSequenceData sequence;
	sequence.Width = 1;
	sequence.Height = 1;
	sequence.FrameDurations.assign(257, .25f);
	for (size_t frame = 0; frame < 257; ++frame) {
		sequence.Pixels.insert(
			sequence.Pixels.end(),
			{static_cast<std::byte>(frame & 255),
			 static_cast<std::byte>(frame >> 8),
			 std::byte{0},
			 std::byte{255}}
		);
	}
	REQUIRE(fixture.Render.AddTextureSequence(name, std::move(sequence), owner));
	assets::TextureData copied;
	REQUIRE(fixture.Render.CopyTexture(name, copied, 4096, owner) == render::TextureCopyStatus::Copied);
	CHECK(copied.FlipbookSide == 32);
	CHECK(copied.FlipbookFrames == 257);
	CHECK(copied.FlipbookFrameDurations.size() == 257);
	CHECK(copied.Width == 32);
	CHECK(copied.Height == 32);
	CHECK(copied.Pixels[(8 * 32) * 4 + 1] == std::byte{1});
	CHECK(fixture.Render.CopyTexture(name, copied, 4096, other) == render::TextureCopyStatus::Missing);
	const auto finalCell = fixture.Render.TextureCell(name, 64, owner);
	CHECK(finalCell.OffsetV == .25f);
	CHECK(finalCell.OffsetU == 0);
	CHECK(fixture.Render.TextureCell(name, 64.25, owner).OffsetV == 0);
	fixture.Render.DropContentOwner(owner);
	CHECK(fixture.Render.CopyTexture(name, copied, 4, owner) == render::TextureCopyStatus::Missing);
}
