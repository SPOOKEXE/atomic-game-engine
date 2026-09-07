#include "FramePreparation.hpp"

#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>

TEST_SUITE_ID("engine.render.framepreparation")

TEST_CASE(
	"resolved fallback views stay adjacent across interleaved world requests", "[render][frame-prefix]"
) {
	const int fallback = 0;
	const int authored = 0;
	const std::array<engine::render::FrameViewIdentity, 4> views = {{
		{7, &fallback},
		{9, &fallback},
		{7, &authored},
		{7, &fallback},
	}};
	const auto groups = engine::render::GroupFrameViews(views, false);
	REQUIRE(groups.size() == 3);
	CHECK(groups[0].Identity.World == 7);
	CHECK(groups[0].Identity.Pipeline == &fallback);
	CHECK(groups[0].Views == std::vector<size_t>{0, 3});
	CHECK(groups[1].Identity.World == 9);
	CHECK(groups[1].Views == std::vector<size_t>{1});
	CHECK(groups[2].Identity.Pipeline == &authored);
	CHECK(groups[2].Views == std::vector<size_t>{2});

	const auto presented = engine::render::GroupFrameViews(views, true);
	REQUIRE(presented.size() == 3);
	CHECK(presented[0].Views == std::vector<size_t>{1});
	CHECK(presented[1].Views == std::vector<size_t>{2});
	CHECK(presented[2].Views == std::vector<size_t>{0, 3});
	CHECK(engine::render::GroupFrameViews({}, true).empty());
	const std::array<engine::render::FrameViewIdentity, 1> one = {{{7, &fallback}}};
	const auto single = engine::render::GroupFrameViews(one, true);
	REQUIRE(single.size() == 1);
	CHECK(single[0].Views == std::vector<size_t>{0});
}

TEST_CASE("only completed scene setup consumes a pipeline's frame preparation", "[render][frame-prefix]") {
	engine::render::FramePreparation prepared;
	const int pipeline = 0;
	CHECK(prepared.NeedsFrame(&pipeline));
	CHECK(prepared.NeedsWorld(&pipeline, 7));

	// An idle or failed first view does not acknowledge setup.
	prepared.Complete(&pipeline, 7, false);
	CHECK(prepared.NeedsFrame(&pipeline));
	CHECK(prepared.NeedsWorld(&pipeline, 7));
	prepared.Complete(&pipeline, 7, true);
	CHECK_FALSE(prepared.NeedsFrame(&pipeline));
	CHECK_FALSE(prepared.NeedsWorld(&pipeline, 7));
	CHECK(prepared.NeedsWorld(&pipeline, 9));

	prepared.Complete(&pipeline, 9, true);
	CHECK_FALSE(prepared.NeedsFrame(&pipeline));
	CHECK_FALSE(prepared.NeedsWorld(&pipeline, 9));
	prepared.Clear();
	CHECK(prepared.NeedsFrame(&pipeline));
	CHECK(prepared.NeedsWorld(&pipeline, 7));
}

TEST_CASE(
	"setup identity follows the resolved pipeline rather than requested aliases", "[render][frame-prefix]"
) {
	engine::render::FramePreparation prepared;
	const int fallback = 0;
	const int authored = 0;
	const void *missingFirst = &fallback;
	const void *missingSecond = &fallback;
	prepared.Complete(missingFirst, 7, true);
	CHECK_FALSE(prepared.NeedsFrame(missingSecond));
	CHECK_FALSE(prepared.NeedsWorld(missingSecond, 7));
	CHECK(prepared.NeedsWorld(missingSecond, 9));
	CHECK(prepared.NeedsFrame(&authored));
	CHECK(prepared.NeedsWorld(&authored, 7));
	prepared.Complete(&authored, 7, true);
	prepared.Complete(&authored, 7, true);
	CHECK_FALSE(prepared.NeedsFrame(&authored));
	CHECK_FALSE(prepared.NeedsWorld(&authored, 7));
}
