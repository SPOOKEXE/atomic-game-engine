#include <engine/core/Name.hpp>
#include <engine/core/Paths.hpp>
#include <engine/gui/ReferenceRaster.hpp>
#include <engine/gui/ShapedText.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <filesystem>
#include <fstream>
#include <limits>

TEST_SUITE_ID("engine.gui.reference_raster")

using Catch::Approx;
using engine::core::Color3;
using engine::core::ColorSequence;
using engine::core::Name;
using engine::core::NumberSequence;
using engine::core::Rect;
using engine::core::Vector2;
using namespace engine::gui;

namespace {
	DrawCommand Rectangle(Rect bounds, Color3 tint) {
		DrawCommand command;
		command.Kind = DrawKind::Rectangle;
		command.Bounds = bounds;
		command.Clip = {0.0f, 0.0f, 48.0f, 32.0f};
		command.Tint = tint;
		return command;
	}

	DrawOperation Operation(DrawOperationKind kind, size_t command) {
		DrawOperation operation;
		operation.Kind = kind;
		operation.Command = command;
		return operation;
	}

	ReferenceAsset Checkerboard() {
		ReferenceAsset asset;
		asset.Name = Name("test/checker");
		asset.Image.Width = 2;
		asset.Image.Height = 2;
		asset.Image.Pixels = {
			{255, 0, 0, 255},
			{0, 255, 0, 255},
			{0, 0, 255, 255},
			{255, 255, 255, 255},
		};
		return asset;
	}

	struct StagedAssets {
		std::filesystem::path Previous = engine::core::Paths::Assets();

		StagedAssets() {
			engine::core::Paths::SetAssetsOverride(engine::core::Paths::Base());
		}

		~StagedAssets() {
			engine::core::Paths::SetAssetsOverride(Previous);
		}
	};

	std::vector<std::byte> ReadFont(std::string_view name) {
		std::ifstream file(engine::core::Paths::Fonts() / name, std::ios::binary | std::ios::ate);
		if (!file) {
			return {};
		}
		const std::streamsize size = file.tellg();
		if (size <= 0) {
			return {};
		}
		std::vector<std::byte> bytes(static_cast<size_t>(size));
		file.seekg(0);
		if (!file.read(reinterpret_cast<char *>(bytes.data()), size)) {
			return {};
		}
		return bytes;
	}

	FontPackage Package() {
		FontPackage package;
		const std::vector<std::byte> inter = ReadFont("Inter.ttf");
		if (!inter.empty()) {
			REQUIRE(package.Add(Name("fonts/Inter.ttf"), FontFace::Regular, inter));
		}
		const std::vector<std::byte> noto = ReadFont("NotoSans.ttf");
		if (!noto.empty()) {
			REQUIRE(package.Add(Name("fonts/NotoSans.ttf"), FontFace::Regular, noto));
		}
		return package;
	}

	ShapedGlyph Glyph(float x, Color3 tint) {
		ShapedGlyph glyph;
		glyph.X = x;
		glyph.Y = 10.0f;
		glyph.AdvanceX = 3.0f;
		glyph.PixelSize = 8.0f;
		glyph.Tint = tint;
		return glyph;
	}

	DrawList GoldenList() {
		DrawList list;
		list.CanvasSize = {48.0f, 32.0f};
		const engine::ecs::Entity spatialCollector{1};
		list.Transforms.push_back({spatialCollector, {1.0f, 0.0f}, {1.0f, 1.0f}});

		list.Commands.push_back(Rectangle({0.0f, 0.0f, 48.0f, 32.0f}, {0.02f, 0.03f, 0.05f}));
		DrawCommand gradient = Rectangle({2.0f, 2.0f, 18.0f, 15.0f}, {1.0f, 1.0f, 1.0f});
		gradient.CornerRadius = 3.0f;
		gradient.Gradient = 0;
		list.Commands.push_back(gradient);
		DrawCommand outline = gradient;
		outline.Kind = DrawKind::Outline;
		outline.Thickness = 2.0f;
		outline.Tint = {1.0f, 1.0f, 1.0f};
		outline.Gradient = -1;
		list.Commands.push_back(outline);

		DrawGradient ramp;
		ramp.Color = ColorSequence{Color3{1.0f, 0.0f, 0.0f}, Color3{0.0f, 0.0f, 1.0f}};
		ramp.Transparency = NumberSequence{0.0f};
		ramp.Origin = {2.0f, 2.0f};
		ramp.Axis = {16.0f, 0.0f};
		list.Gradients.push_back(ramp);

		DrawCommand image = Rectangle({21.0f, 2.0f, 32.0f, 10.0f}, {1.0f, 1.0f, 1.0f});
		image.Kind = DrawKind::Image;
		image.Image = Name("test/checker");
		list.Commands.push_back(image);
		DrawCommand slice = image;
		slice.Bounds = {35.0f, 2.0f, 47.0f, 11.0f};
		slice.Scale = ScaleType::Slice;
		slice.SliceCenter = {0.5f, 0.5f, 1.5f, 1.5f};
		list.Commands.push_back(slice);
		DrawCommand tile = image;
		tile.Bounds = {21.0f, 13.0f, 32.0f, 21.0f};
		tile.Clip = {21.0f, 13.0f, 28.0f, 21.0f};
		tile.Scale = ScaleType::Tile;
		tile.Tile = {3.0f, 2.0f};
		list.Commands.push_back(tile);

		DrawCommand masked = Rectangle({3.0f, 18.0f, 18.0f, 30.0f}, {0.0f, 1.0f, 0.0f});
		list.Commands.push_back(masked);
		DrawCommand firstGroup = Rectangle({21.0f, 23.0f, 31.0f, 30.0f}, {1.0f, 0.0f, 0.0f});
		firstGroup.Transparency = 0.25f;
		list.Commands.push_back(firstGroup);
		DrawCommand secondGroup = firstGroup;
		secondGroup.Bounds = {26.0f, 20.0f, 36.0f, 28.0f};
		secondGroup.Tint = {0.0f, 0.0f, 1.0f};
		list.Commands.push_back(secondGroup);
		DrawCommand viewport = Rectangle({38.0f, 15.0f, 47.0f, 30.0f}, {});
		viewport.Kind = DrawKind::Viewport;
		viewport.Collector = spatialCollector;
		list.Commands.push_back(viewport);

		DrawCommand text = Rectangle({2.0f, 16.0f, 18.0f, 30.0f}, {});
		text.Kind = DrawKind::Text;
		text.Shaping.Glyphs = {Glyph(10.0f, {1.0f, 1.0f, 0.0f}), Glyph(6.0f, {0.0f, 1.0f, 1.0f})};
		// The visual order is right-to-left even though the source byte order is
		// the opposite. Rasterising the published glyph positions is the contract.
		ShapedRun rtl;
		rtl.GlyphCount = 2;
		rtl.RightToLeft = true;
		text.Shaping.Runs.push_back(rtl);
		list.Commands.push_back(text);

		DrawOperation beginMask = Operation(DrawOperationKind::BeginMask, 6);
		beginMask.Bounds = {3.0f, 18.0f, 18.0f, 30.0f};
		beginMask.Clip = {0.0f, 0.0f, 48.0f, 32.0f};
		beginMask.CornerRadius = 4.0f;
		DrawOperation endMask = Operation(DrawOperationKind::EndMask, 7);
		DrawOperation beginGroup = Operation(DrawOperationKind::BeginGroup, 7);
		beginGroup.Transparency = 0.5f;
		DrawOperation endGroup = Operation(DrawOperationKind::EndGroup, 9);
		list.Operations = {beginMask, endMask, beginGroup, endGroup};
		return list;
	}
}

TEST_CASE("the reference raster pins a headless visual contract", "[gui][reference_image]") {
	const std::array assets{Checkerboard()};
	const ReferenceImage image = RasterizeReference(GoldenList(), 48, 32, assets);
	REQUIRE(image.Valid());
	CHECK(image.At(1, 1) == ReferencePixel{5, 8, 13, 255});
	CHECK(image.At(5, 8).R > 20);
	CHECK(image.At(5, 8).B < image.At(15, 8).B);
	CHECK(image.At(22, 3) == ReferencePixel{255, 0, 0, 255});
	CHECK(image.At(22, 14) == ReferencePixel{255, 255, 255, 255});
	CHECK(image.At(30, 14).B < 20);
	CHECK(image.At(3, 18).G < 100);
	CHECK(image.At(6, 24).G > 200);
	CHECK(image.At(28, 25).R > 20);
	CHECK(image.At(28, 25).B > 80);
	CHECK(image.At(38, 16).B < 20);
	CHECK(image.At(40, 16).R == 51);
	CHECK(image.At(40, 16).B == 51);
	CHECK(image.At(9, 21).G > 200);

	// This hash is the compact golden image. Every source pixel stays observable
	// through the samples above, while a reviewer gets one stable line to update
	// when accepting an intentional whole-image change.
	CHECK(ReferenceImageHash(image) == UINT64_C(5615975440760828042));
}

TEST_CASE("the reference comparator enforces a strict changed-area budget", "[gui][reference_image]") {
	ReferenceImage before;
	before.Width = 10;
	before.Height = 10;
	before.Pixels.resize(100, {12, 24, 36, 255});
	ReferenceImage after = before;
	after.Pixels[0].R++;
	after.Pixels[1].G += 4;

	const ReferenceComparison tolerant = CompareReferenceImages(before, after, 1, 0.02f);
	CHECK(tolerant.ChangedPixels == 1);
	CHECK(tolerant.ChangedArea == Approx(0.01f));
	CHECK(tolerant.MaximumChannelDifference == 4);
	CHECK(tolerant.WithinChangedAreaCap);

	const ReferenceComparison strict = CompareReferenceImages(before, after, 1, 0.005f);
	CHECK_FALSE(strict.WithinChangedAreaCap);
}

TEST_CASE(
	"the reference raster paints pinned multilingual glyph coverage at published bidi positions",
	"[gui][reference_image][text]"
) {
	const StagedAssets assets;
	const FontPackage package = Package();
	REQUIRE(package.Faces().size() >= 2);
	const ShapedText shaped = ShapeText(
		package,
		TextShapeRequest{
			.Text = "A \xD7\x90\xD7\x91\xD7\x92", .Direction = TextDirection::Automatic, .PixelSize = 20.0f
		}
	);
	REQUIRE(shaped.Status == TextShapeStatus::Ok);
	REQUIRE_FALSE(shaped.Runs.empty());
	CHECK(std::any_of(shaped.Runs.begin(), shaped.Runs.end(), [](const ShapedRun &run) {
		return run.RightToLeft;
	}));
	REQUIRE_FALSE(shaped.Glyphs.empty());

	DrawList list;
	DrawCommand text = Rectangle({4.0f, 2.0f, 92.0f, 30.0f}, {1.0f, 1.0f, 1.0f});
	text.Kind = DrawKind::Text;
	text.TextSize = 20;
	text.XAlignment = TextXAlignment::Left;
	text.YAlignment = TextYAlignment::Top;
	text.Clip = {4.0f, 2.0f, 48.0f, 30.0f};
	text.Shaping = shaped;
	list.Commands.push_back(text);

	const ReferenceImage image = RasterizeReference(list, 96, 32, {}, &package);
	REQUIRE(image.Valid());
	size_t covered = 0;
	for (const ReferencePixel pixel : image.Pixels) {
		covered += pixel.A != 0 ? 1 : 0;
	}
	CHECK(covered > 40);
	CHECK(covered < 500);
	size_t outsideClip = 0;
	for (uint32_t y = 0; y < image.Height; y++) {
		for (uint32_t x = 48; x < image.Width; x++) {
			outsideClip += image.At(x, y).A != 0 ? 1 : 0;
		}
	}
	CHECK(outsideClip == 0);
	CHECK(ReferenceImageHash(image) == UINT64_C(1764399604101926117));
}

TEST_CASE(
	"the reference raster refuses hostile dimensions and nested targets before allocation",
	"[gui][reference_image]"
) {
	DrawList empty;
	CHECK_FALSE(
		RasterizeReference(empty, std::numeric_limits<uint32_t>::max(), std::numeric_limits<uint32_t>::max())
			.Valid()
	);
	CHECK_FALSE(RasterizeReference(empty, 1025, 1024).Valid());

	DrawList groups;
	for (size_t index = 0; index < MAXIMUM_REFERENCE_RASTER_GROUPS + 1; index++) {
		groups.Operations.push_back(Operation(DrawOperationKind::BeginGroup, 0));
	}
	for (size_t index = 0; index < MAXIMUM_REFERENCE_RASTER_GROUPS + 1; index++) {
		groups.Operations.push_back(Operation(DrawOperationKind::EndGroup, 0));
	}
	CHECK_FALSE(RasterizeReference(groups, 1, 1).Valid());

	DrawList byteBudget;
	for (size_t index = 0; index < 4; index++) {
		byteBudget.Operations.push_back(Operation(DrawOperationKind::BeginGroup, 0));
	}
	for (size_t index = 0; index < 4; index++) {
		byteBudget.Operations.push_back(Operation(DrawOperationKind::EndGroup, 0));
	}
	CHECK_FALSE(RasterizeReference(byteBudget, 1024, 1024).Valid());
}

TEST_CASE(
	"the reference raster rejects text that exceeds its fixed coverage budget", "[gui][reference_image][text]"
) {
	const StagedAssets assets;
	const FontPackage package = Package();
	REQUIRE_FALSE(package.Faces().empty());
	const ShapedText shaped = ShapeText(package, TextShapeRequest{.Text = "A", .PixelSize = 16.0f});
	REQUIRE(shaped.Status == TextShapeStatus::Ok);
	REQUIRE(shaped.Glyphs.size() == 1);

	DrawList list;
	DrawCommand text = Rectangle({0.0f, 0.0f, 64.0f, 32.0f}, {1.0f, 1.0f, 1.0f});
	text.Kind = DrawKind::Text;
	text.TextSize = 16;
	text.Shaping.Glyphs.assign(MAXIMUM_REFERENCE_RASTER_GLYPHS / 2 + 1, shaped.Glyphs.front());
	list.Commands.push_back(text);
	list.Commands.push_back(text);
	CHECK_FALSE(RasterizeReference(list, 64, 32, {}, &package).Valid());
}
