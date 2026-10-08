#include <engine/bake/SpriteCache.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <nlohmann/json.hpp>
#include <utility>
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

TEST_CASE("Source surface cache preserves sparse nested surface array ordering", "[sprite_cache]") {
	constexpr std::string_view Nested =
		R"cache([[{"width":1,"height":1,"buffer":"eJzjUbLwAwABWAC1"},0,null],[],false,"skip",{"future":1},{"width":2,"height":1,"buffer":"eJw7waXB8L/B4T8ADnkDuQ=="}])cache";
	std::vector<SurfaceCacheItem> items;
	std::string failure;
	REQUIRE(ReadSurfaceCache(Nested, SpriteCacheLayout::Rgba8TopDown, items, failure, Bytes));
	REQUIRE(items.size() == 6);
	REQUIRE(items[0].IsArray);
	REQUIRE(items[0].Elements.size() == 3);
	CHECK(items[0].Elements[0].Surface == Expected().front());
	CHECK(items[0].Elements[1].Surface.Width == 0);
	CHECK(items[0].Elements[2].Surface.Height == 0);
	CHECK(items[1].IsArray);
	CHECK(items[1].Elements.empty());
	for (size_t index = 2; index < 5; ++index) {
		CHECK_FALSE(items[index].IsArray);
		CHECK(items[index].Surface.Width == 0);
		CHECK(items[index].Elements.empty());
	}
	CHECK(items[5].Surface == Expected()[1]);
}

TEST_CASE("Source surface cache decodes each explicit byte layout", "[sprite_cache]") {
	constexpr std::string_view Vertical =
		R"cache([{"width":1,"height":2,"buffer":"eJzjEpHTSMmraAIABqwCMQ=="}])cache";
	const auto expected = Expected();
	const std::array<std::pair<SpriteCacheLayout, bool>, 4> layouts{
		{{SpriteCacheLayout::Rgba8TopDown, false},
		 {SpriteCacheLayout::Bgra8TopDown, true},
		 {SpriteCacheLayout::Rgba8BottomUp, false},
		 {SpriteCacheLayout::Bgra8BottomUp, true}}
	};
	for (const auto &[layout, bgra] : layouts) {
		std::vector<SurfaceCacheItem> items;
		std::string failure;
		REQUIRE(ReadSurfaceCache(Rgba, layout, items, failure, Bytes));
		REQUIRE(items.size() == expected.size());
		for (size_t index = 0; index < expected.size(); ++index) {
			auto wanted = expected[index];
			if (bgra)
				for (size_t pixel = 0; pixel < wanted.Rgba.size(); pixel += 4)
					std::swap(wanted.Rgba[pixel], wanted.Rgba[pixel + 2]);
			CHECK(items[index].Surface == wanted);
		}
	}
	const std::array<std::pair<SpriteCacheLayout, std::vector<uint8_t>>, 4> verticalLayouts{
		{{SpriteCacheLayout::Rgba8TopDown, {10, 20, 30, 40, 100, 110, 120, 130}},
		 {SpriteCacheLayout::Bgra8TopDown, {30, 20, 10, 40, 120, 110, 100, 130}},
		 {SpriteCacheLayout::Rgba8BottomUp, {100, 110, 120, 130, 10, 20, 30, 40}},
		 {SpriteCacheLayout::Bgra8BottomUp, {120, 110, 100, 130, 30, 20, 10, 40}}}
	};
	for (const auto &[layout, expectedBytes] : verticalLayouts) {
		std::vector<SurfaceCacheItem> items;
		std::string failure;
		REQUIRE(ReadSurfaceCache(Vertical, layout, items, failure, Bytes));
		REQUIRE(items.size() == 1);
		CHECK(items[0].Surface.Width == 1);
		CHECK(items[0].Surface.Height == 2);
		CHECK(items[0].Surface.Rgba == expectedBytes);
	}
}

TEST_CASE("Source surface cache malformed size and zlib refusals retain prior tree", "[sprite_cache]") {
	constexpr std::string_view Nested =
		R"cache([[{"width":1,"height":1,"buffer":"eJzjUbLwAwABWAC1"},null],[]])cache";
	auto items = std::vector<SurfaceCacheItem>{};
	std::string failure;
	REQUIRE(ReadSurfaceCache(Nested, SpriteCacheLayout::Rgba8TopDown, items, failure, Bytes));
	const auto prior = items;
	for (const auto invalid :
		 {std::string_view{"[}"},
		  Short,
		  Long,
		  Trailing,
		  std::string_view{R"cache([{"width":0,"height":1,"buffer":"AA=="}])cache"},
		  std::string_view{R"cache([{"width":1,"height":1,"buffer":"===="}])cache"}}) {
		CHECK_FALSE(ReadSurfaceCache(invalid, SpriteCacheLayout::Rgba8TopDown, items, failure, Bytes));
		CHECK(items == prior);
		CHECK_FALSE(failure.empty());
	}
	CHECK_FALSE(ReadSurfaceCache(Rgba, static_cast<SpriteCacheLayout>(255), items, failure, Bytes));
	CHECK(items == prior);
	CHECK_FALSE(ReadSurfaceCache(Rgba, SpriteCacheLayout::Rgba8TopDown, items, failure, 64));
	CHECK(items == prior);
}

TEST_CASE("Source surface cache item and nesting bounds retain prior tree", "[sprite_cache]") {
	constexpr std::string_view Nested =
		R"cache([[{"width":1,"height":1,"buffer":"eJzjUbLwAwABWAC1"},null],[]])cache";
	auto items = std::vector<SurfaceCacheItem>{};
	std::string failure;
	REQUIRE(ReadSurfaceCache(Nested, SpriteCacheLayout::Rgba8TopDown, items, failure, Bytes));
	const auto prior = items;
	std::string tooMany{"["};
	for (uint32_t index = 0; index <= SurfaceCacheLimits::MaximumItems; ++index) {
		if (index) tooMany += ',';
		tooMany += "null";
	}
	tooMany += ']';
	CHECK_FALSE(ReadSurfaceCache(tooMany, SpriteCacheLayout::Rgba8TopDown, items, failure, Bytes));
	CHECK(items == prior);
	std::string tooDeep(SurfaceCacheLimits::MaximumDepth + 2, '[');
	tooDeep += "null";
	tooDeep.append(SurfaceCacheLimits::MaximumDepth + 2, ']');
	CHECK_FALSE(ReadSurfaceCache(tooDeep, SpriteCacheLayout::Rgba8TopDown, items, failure, Bytes));
	CHECK(items == prior);
}
