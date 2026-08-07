// What an `AssetKind::Texture`'s bytes are.
//
// **The gap was never a missing decoder**, which is worth stating because it
// was diagnosed as one twice. `ImageLabel` drew the missing-texture marker
// because the kind was named and the format was not — so there was nothing for
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
	// **A stride bug is invisible in a square RGBA image** — the same byte count
	// either way — so the case that catches one is a non-square image in the
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
	// An image of no width is not a small image, it is a corrupt header — and
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
