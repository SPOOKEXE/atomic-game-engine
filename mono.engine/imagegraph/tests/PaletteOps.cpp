#include "../src/PaletteOps.hpp"

#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <vector>

TEST_SUITE_ID("engine.imagegraph.palette_ops")

using engine::imagegraph::ArrayValue;
using engine::imagegraph::Colour;
using engine::imagegraph::ElementValue;
using engine::imagegraph::Image;
using engine::imagegraph::ValueType;
using engine::imagegraph::detail::MakePalette;
using engine::imagegraph::detail::MAXIMUM_PALETTE_ENTRIES;
using engine::imagegraph::detail::PaletteStatus;
using engine::imagegraph::detail::PaletteValue;
using engine::imagegraph::detail::PosterizeWithPalette;

TEST_CASE("Palette construction requires bounded homogeneous colours", "[imagegraph][palette]") {
	PaletteValue palette;
	const std::array<Colour, 2> source{{{1, 2, 3, 4}, {5, 6, 7, 8}}};
	REQUIRE(MakePalette(source, palette));
	CHECK(palette.Count == 2);
	CHECK(palette.Colors[0] == source[0]);
	CHECK(palette.Colors[1] == source[1]);

	ArrayValue authored{ValueType::Colour, {ElementValue{source[0]}, ElementValue{source[1]}}};
	REQUIRE(MakePalette(authored, palette));
	CHECK(palette.Count == 2);
	CHECK_FALSE(MakePalette(ArrayValue{ValueType::Integer, {}}, palette));
	CHECK_FALSE(MakePalette(ArrayValue{ValueType::Colour, {}}, palette));
	CHECK(palette.Count == 2);

	authored.ElementType = ValueType::Integer;
	CHECK_FALSE(MakePalette(authored, palette));
	authored.ElementType = ValueType::Colour;
	authored.Elements[1] = int64_t{7};
	CHECK_FALSE(MakePalette(authored, palette));

	std::array<Colour, MAXIMUM_PALETTE_ENTRIES + 1> tooMany{};
	CHECK_FALSE(MakePalette(tooMany, palette));
}

TEST_CASE(
	"Palette posterize uses squared RGB distance and preserves first tie and alpha", "[imagegraph][palette]"
) {
	PaletteValue palette;
	const std::array<Colour, 3> colors{{{10, 0, 0, 1}, {14, 0, 0, 2}, {0, 0, 255, 3}}};
	REQUIRE(MakePalette(colors, palette));
	const Image source{2, 1, {12, 0, 0, 71, 13, 0, 0, 93}, 0};
	Image output{2, 1, std::vector<uint8_t>(8, 0), 0};

	CHECK(PosterizeWithPalette(source, output, palette) == PaletteStatus::Ok);
	CHECK(output.Pixels == std::vector<uint8_t>{10, 0, 0, 71, 14, 0, 0, 93});
}

TEST_CASE("Palette alpha mode matches premultiplied RGB and emits palette alpha", "[imagegraph][palette]") {
	PaletteValue palette;
	const std::array<Colour, 2> colors{{{100, 0, 0, 55}, {200, 0, 0, 255}}};
	REQUIRE(MakePalette(colors, palette));
	const Image source{1, 1, {200, 0, 0, 128}, 0};
	Image output{1, 1, std::vector<uint8_t>(4, 0), 0};

	CHECK(PosterizeWithPalette(source, output, palette) == PaletteStatus::Ok);
	CHECK(output.Pixels == std::vector<uint8_t>{200, 0, 0, 128});
	CHECK(PosterizeWithPalette(source, output, palette, true) == PaletteStatus::Ok);
	CHECK(output.Pixels == std::vector<uint8_t>{100, 0, 0, 55});
}

TEST_CASE(
	"Palette posterize rejects invalid bounds and image shapes without partial writes",
	"[imagegraph][palette]"
) {
	PaletteValue empty;
	const Image source{1, 1, {20, 30, 40, 50}, 0};
	Image output{1, 1, {9, 9, 9, 9}, 0};
	CHECK(PosterizeWithPalette(source, output, empty) == PaletteStatus::InvalidControl);
	CHECK(output.Pixels == std::vector<uint8_t>{9, 9, 9, 9});

	PaletteValue tooMany;
	tooMany.Count = MAXIMUM_PALETTE_ENTRIES + 1;
	CHECK(PosterizeWithPalette(source, output, tooMany) == PaletteStatus::InvalidControl);
	CHECK(output.Pixels == std::vector<uint8_t>{9, 9, 9, 9});

	PaletteValue one;
	const std::array<Colour, 1> oneColor{{{1, 2, 3, 255}}};
	REQUIRE(MakePalette(oneColor, one));
	CHECK(PosterizeWithPalette(Image{}, output, one) == PaletteStatus::InvalidImage);
	Image wrongShape{2, 1, std::vector<uint8_t>(8, 9), 0};
	CHECK(PosterizeWithPalette(source, wrongShape, one) == PaletteStatus::InvalidImage);
	CHECK(output.Pixels == std::vector<uint8_t>{9, 9, 9, 9});
}
