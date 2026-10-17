#include <engine/core/Paths.hpp>
#include <engine/gui/ShapedText.hpp>
#include <engine/render/ShapedGlyphAtlas.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>

TEST_SUITE_ID("engine.render.shapedglyphatlas")

namespace {
	struct StagedAssets {
		std::filesystem::path Previous = engine::core::Paths::Assets();
		StagedAssets() {
			engine::core::Paths::SetAssetsOverride(engine::core::Paths::Base());
		}
		~StagedAssets() {
			engine::core::Paths::SetAssetsOverride(Previous);
		}
	};

	engine::gui::FontPackage Package() {
		std::ifstream file(engine::core::Paths::Fonts() / "Inter.ttf", std::ios::binary | std::ios::ate);
		REQUIRE(file);
		const std::streamsize size = file.tellg();
		std::vector<std::byte> bytes(static_cast<size_t>(size));
		file.seekg(0);
		REQUIRE(file.read(reinterpret_cast<char *>(bytes.data()), size));
		engine::gui::FontPackage package;
		REQUIRE(package.Add(engine::core::Name("fonts/Inter.ttf"), engine::gui::FontFace::Regular, bytes));
		return package;
	}
}

TEST_CASE("shaped glyph atlas consumes shape identities without measuring", "[render][text]") {
	const StagedAssets assets;
	auto package = Package();
	const auto shaped = engine::gui::ShapeText(package, {.Text = "office", .PixelSize = 18.0f});
	REQUIRE(shaped.Status == engine::gui::TextShapeStatus::Ok);
	engine::render::ShapedGlyphAtlas atlas(18.0f);
	const auto first = atlas.Resolve(package, shaped.Glyphs, 1);
	REQUIRE(first.size() == shaped.Glyphs.size());
	CHECK(atlas.PageCount() == 1);
	CHECK_FALSE(atlas.Coverage(0).empty());
	for (const auto &glyph : first)
		CHECK(glyph.Present);
	const auto second = atlas.Resolve(package, shaped.Glyphs, 2);
	CHECK(second == first);
}

TEST_CASE("shaped atlas keeps pages referenced by the current draw list", "[render][text]") {
	const StagedAssets assets;
	auto package = Package();
	const auto shaped = engine::gui::ShapeText(package, {.Text = "W", .PixelSize = 200.0f});
	REQUIRE(shaped.Status == engine::gui::TextShapeStatus::Ok);
	REQUIRE(shaped.Glyphs.size() == 1);
	engine::render::ShapedGlyphAtlas atlas(200.0f);
	const auto first = atlas.Resolve(package, shaped.Glyphs, 7, 200.0f);
	REQUIRE(first.size() == 1);
	REQUIRE(first.front().Present);
	bool capped = false;
	for (int pixelSize = 201; pixelSize < 401; pixelSize++) {
		const auto result = atlas.Resolve(package, shaped.Glyphs, 7, static_cast<float>(pixelSize));
		REQUIRE(result.size() == 1);
		capped |= !result.front().Present;
	}
	CHECK(capped);
	CHECK(atlas.PageCount() == engine::render::ShapedGlyphAtlas::MAXIMUM_PAGES);
	CHECK(atlas.Resolve(package, shaped.Glyphs, 7, 200.0f) == first);
}

TEST_CASE("the active packing page follows an eviction", "[render][text]") {
	const StagedAssets assets;
	auto package = Package();
	const auto wide = engine::gui::ShapeText(package, {.Text = "W", .PixelSize = 400.0f});
	const auto narrow = engine::gui::ShapeText(package, {.Text = "i", .PixelSize = 100.0f});
	REQUIRE(wide.Status == engine::gui::TextShapeStatus::Ok);
	REQUIRE(narrow.Status == engine::gui::TextShapeStatus::Ok);
	REQUIRE(wide.Glyphs.size() == 1);
	REQUIRE(narrow.Glyphs.size() == 1);

	engine::render::ShapedGlyphAtlas atlas(400.0f);
	for (int size = 400; size < 404; size++) {
		const auto resolved = atlas.Resolve(package, wide.Glyphs, 1, static_cast<float>(size));
		REQUIRE(resolved.size() == 1);
		REQUIRE(resolved.front().Present);
	}
	REQUIRE(atlas.PageCount() == engine::render::ShapedGlyphAtlas::MAXIMUM_PAGES);

	const auto replacement = atlas.Resolve(package, wide.Glyphs, 2, 404.0f);
	REQUIRE(replacement.size() == 1);
	REQUIRE(replacement.front().Present);
	REQUIRE(replacement.front().Page == 0);
	const auto packed = atlas.Resolve(package, narrow.Glyphs, 2, 100.0f);
	REQUIRE(packed.size() == 1);
	REQUIRE(packed.front().Present);
	CHECK(packed.front().Page == replacement.front().Page);
}

TEST_CASE("clearing the atlas drops coverage owned by replaced font bytes", "[render][text]") {
	const StagedAssets assets;
	auto package = Package();
	const auto shaped = engine::gui::ShapeText(package, {.Text = "A", .PixelSize = 18.0f});
	REQUIRE(shaped.Status == engine::gui::TextShapeStatus::Ok);
	engine::render::ShapedGlyphAtlas atlas(18.0f);
	REQUIRE(atlas.Resolve(package, shaped.Glyphs, 1).front().Present);
	REQUIRE(atlas.PageCount() == 1);

	atlas.Clear();
	CHECK(atlas.PageCount() == 0);
	CHECK(atlas.Coverage(0).empty());
	REQUIRE(atlas.Resolve(package, shaped.Glyphs, 2).front().Present);
	CHECK(atlas.PageCount() == 1);
}
