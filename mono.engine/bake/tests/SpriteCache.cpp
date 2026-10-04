#include <engine/bake/SpriteCache.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <nlohmann/json.hpp>
TEST_SUITE_ID("engine.bake.spritecache")
using namespace engine::bake;
namespace {
	constexpr std::string_view Rgba =
		R"cache([{"width":1,"height":1,"buffer":"eJzjUbLwAwABWAC1"},{"width":2,"height":1,"buffer":"eJw7waXB8L/B4T8ADnkDuQ=="}])cache";
	constexpr std::string_view Bgra =
		R"cache([{"width":1,"height":1,"buffer":"eJyzUOLxAwABsAC1"},{"width":2,"height":1,"buffer":"eJzT4DrB4NDw/z8AC7sDuQ=="}])cache";
	constexpr std::string_view Short =
		R"cache([{"width": 1, "height": 1, "buffer": "eJzjUbIAAACjAGc="}])cache";
	constexpr std::string_view Long =
		R"cache([{"width": 1, "height": 1, "buffer": "eJzjUbLwiwAAAmUBDQ=="}])cache";
	constexpr std::string_view Trailing =
		R"cache([{"width": 1, "height": 1, "buffer": "eJzjUbLwAwABWAC1WA=="}])cache";

	constexpr uint64_t Bytes = 4 * 1024 * 1024;
	std::vector<SpriteCacheFrame> Expected() {
		return {{1, 1, {12, 34, 56, 78}}, {2, 1, {200, 10, 40, 0, 255, 128, 64, 255}}};
	}
}
TEST_CASE("Source sprite cache reads independent zlib/base64 literal RGBA and BGRA bytes", "[sprite_cache]") {
	std::vector<SpriteCacheFrame> frames;
	std::string failure;
	REQUIRE(ReadSpriteCache(Rgba, SpriteCacheLayout::Rgba8TopDown, frames, failure, Bytes));
	CHECK(frames == Expected());
	REQUIRE(ReadSpriteCache(Bgra, SpriteCacheLayout::Bgra8TopDown, frames, failure, Bytes));
	CHECK(frames == Expected());
	REQUIRE(ReadSpriteCache(Bgra, SpriteCacheLayout::Rgba8TopDown, frames, failure, Bytes));
	CHECK(frames[0].Rgba == std::vector<uint8_t>{56, 34, 12, 78});
	CHECK(frames[1].Rgba == std::vector<uint8_t>{40, 10, 200, 0, 64, 128, 255, 255});
}
TEST_CASE(
	"Source sprite cache writer preserves dimensions channels and empty array semantics", "[sprite_cache]"
) {
	const auto frames = Expected();
	std::string failure, text = "prior";
	REQUIRE(WriteSpriteCache(frames, SpriteCacheLayout::Bgra8TopDown, text, failure, Bytes));
	const auto json = nlohmann::json::parse(text);
	REQUIRE(json.is_array());
	REQUIRE(json.size() == 2);
	CHECK(json[0]["width"] == 1);
	CHECK(json[1]["width"] == 2);
	CHECK(json[1]["height"] == 1);
	std::vector<SpriteCacheFrame> decoded;
	REQUIRE(ReadSpriteCache(text, SpriteCacheLayout::Bgra8TopDown, decoded, failure, Bytes));
	CHECK(decoded == frames);
	REQUIRE(ReadSpriteCache(text, SpriteCacheLayout::Rgba8TopDown, decoded, failure, Bytes));
	CHECK(decoded[0].Rgba == std::vector<uint8_t>{56, 34, 12, 78});
	REQUIRE(WriteSpriteCache({}, SpriteCacheLayout::Rgba8TopDown, text, failure, Bytes));
	CHECK(text == "[]");
	REQUIRE(ReadSpriteCache(text, SpriteCacheLayout::Rgba8TopDown, decoded, failure, Bytes));
	CHECK(decoded.empty());
}
TEST_CASE(
	"Source sprite cache refusals retain previous frames and encoded text atomically", "[sprite_cache]"
) {
	auto frames = Expected();
	const auto prior = frames;
	std::string failure;
	for (auto invalid :
		 {Short,
		  Long,
		  Trailing,
		  std::string_view{"[[]]"},
		  std::string_view{"[{\"width\":1,\"height\":1,\"buffer\":\"====\"}]"},
		  std::string_view{"[{\"width\":0,\"height\":1,\"buffer\":\"AA==\"}]"}}) {
		CHECK_FALSE(ReadSpriteCache(invalid, SpriteCacheLayout::Rgba8TopDown, frames, failure, Bytes));
		CHECK(frames == prior);
		CHECK_FALSE(failure.empty());
	}
	CHECK_FALSE(ReadSpriteCache(Rgba, static_cast<SpriteCacheLayout>(255), frames, failure, Bytes));
	CHECK(frames == prior);
	CHECK_FALSE(ReadSpriteCache(Rgba, SpriteCacheLayout::Rgba8TopDown, frames, failure, 64));
	CHECK(frames == prior);
	std::string text = "keep encoded bytes";
	const auto old = text;
	auto bad = Expected();
	bad[1].Rgba.pop_back();
	CHECK_FALSE(WriteSpriteCache(bad, SpriteCacheLayout::Rgba8TopDown, text, failure, Bytes));
	CHECK(text == old);
	CHECK_FALSE(WriteSpriteCache(prior, SpriteCacheLayout::Rgba8TopDown, text, failure, 64));
	CHECK(text == old);
	frames[0].Rgba.reserve(1024 * 1024);
	const auto retained = frames;
	CHECK_FALSE(ReadSpriteCache(Rgba, SpriteCacheLayout::Rgba8TopDown, frames, failure, 256 * 1024));
	CHECK(frames == retained);
}

TEST_CASE("Source sprite cache row orientation is an explicit observation", "[sprite_cache]") {
	const std::vector<SpriteCacheFrame> frames{{1, 2, {10, 20, 30, 40, 100, 110, 120, 130}}};
	std::string text, failure;
	REQUIRE(WriteSpriteCache(frames, SpriteCacheLayout::Bgra8BottomUp, text, failure, Bytes));
	std::vector<SpriteCacheFrame> decoded;
	REQUIRE(ReadSpriteCache(text, SpriteCacheLayout::Rgba8TopDown, decoded, failure, Bytes));
	REQUIRE(decoded.size() == 1);
	CHECK(decoded[0].Rgba == std::vector<uint8_t>{120, 110, 100, 130, 30, 20, 10, 40});
	REQUIRE(ReadSpriteCache(text, SpriteCacheLayout::Bgra8BottomUp, decoded, failure, Bytes));
	CHECK(decoded == frames);
}

TEST_CASE("Single sprite cache uses the scalar source object rather than an array", "[sprite_cache]") {
	const auto literal = nlohmann::json::parse(Rgba)[0].dump();
	std::string failure, text;
	std::vector<SpriteCacheFrame> decoded;
	REQUIRE(ReadSpriteCache(
		literal, SpriteCacheLayout::Rgba8TopDown, decoded, failure, Bytes, SpriteCacheShape::Sprite
	));
	REQUIRE(decoded.size() == 1);
	CHECK(decoded.front() == Expected().front());
	REQUIRE(WriteSpriteCache(
		decoded, SpriteCacheLayout::Rgba8TopDown, text, failure, Bytes, SpriteCacheShape::Sprite
	));
	CHECK(nlohmann::json::parse(text).is_object());
	const auto prior = decoded;
	CHECK_FALSE(ReadSpriteCache(
		Rgba, SpriteCacheLayout::Rgba8TopDown, decoded, failure, Bytes, SpriteCacheShape::Sprite
	));
	CHECK(decoded == prior);
	const auto previous = text;
	CHECK_FALSE(
		WriteSpriteCache({}, SpriteCacheLayout::Rgba8TopDown, text, failure, Bytes, SpriteCacheShape::Sprite)
	);
	CHECK(text == previous);
}

TEST_CASE("Source cache identity binds exact text rather than parsed JSON semantics", "[sprite_cache]") {
	const auto empty = SpriteCacheDataHash("");
	REQUIRE(empty);
	CHECK(
		std::string_view(empty->data(), empty->size()) ==
		"af1349b9f5f9a1a6a0404dea36dcc9499bcb25c9adc112b7cc9a93cae41f3262"
	);
	const auto a = SpriteCacheDataHash("[]"), b = SpriteCacheDataHash("[ ]");
	REQUIRE(a);
	REQUIRE(b);
	CHECK(a != b);
	CHECK(SpriteCacheDataHash("[]") == a);
	std::string large(SpriteCacheLimits::MaximumEncodedBytes + 1, ' ');
	CHECK_FALSE(SpriteCacheDataHash(large));
}

TEST_CASE("Sprite byte layout observations use explicit durable names", "[sprite_cache]") {
	for (const auto layout :
		 {SpriteCacheLayout::Rgba8TopDown,
		  SpriteCacheLayout::Bgra8TopDown,
		  SpriteCacheLayout::Rgba8BottomUp,
		  SpriteCacheLayout::Bgra8BottomUp})
		CHECK(ParseSpriteCacheLayoutName(SpriteCacheLayoutName(layout)) == layout);
	CHECK_FALSE(ParseSpriteCacheLayoutName("rgba"));
	CHECK_FALSE(ParseSpriteCacheLayoutName("0"));
	CHECK(SpriteCacheLayoutName(static_cast<SpriteCacheLayout>(255)).empty());
}
