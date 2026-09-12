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

TEST_CASE("atlas planner keeps a sixteen-source 4k page disjoint", "[render][texture-atlas]") {
	const auto plan = engine::render::PlanTextureAtlas(16, 4096);
	REQUIRE(plan.Valid());
	CHECK(plan.PageWidth == 16384);
	CHECK(plan.PageHeight == 16384);
	REQUIRE(plan.Rects.size() == 16);
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

TEST_CASE("atlas residency only counts committed device work", "[render][texture-atlas]") {
	engine::render::TextureAtlasResidency atlas(engine::render::PlanTextureAtlas(4, 4));
	const auto cold = atlas.Request(2);
	REQUIRE(cold.Valid());
	CHECK(cold.AllocatePage);
	CHECK(cold.Upload);
	CHECK(atlas.Usage().Misses == 1);

	atlas.PageAllocated(cold.Page);
	atlas.Copied(2);
	CHECK(atlas.Usage().PageAllocations == 1);
	CHECK(atlas.Usage().CopyCalls == 1);

	const auto warm = atlas.Request(2);
	CHECK(warm.Valid());
	CHECK_FALSE(warm.AllocatePage);
	CHECK_FALSE(warm.Upload);
	CHECK(atlas.Usage().Hits == 1);
}

TEST_CASE("atlas refuses mip layouts without gutters", "[render][texture-atlas]") {
	CHECK_FALSE(engine::render::PlanTextureAtlas(4, 4, 2).Valid());
}
