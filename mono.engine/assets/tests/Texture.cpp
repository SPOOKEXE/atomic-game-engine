// What an `AssetKind::Texture`'s bytes are.
//
// **The gap was never a missing decoder**, which is worth stating because it
// was diagnosed as one twice. `ImageLabel` drew the missing-texture marker
// because the kind was named and the format was not - so there was nothing for
// a backend to sample even once the bytes arrived. Vendoring a PNG reader would
// have answered how to read somebody else's format, when what was needed was to
// have one.
//
// The refusal cases are most of this file, for `Packet`'s reason: every byte
// here comes off a disk or a wire, and a header field an attacker chooses must
// cost a comparison rather than an allocation.

#include <engine/assets/Texture.hpp>
#include <engine/core/Bytes.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <vector>

TEST_SUITE_ID("engine.assets.texture")

using engine::assets::BytesPerPixel;
using engine::assets::MipExtent;
using engine::assets::MipLevelCount;
using engine::assets::Texture;
using engine::assets::TextureData;
using engine::assets::TextureFormat;
using engine::core::ByteReader;
using engine::core::ByteWriter;

namespace {
	TextureData Made(uint32_t width, uint32_t height, TextureFormat format = TextureFormat::RGBA8) {
		TextureData data;
		data.Width = width;
		data.Height = height;
		data.Format = format;
		data.Pixels.resize(static_cast<size_t>(width) * static_cast<size_t>(height) * BytesPerPixel(format));
		for (size_t index = 0; index < data.Pixels.size(); index++) {
			data.Pixels[index] = static_cast<std::byte>(index * 7u);
		}
		return data;
	}

	// Fills `data` with a full chain of the right shape, without filtering
	// anything - the pixel values are irrelevant to the format's job.
	void GiveChain(TextureData &data) {
		data.Mips.clear();
		for (uint32_t level = 1; level < MipLevelCount(data.Width, data.Height); level++) {
			data.Mips.emplace_back(
				static_cast<size_t>(MipExtent(data.Width, level)) * MipExtent(data.Height, level) *
					BytesPerPixel(data.Format),
				static_cast<std::byte>(level)
			);
		}
	}
}

TEST_CASE("a texture round-trips", "[assets][texture]") {
	const TextureData source = Made(4, 3);
	REQUIRE(source.IsValid());

	ByteWriter writer;
	REQUIRE(Texture::Write(writer, source));

	TextureData read;
	ByteReader reader(writer.Bytes());
	REQUIRE(Texture::Read(reader, read));

	CHECK(read.Width == 4);
	CHECK(read.Height == 3);
	CHECK(read.Format == TextureFormat::RGBA8);
	CHECK(read.Pixels == source.Pixels);

	// Every byte consumed. A reader and a writer that disagree about the layout
	// otherwise surface as a bug much later.
	CHECK(reader.AtEnd());
}

TEST_CASE("the single-channel format round-trips at its own stride", "[assets][texture]") {
	// **A stride bug is invisible in a square RGBA image** - the same byte count
	// either way - so the case that catches one is a non-square image in the
	// other format.
	const TextureData source = Made(5, 2, TextureFormat::R8);
	CHECK(source.Pixels.size() == 10);

	ByteWriter writer;
	REQUIRE(Texture::Write(writer, source));

	TextureData read;
	ByteReader reader(writer.Bytes());
	REQUIRE(Texture::Read(reader, read));
	CHECK(read.Format == TextureFormat::R8);
	CHECK(read.Pixels == source.Pixels);
}

TEST_CASE("an image whose pixels disagree with its dimensions is invalid", "[assets][texture]") {
	TextureData data = Made(4, 4);
	data.Pixels.pop_back();

	CHECK_FALSE(data.IsValid());

	ByteWriter writer;
	CHECK_FALSE(Texture::Write(writer, data));
	CHECK(writer.Empty());
}

TEST_CASE("a header claiming more than the file holds is refused", "[assets][texture]") {
	// **The decompression bomb, and the whole reason the size is checked against
	// what is present before a byte is allocated.** A header claiming a gigabyte
	// over a forty-byte file must cost a comparison.
	ByteWriter writer;
	writer.WriteUInt32(Texture::MAGIC);
	writer.WriteUInt16(Texture::VERSION);
	writer.WriteUInt8(static_cast<uint8_t>(TextureFormat::RGBA8));
	writer.WriteUInt32(4096);
	writer.WriteUInt32(4096);
	// And no pixels at all.

	TextureData read;
	ByteReader reader(writer.Bytes());
	CHECK_FALSE(Texture::Read(reader, read));

	// Untouched, so a caller reusing one across a load loop cannot act on a
	// mixture of the last good image and a bad one.
	CHECK(read.Width == 0);
	CHECK(read.Pixels.empty());
}

TEST_CASE("a dimension past the bound is refused", "[assets][texture]") {
	ByteWriter writer;
	writer.WriteUInt32(Texture::MAGIC);
	writer.WriteUInt16(Texture::VERSION);
	writer.WriteUInt8(static_cast<uint8_t>(TextureFormat::RGBA8));
	writer.WriteUInt32(Texture::MAXIMUM_DIMENSION + 1);
	writer.WriteUInt32(1);

	TextureData read;
	ByteReader reader(writer.Bytes());
	CHECK_FALSE(Texture::Read(reader, read));
}

TEST_CASE("a zero dimension is refused rather than read as empty", "[assets][texture]") {
	// An image of no width is not a small image, it is a corrupt header - and
	// accepting one would put a zero-sized texture into a backend that has to
	// special-case it forever after.
	ByteWriter writer;
	writer.WriteUInt32(Texture::MAGIC);
	writer.WriteUInt16(Texture::VERSION);
	writer.WriteUInt8(static_cast<uint8_t>(TextureFormat::RGBA8));
	writer.WriteUInt32(0);
	writer.WriteUInt32(16);

	TextureData read;
	ByteReader reader(writer.Bytes());
	CHECK_FALSE(Texture::Read(reader, read));
}

TEST_CASE("a format outside the enum is refused before the cast", "[assets][texture]") {
	// **Range-checked before the cast**, for `ReadMessage`'s reason: casting an
	// out-of-range byte produces a value no switch handles, and every consumer
	// downstream then reads something the type says cannot exist.
	ByteWriter writer;
	writer.WriteUInt32(Texture::MAGIC);
	writer.WriteUInt16(Texture::VERSION);
	writer.WriteUInt8(200);
	writer.WriteUInt32(2);
	writer.WriteUInt32(2);

	TextureData read;
	ByteReader reader(writer.Bytes());
	CHECK_FALSE(Texture::Read(reader, read));
}

TEST_CASE("a wrong magic and a wrong version are both refused", "[assets][texture]") {
	// A file that is not this format has to fail as "not this format" rather
	// than as a plausible width of nine hundred million.
	const TextureData source = Made(2, 2);

	{
		ByteWriter writer;
		REQUIRE(Texture::Write(writer, source));
		std::vector<std::byte> bytes(writer.Bytes().begin(), writer.Bytes().end());
		bytes[0] = static_cast<std::byte>(0x00);

		TextureData read;
		ByteReader reader(bytes);
		CHECK_FALSE(Texture::Read(reader, read));
	}

	{
		ByteWriter writer;
		writer.WriteUInt32(Texture::MAGIC);
		writer.WriteUInt16(Texture::VERSION + 1);
		writer.WriteUInt8(0);
		writer.WriteUInt32(2);
		writer.WriteUInt32(2);

		TextureData read;
		ByteReader reader(writer.Bytes());
		CHECK_FALSE(Texture::Read(reader, read));
	}
}

TEST_CASE("a truncated file is refused rather than half read", "[assets][texture]") {
	const TextureData source = Made(8, 8);

	ByteWriter writer;
	REQUIRE(Texture::Write(writer, source));

	const std::span<const std::byte> whole = writer.Bytes();
	const std::vector<std::byte> cut(whole.begin(), whole.begin() + whole.size() / 2);

	TextureData read;
	ByteReader reader(cut);
	CHECK_FALSE(Texture::Read(reader, read));
	CHECK(read.Pixels.empty());
}

TEST_CASE("a late texture truncation leaves the destination alone", "[assets][texture]") {
	TextureData held = Made(2, 2, TextureFormat::R8);
	held.Pixels[0] = std::byte{0x5A};

	ByteWriter writer;
	REQUIRE(Texture::Write(writer, Made(8, 8)));
	const std::span<const std::byte> complete = writer.Bytes();
	const std::span<const std::byte> truncated(complete.data(), complete.size() - 1);
	ByteReader reader(truncated);
	CHECK_FALSE(Texture::Read(reader, held));

	CHECK(held.Width == 2);
	CHECK(held.Height == 2);
	CHECK(held.Format == TextureFormat::R8);
	CHECK(held.Pixels[0] == std::byte{0x5A});
}

// --- flipbook layout, added at v0.10 ------------------------------------------

TEST_CASE("a flipbook's grid, frame count and rate round-trip", "[assets][texture]") {
	// Without these three a 4x4 animation sheet and a 4x4 tile atlas are the
	// same file, and everything downstream has to be told the numbers by hand.
	TextureData source = Made(8, 8);
	source.FlipbookSide = 4;
	source.FlipbookFrames = 13;
	source.FlipbookFrameRate = 24.0f;
	REQUIRE(source.IsFlipbook());

	ByteWriter writer;
	REQUIRE(Texture::Write(writer, source));

	TextureData read;
	ByteReader reader(writer.Bytes());
	REQUIRE(Texture::Read(reader, read));

	CHECK(read.FlipbookSide == 4);
	CHECK(read.FlipbookFrames == 13);
	CHECK(read.FlipbookFrameRate == 24.0f);
	CHECK(read.Pixels == source.Pixels);

	// **The three fields are before the pixels**, and this is what pins it: the
	// pixels have no length of their own, so anything written after them would
	// be unreachable.
	CHECK(reader.AtEnd());
}

TEST_CASE("a still image writes zeroes and reads back a still", "[assets][texture]") {
	const TextureData source = Made(4, 4);
	CHECK_FALSE(source.IsFlipbook());

	ByteWriter writer;
	REQUIRE(Texture::Write(writer, source));

	TextureData read;
	ByteReader reader(writer.Bytes());
	REQUIRE(Texture::Read(reader, read));

	CHECK(read.FlipbookSide == 0);
	CHECK(read.FlipbookFrames == 0);
	CHECK_FALSE(read.IsFlipbook());
}

TEST_CASE("a version 1 file still reads, as a still image", "[assets][texture]") {
	// **Not compatibility for its own sake** - this is pre-release and a format
	// break is acceptable. The absent case has an obviously right answer: a v1
	// file is a still image, which is what zeroes already mean, so refusing one
	// would make every baked texture on disk unreadable to buy nothing.
	ByteWriter writer;
	writer.WriteUInt32(Texture::MAGIC);
	writer.WriteUInt16(1);
	writer.WriteUInt8(static_cast<uint8_t>(TextureFormat::RGBA8));
	writer.WriteUInt32(2);
	writer.WriteUInt32(1);
	for (size_t index = 0; index < 2 * 1 * 4; index++) {
		writer.WriteUInt8(static_cast<uint8_t>(index));
	}

	TextureData read;
	ByteReader reader(writer.Bytes());
	REQUIRE(Texture::Read(reader, read));

	CHECK(read.Width == 2);
	CHECK(read.Height == 1);
	CHECK(read.FlipbookSide == 0);
	CHECK(read.FlipbookFrameRate == 0.0f);
	CHECK(reader.AtEnd());
}

TEST_CASE("a frame count past the grid is clamped rather than refusing the file", "[assets][texture]") {
	// **The opposite of how every other field here is treated, and right for the
	// same reason the rest are not.** The others decide how many bytes to
	// allocate; this one decides which cell to sample. A wrong count is a
	// shorter animation, a wrong dimension is a buffer overrun.
	ByteWriter writer;
	writer.WriteUInt32(Texture::MAGIC);
	writer.WriteUInt16(Texture::VERSION);
	writer.WriteUInt8(static_cast<uint8_t>(TextureFormat::RGBA8));
	writer.WriteUInt32(2);
	writer.WriteUInt32(2);
	writer.WriteUInt8(2);	// A 2x2 grid: four cells.
	writer.WriteUInt8(200); // Claiming two hundred frames.
	writer.WriteFloat(24.0f);
	writer.WriteUInt8(1); // One level: no chain.
	for (size_t index = 0; index < 2 * 2 * 4; index++) {
		writer.WriteUInt8(0);
	}

	TextureData read;
	ByteReader reader(writer.Bytes());
	REQUIRE(Texture::Read(reader, read));
	CHECK(read.FlipbookFrames == 4);

	// A rate that is not a rate reads as "unknown" rather than being refused,
	// because every consumer already has to handle zero.
	{
		ByteWriter bad;
		bad.WriteUInt32(Texture::MAGIC);
		bad.WriteUInt16(Texture::VERSION);
		bad.WriteUInt8(static_cast<uint8_t>(TextureFormat::RGBA8));
		bad.WriteUInt32(1);
		bad.WriteUInt32(1);
		bad.WriteUInt8(1);
		bad.WriteUInt8(1);
		bad.WriteFloat(-5.0f);
		bad.WriteUInt8(1);
		for (size_t index = 0; index < 4; index++) {
			bad.WriteUInt8(0);
		}

		TextureData negative;
		ByteReader again(bad.Bytes());
		REQUIRE(Texture::Read(again, negative));
		CHECK(negative.FlipbookFrameRate == 0.0f);
	}
}

// --- the mip chain, added at v0.14 --------------------------------------------

TEST_CASE("a level's extent halves and stops at one", "[assets][texture]") {
	// The rule every graphics API uses, and the reason a level's dimensions are
	// derived rather than stored: a stored pair is the one field of a level a
	// corrupt file gets to choose.
	CHECK(MipExtent(8, 0) == 8);
	CHECK(MipExtent(8, 3) == 1);
	CHECK(MipExtent(8, 9) == 1);
	CHECK(MipExtent(5, 1) == 2);

	// A non-square image runs until *both* axes are one, which is what makes a
	// 64x1 texture keep six levels rather than one.
	CHECK(MipLevelCount(1, 1) == 1);
	CHECK(MipLevelCount(8, 8) == 4);
	CHECK(MipLevelCount(64, 1) == 7);
	CHECK(MipLevelCount(Texture::MAXIMUM_DIMENSION, Texture::MAXIMUM_DIMENSION) == 15);
}

TEST_CASE("a mip chain round-trips", "[assets][texture]") {
	TextureData source = Made(4, 2);
	GiveChain(source);
	REQUIRE(source.LevelCount() == 3);
	REQUIRE(source.IsValid());

	ByteWriter writer;
	REQUIRE(Texture::Write(writer, source));

	TextureData read;
	ByteReader reader(writer.Bytes());
	REQUIRE(Texture::Read(reader, read));

	CHECK(read.LevelCount() == 3);
	CHECK(read.Pixels == source.Pixels);
	CHECK(read.Mips == source.Mips);

	// **The levels are the tail of the payload and have no length of their
	// own**, so anything written after them would be unreachable - the same
	// property the flipbook triple is placed before the pixels to preserve.
	CHECK(reader.AtEnd());
}

TEST_CASE("a level whose size disagrees with its extent is invalid", "[assets][texture]") {
	// The check that makes a chain either right or refused. A level one byte
	// short is a texture that samples off the end of its own buffer on upload,
	// and nothing downstream can tell.
	TextureData data = Made(4, 4);
	GiveChain(data);
	REQUIRE(data.IsValid());

	data.Mips[0].pop_back();
	CHECK_FALSE(data.IsValid());

	ByteWriter writer;
	CHECK_FALSE(Texture::Write(writer, data));
	CHECK(writer.Empty());
}

TEST_CASE("more levels than the dimensions allow is refused", "[assets][texture]") {
	// **Bounded by what the dimensions imply rather than by a constant**, so a
	// count is refused at the point it stops describing an image rather than at
	// some larger number that happens to be safe to allocate.
	ByteWriter writer;
	writer.WriteUInt32(Texture::MAGIC);
	writer.WriteUInt16(Texture::VERSION);
	writer.WriteUInt8(static_cast<uint8_t>(TextureFormat::RGBA8));
	writer.WriteUInt32(4);
	writer.WriteUInt32(4);
	writer.WriteUInt8(0);
	writer.WriteUInt8(0);
	writer.WriteFloat(0.0f);
	writer.WriteUInt8(9); // A 4x4 image has three levels.
	for (size_t index = 0; index < 4 * 4 * 4; index++) {
		writer.WriteUInt8(0);
	}

	TextureData read;
	ByteReader reader(writer.Bytes());
	CHECK_FALSE(Texture::Read(reader, read));
	CHECK(read.Pixels.empty());
}

TEST_CASE("a chain claiming more bytes than the file holds is refused", "[assets][texture]") {
	// The decompression-bomb check again, now over the whole chain: the levels
	// are summed and compared against what is present before a byte is
	// allocated, so a header claiming three levels over a one-level file costs a
	// comparison.
	ByteWriter writer;
	writer.WriteUInt32(Texture::MAGIC);
	writer.WriteUInt16(Texture::VERSION);
	writer.WriteUInt8(static_cast<uint8_t>(TextureFormat::RGBA8));
	writer.WriteUInt32(4);
	writer.WriteUInt32(4);
	writer.WriteUInt8(0);
	writer.WriteUInt8(0);
	writer.WriteFloat(0.0f);
	writer.WriteUInt8(3);
	for (size_t index = 0; index < 4 * 4 * 4; index++) {
		writer.WriteUInt8(0);
	}

	TextureData read;
	ByteReader reader(writer.Bytes());
	CHECK_FALSE(Texture::Read(reader, read));
	CHECK(read.Pixels.empty());
}

TEST_CASE("a version 2 file reads as a single level", "[assets][texture]") {
	// The v1 argument, one version on: the absent case has an obviously right
	// answer, and a file with no chain is a texture with one level - which is
	// what every texture baked before v0.14 is.
	ByteWriter writer;
	writer.WriteUInt32(Texture::MAGIC);
	writer.WriteUInt16(2);
	writer.WriteUInt8(static_cast<uint8_t>(TextureFormat::RGBA8));
	writer.WriteUInt32(2);
	writer.WriteUInt32(2);
	writer.WriteUInt8(2);
	writer.WriteUInt8(3);
	writer.WriteFloat(12.0f);
	for (size_t index = 0; index < 2 * 2 * 4; index++) {
		writer.WriteUInt8(static_cast<uint8_t>(index));
	}

	TextureData read;
	ByteReader reader(writer.Bytes());
	REQUIRE(Texture::Read(reader, read));

	CHECK(read.LevelCount() == 1);
	CHECK(read.Mips.empty());
	CHECK(read.FlipbookSide == 2);
	CHECK(read.FlipbookFrames == 3);
	CHECK(reader.AtEnd());
}
