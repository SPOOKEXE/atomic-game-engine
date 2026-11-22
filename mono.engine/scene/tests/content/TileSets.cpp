#include <engine/scene/content/TileSets.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

TEST_SUITE_ID("engine.scene.tilesets");

namespace {
	using engine::core::Name;
	using namespace engine::scene;

	TEST_CASE("UDIM sets round-trip through one slot name", "[scene][udim]") {
		const std::vector<UdimTile> tiles{
			{1003, Name("imagegraph-instance://skin#albedo3")}, {1001, Name("editable-image://42")}
		};
		const std::string composed = ComposeTileSet(tiles);
		CHECK(composed == "udim:1001=editable-image://42|1003=imagegraph-instance://skin#albedo3");
		CHECK(IsTileSet(composed));
		const auto expanded = ExpandTileSet(composed);
		REQUIRE(expanded.size() == 2);
		CHECK(expanded[0] == tiles[1]);
		CHECK(expanded[1] == tiles[0]);

		const auto prefix = ExpandTileSet("udim://body/albedo");
		REQUIRE(prefix.size() == UDIM_PREFIX_LAST_TILE - UDIM_FIRST_TILE + 1);
		CHECK(prefix.front().Texture == Name("body/albedo.1001"));
		CHECK(IsTileSet("udim://body/albedo"));

		CHECK_FALSE(IsTileSet("body/albedo.atex"));
		CHECK(ExpandTileSet("udim:1001=a|1001=b").empty());
		CHECK(ExpandTileSet("udim:999=a").empty());
		CHECK(ExpandTileSet("udim:1001=").empty());
		CHECK(ComposeTileSet(std::vector<UdimTile>{{1001, Name("a|b")}}).empty());
		CHECK(ComposeTileSet(std::vector<UdimTile>{{1001, Name("a")}, {1001, Name("b")}}).empty());
	}
}
