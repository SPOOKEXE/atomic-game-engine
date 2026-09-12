#include "TextureAtlasProbe.hpp"

#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>

TEST_SUITE_ID("engine.render.textureatlasprobe")

TEST_CASE("4k atlas plan packs four sources without overlap", "[render][texture-atlas]") {
	const auto plan = engine::render::PlanTextureAtlas(4, 4096);
	REQUIRE(plan.Valid());
	CHECK(plan.PageWidth == 8192);
	CHECK(plan.PageHeight == 8192);
	REQUIRE(plan.Rects.size() == 4);
	for (size_t index = 0; index < plan.Rects.size(); index++) {
		const auto &rect = plan.Rects[index];
		CHECK(rect.Width == 4096);
		CHECK(rect.Height == 4096);
		for (size_t other = index + 1; other < plan.Rects.size(); other++) {
			const auto &candidate = plan.Rects[other];
			const bool separate =
				rect.X + rect.Width <= candidate.X || candidate.X + candidate.Width <= rect.X ||
				rect.Y + rect.Height <= candidate.Y || candidate.Y + candidate.Height <= rect.Y;
			CHECK(separate);
		}
	}
}

TEST_CASE("atlas plan refuses empty and overflowing layouts", "[render][texture-atlas]") {
	CHECK_FALSE(engine::render::PlanTextureAtlas(0, 4096).Valid());
	CHECK_FALSE(engine::render::PlanTextureAtlas(4, 0).Valid());
	CHECK_FALSE(engine::render::PlanTextureAtlas(4, 0x80000000u).Valid());
}
