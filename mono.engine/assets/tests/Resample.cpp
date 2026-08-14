// The box filter and the mip chain built out of it.
//
// **These cases were `bake`'s until v0.15 and the arithmetic did not change when
// they moved** - only the tier it lives at, so that `assets` may build a chain
// over pixels it generated itself. What did change is the fixtures: `bake`'s
// suite fed the filter a decoded BMP, and a suite one tier down has no decoder
// to reach for, so the images here are written out by hand. That is a fair trade
// for this file - every property below is about what the filter does to bytes,
// not about where the bytes came from.

#include <engine/assets/Resample.hpp>
#include <engine/assets/Texture.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

TEST_SUITE_ID("engine.assets.resample")

using engine::assets::BuildMipChain;
using engine::assets::MipChainLevels;
using engine::assets::MipLevelCount;
using engine::assets::ResizeImage;
using engine::assets::TextureData;
using engine::assets::TextureFormat;

namespace {

	struct Pixel {
		int R = 0;
		int G = 0;
		int B = 0;
		int A = 0;

		bool operator==(const Pixel &) const = default;
	};

	Pixel At(const TextureData &image, uint32_t x, uint32_t y) {
		const size_t offset = (static_cast<size_t>(y) * image.Width + x) * 4;
		return Pixel{
			static_cast<int>(image.Pixels[offset]),
			static_cast<int>(image.Pixels[offset + 1]),
			static_cast<int>(image.Pixels[offset + 2]),
			static_cast<int>(image.Pixels[offset + 3]),
		};
	}

	// Two by two: red and green over blue and yellow.
	//
	// **Four saturated colours no two of which share a channel value**, so an
	// average is a number none of the four holds and a nearest sample is exactly
	// one of them. That is what tells the two apart in one assertion.
	TextureData Quad() {
		constexpr std::array<Pixel, 4> COLOURS{
			Pixel{255, 0, 0, 255},
			Pixel{0, 255, 0, 255},
			Pixel{0, 0, 255, 255},
			Pixel{255, 255, 0, 255},
		};

		TextureData image;
		image.Width = 2;
		image.Height = 2;
		image.Format = TextureFormat::RGBA8;
		image.Pixels.resize(4 * 4);

		for (size_t index = 0; index < COLOURS.size(); index++) {
			const Pixel &colour = COLOURS[index];
			image.Pixels[index * 4] = static_cast<std::byte>(colour.R);
			image.Pixels[index * 4 + 1] = static_cast<std::byte>(colour.G);
			image.Pixels[index * 4 + 2] = static_cast<std::byte>(colour.B);
			image.Pixels[index * 4 + 3] = static_cast<std::byte>(colour.A);
		}
		return image;
	}

	// A sheet of `side * side` cells, each `cell` pixels square, where every cell
	// is one flat colour and the colours differ.
	//
	// **Flat cells with hard borders between them**, which is what makes bleeding
	// visible: a level built across a cell boundary averages two colours and lands
	// somewhere neither cell holds.
	TextureData Sheet(uint8_t side, uint32_t cell, uint8_t frames) {
		TextureData sheet;
		sheet.Width = side * cell;
		sheet.Height = side * cell;
		sheet.Format = TextureFormat::RGBA8;
		sheet.FlipbookSide = side;
		sheet.FlipbookFrames = frames;
		sheet.FlipbookFrameRate = 12.0f;
		sheet.Pixels.resize(static_cast<size_t>(sheet.Width) * sheet.Height * 4);

		for (uint32_t row = 0; row < sheet.Height; row++) {
			for (uint32_t column = 0; column < sheet.Width; column++) {
				const uint32_t cellIndex = (row / cell) * side + (column / cell);
				const size_t offset = (static_cast<size_t>(row) * sheet.Width + column) * 4;
				sheet.Pixels[offset] = static_cast<std::byte>(cellIndex * 40 + 10);
				sheet.Pixels[offset + 1] = std::byte{0};
				sheet.Pixels[offset + 2] = std::byte{0};
				sheet.Pixels[offset + 3] = std::byte{255};
			}
		}
		return sheet;
	}
}

TEST_CASE("a box filter averages rather than samples", "[assets][resample]") {
	const TextureData source = Quad();

	TextureData half;
	REQUIRE(ResizeImage(source, 1, 1, half));
	CHECK(half.Width == 1);
	CHECK(half.Height == 1);

	// The mean of red, green, blue and yellow. A nearest sample would give one
	// of the four corners, which is what aliases into speckle at distance.
	CHECK(
		At(half, 0, 0) == Pixel{(255 + 0 + 0 + 255) / 4, (0 + 255 + 0 + 255) / 4, (0 + 0 + 255 + 0) / 4, 255}
	);
}

TEST_CASE("resizing to nothing is refused", "[assets][resample]") {
	const TextureData source = Quad();
	TextureData out;

	CHECK_FALSE(ResizeImage(source, 0, 1, out));
	CHECK_FALSE(ResizeImage(source, 1, 0, out));
	CHECK_FALSE(ResizeImage(source, 100000, 1, out));
	CHECK_FALSE(ResizeImage(TextureData{}, 1, 1, out));
}

TEST_CASE("an upscale duplicates rather than inventing detail", "[assets][resample]") {
	const TextureData source = Quad();

	TextureData bigger;
	REQUIRE(ResizeImage(source, 4, 4, bigger));

	CHECK(At(bigger, 0, 0) == Pixel{255, 0, 0, 255});
	CHECK(At(bigger, 1, 0) == Pixel{255, 0, 0, 255});
	CHECK(At(bigger, 3, 3) == Pixel{255, 255, 0, 255});
}

TEST_CASE("a resize keeps the flipbook it was given", "[assets][resample]") {
	// **The failure this pins would have looked like a broken decoder.** A GIF
	// larger than `--max-texture` goes through a resize on its way to disk, and
	// a resize that dropped these three fields would turn every big imported
	// animation back into an anonymous atlas - with the decoder working
	// perfectly the whole time.
	const TextureData source = Sheet(2, 4, 3);

	TextureData smaller;
	REQUIRE(ResizeImage(source, 2, 2, smaller));

	CHECK(smaller.Width == 2);
	CHECK(smaller.FlipbookSide == source.FlipbookSide);
	CHECK(smaller.FlipbookFrames == 3);
	CHECK(smaller.FlipbookFrameRate == source.FlipbookFrameRate);
}

TEST_CASE("a chain halves all the way down and every level is the right size", "[assets][resample]") {
	const TextureData source = Quad();

	TextureData chained = source;
	REQUIRE(BuildMipChain(chained));

	// 2x2 gives two levels, and the smallest is the mean of the four corners -
	// the same number `ResizeImage` produces directly, because the chain is that
	// filter run repeatedly rather than a second one.
	REQUIRE(chained.LevelCount() == 2);
	REQUIRE(chained.IsValid());
	CHECK(chained.Pixels == source.Pixels);

	TextureData direct;
	REQUIRE(ResizeImage(source, 1, 1, direct));
	CHECK(chained.Mips[0] == direct.Pixels);
}

TEST_CASE("a chain is built from the level above, not from the base", "[assets][resample]") {
	// **Successive halving, which is what a sampler interpolating between two
	// levels expects.** Filtering every level from the base instead would make
	// each one a different reconstruction of the image, and the seam between two
	// of them is what a trilinear fetch shows.
	// **An odd width, which is the only shape that tells the two apart.** Over
	// power-of-two dimensions both orders average the same source pixels and
	// agree to the byte; five columns halve to two unequal groups, and the level
	// below then weights them equally where the base would not.
	TextureData wide;
	wide.Width = 5;
	wide.Height = 1;
	wide.Format = TextureFormat::R8;
	wide.Pixels = {std::byte{0}, std::byte{0}, std::byte{0}, std::byte{0}, std::byte{100}};
	REQUIRE(BuildMipChain(wide));

	REQUIRE(wide.LevelCount() == 3);
	REQUIRE(wide.Mips[0].size() == 2);
	CHECK(static_cast<int>(wide.Mips[0][0]) == 0);
	CHECK(static_cast<int>(wide.Mips[0][1]) == 33);

	// 16 is (0 + 33) / 2. Filtering the base directly would give 20.
	REQUIRE(wide.Mips[1].size() == 1);
	CHECK(static_cast<int>(wide.Mips[1][0]) == 16);
}

TEST_CASE("a flipbook's chain stops before frames bleed into each other", "[assets][resample]") {
	// **The decision this case exists for.** Halving a grid of frames is only
	// safe while every destination pixel sits inside one cell, which holds
	// exactly while the cell size is still an even halving. One level further and
	// a pixel averages two frames - a ghost of the next frame, at distance, that
	// looks like the flipbook's cell arithmetic is wrong rather than like the
	// chain being one level too long.
	TextureData sheet = Sheet(2, 4, 4);
	REQUIRE(MipChainLevels(sheet) == 3);
	REQUIRE(BuildMipChain(sheet));
	REQUIRE(sheet.LevelCount() == 3);

	// The last level is one pixel a frame, and each is still its own colour
	// rather than a blend of its neighbours.
	const std::vector<std::byte> &last = sheet.Mips.back();
	REQUIRE(last.size() == 2 * 2 * 4);
	CHECK(static_cast<int>(last[0]) == 10);
	CHECK(static_cast<int>(last[4]) == 50);
	CHECK(static_cast<int>(last[8]) == 90);
	CHECK(static_cast<int>(last[12]) == 130);

	// A still image of the same size runs two levels further, to a single pixel.
	TextureData still = sheet;
	still.FlipbookSide = 0;
	still.FlipbookFrames = 0;
	still.Mips.clear();
	CHECK(MipChainLevels(still) == 4);

	// **And so does a one-cell sheet**, which is what every still GIF decodes to
	// - there is no neighbouring frame for it to bleed into. `bake::ReadGif`
	// gives a single frame a 1x1 grid, so without this case every imported still
	// would lose its levels to a neighbour that does not exist.
	TextureData single = Sheet(1, 2, 1);
	REQUIRE(single.FlipbookSide == 1);
	CHECK(MipChainLevels(single) == MipLevelCount(2, 2));
}

TEST_CASE("a sheet whose cells cannot be halved gets no chain at all", "[assets][resample]") {
	// **Refusing the chain rather than padding the cells.** A gutter would be a
	// change to what a flipbook *is* - every sampler divides the sheet by
	// `FlipbookSide` - so the honest answer for a sheet of odd cells is the
	// texture it already was.
	TextureData odd = Sheet(2, 3, 4);
	CHECK(MipChainLevels(odd) == 1);
	REQUIRE(BuildMipChain(odd));
	CHECK(odd.LevelCount() == 1);
	CHECK(odd.Mips.empty());

	// A grid the dimensions do not divide evenly is the same event: there is no
	// smaller sheet that still holds the same cells.
	TextureData ragged = Sheet(2, 4, 4);
	ragged.FlipbookSide = 3;
	CHECK(MipChainLevels(ragged) == 1);
}

TEST_CASE("a resize drops the chain it cannot carry", "[assets][resample]") {
	// A chain describes one image at one size. Carrying the old levels past a
	// resize would leave level 1 larger than level 0, which `IsValid` refuses and
	// a GPU would read off the end of.
	TextureData source = Quad();
	REQUIRE(BuildMipChain(source));
	REQUIRE(source.LevelCount() == 2);

	TextureData bigger;
	REQUIRE(ResizeImage(source, 4, 4, bigger));
	CHECK(bigger.Mips.empty());
	CHECK(bigger.IsValid());
}

TEST_CASE("building a chain twice gives the same chain", "[assets][resample]") {
	// **Rebuilt rather than appended to**, because a graph may be run twice and
	// a pipeline that added levels each time would produce a different file every
	// bake.
	TextureData once = Quad();
	REQUIRE(BuildMipChain(once));

	TextureData twice = once;
	REQUIRE(BuildMipChain(twice));
	CHECK(twice.Mips == once.Mips);
}
