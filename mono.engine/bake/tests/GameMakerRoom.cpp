#include <engine/bake/GameMakerRoom.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
TEST_SUITE_ID("engine.bake.gamemaker_room")
TEST_DEPENDS("engine.bake.image")
using namespace engine::bake;
namespace {
	std::span<const std::byte> Bytes(std::string_view text) {
		return {reinterpret_cast<const std::byte *>(text.data()), text.size()};
	}
	// Independently authored YY resources follow the pinned GMSprite first-frame/first-layer layout.
	constexpr std::string_view Sprite =
		R"({"resourceType":"GMSprite","width":1,"height":1,"sequence":{"xorigin":0,"yorigin":0},"frames":[{"name":"frame-a"}],"layers":[{"name":"layer-a"}]})";
	constexpr std::string_view Room =
		R"({"resourceType":"GMRoom","roomSettings":{"Width":2,"Height":2},"layers":[{"resourceType":"GMRAssetLayer","name":"assets","visible":true,"depth":0,"layers":[],"assets":[{"spriteId":{"path":"sprites/red/red.yy"},"x":1,"y":0,"scaleX":1,"scaleY":1,"rotation":0,"colour":4294967295}]},{"resourceType":"GMRBackgroundLayer","name":"blue","visible":true,"depth":100,"layers":[],"spriteId":null,"colour":4294901760}]})";
	// Independent standard PNG/zlib encoding of one opaque red pixel.
	constexpr std::array<unsigned char, 70> Red{137, 80, 78, 71, 13, 10,  26,  10,	0,	 0,	  0,   13,
												73,	 72, 68, 82, 0,	 0,	  0,   1,	0,	 0,	  0,   1,
												8,	 6,	 0,	 0,	 0,	 31,  21,  196, 137, 0,	  0,   0,
												13,	 73, 68, 65, 84, 120, 156, 99,	248, 207, 192, 240,
												31,	 0,	 5,	 0,	 1,	 255, 137, 153, 61,	 29,  0,   0,
												0,	 0,	 73, 69, 78, 68,  174, 66,	96,	 130};
}
TEST_CASE(
	"GameMaker room composites granted sprite contents over declared background in source layer order",
	"[bake][gamemaker_room]"
) {
	const std::array<GameMakerResource, 2> resources{
		{{"sprites/red/red.yy", Bytes(Sprite)},
		 {"sprites/red/layers/frame-a/layer-a.png",
		  {reinterpret_cast<const std::byte *>(Red.data()), Red.size()}}}
	};
	engine::assets::TextureData image;
	std::string failure;
	REQUIRE(ReadGameMakerRoom(Bytes(Room), resources, image, failure));
	CHECK(image.Width == 2);
	CHECK(image.Height == 2);
	CHECK(std::to_integer<int>(image.Pixels[0]) == 0);
	CHECK(std::to_integer<int>(image.Pixels[2]) == 255);
	CHECK(std::to_integer<int>(image.Pixels[4]) == 255);
	CHECK(std::to_integer<int>(image.Pixels[6]) == 0);
	CHECK(std::to_integer<int>(image.Pixels[11]) == 255);
}
TEST_CASE(
	"GameMaker room missing assets, malformed JSON and count budgets preserve previous image",
	"[bake][gamemaker_room]"
) {
	engine::assets::TextureData image;
	image.Width = 99;
	std::string failure;
	CHECK_FALSE(ReadGameMakerRoom(Bytes(Room), {}, image, failure));
	CHECK(image.Width == 99);
	CHECK(failure.find("not explicitly granted") != std::string::npos);
	CHECK_FALSE(ReadGameMakerRoom(
		Bytes("{\"resourceType\":\"GMRoom\",\"resourceType\":\"GMRoom\"}"), {}, image, failure
	));
	CHECK(image.Width == 99);
	CHECK_FALSE(ReadGameMakerRoom(Bytes(Room), {}, image, failure, 4));
	CHECK(image.Width == 99);
	const auto oversized =
		R"({"resourceType":"GMRoom","roomSettings":{"Width":8193,"Height":1},"layers":[]})";
	CHECK_FALSE(ReadGameMakerRoom(Bytes(oversized), {}, image, failure));
	CHECK(image.Width == 99);
}
TEST_CASE(
	"GameMaker tile previews bind exact names independently of control order", "[bake][gamemaker_room]"
) {
	const auto room =
		R"({"resourceType":"GMRoom","roomSettings":{"Width":2,"Height":1},"layers":[{"resourceType":"GMRTileLayer","name":"foreground","visible":true,"tilesetId":null},{"resourceType":"GMRBackgroundLayer","name":"blue","spriteId":null,"colour":4294901760}]})";
	GameMakerTileOverride edit;
	edit.LayerName = "foreground";
	edit.Preview.emplace();
	edit.Preview->Width = 1;
	edit.Preview->Height = 1;
	edit.Preview->Pixels = {std::byte{255}, std::byte{0}, std::byte{0}, std::byte{255}};
	engine::assets::TextureData image;
	std::string failure;
	REQUIRE(ReadGameMakerRoom(Bytes(room), {}, image, failure, 4096, std::span(&edit, 1)));
	CHECK(image.Pixels[0] == std::byte{255});
	CHECK(image.Pixels[6] == std::byte{255});
	edit.LayerName = "missing";
	CHECK_FALSE(ReadGameMakerRoom(Bytes(room), {}, image, failure, 4096, std::span(&edit, 1)));
	CHECK(image.Pixels[0] == std::byte{255});
	edit.LayerName = "foreground";
	edit.Preview->Pixels.clear();
	CHECK_FALSE(ReadGameMakerRoom(Bytes(room), {}, image, failure, 4096, std::span(&edit, 1)));
	CHECK(image.Pixels[0] == std::byte{255});
}
