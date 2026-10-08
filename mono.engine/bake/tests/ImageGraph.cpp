#include <engine/bake/ImageGraph.hpp>
#include <engine/core/Bytes.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <string>
#include <vector>

TEST_SUITE_ID("engine.bake.imagegraph")
TEST_DEPENDS("engine.imagegraph.document")
TEST_DEPENDS("engine.assets.texture")

namespace {
	constexpr std::array<uint8_t, 77> PNG_RGB{
		{0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A, 0x00, 0x00, 0x00, 0x0D, 0x49, 0x48, 0x44, 0x52,
		 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x02, 0x08, 0x02, 0x00, 0x00, 0x00, 0xFD, 0xD4, 0x9A,
		 0x73, 0x00, 0x00, 0x00, 0x14, 0x49, 0x44, 0x41, 0x54, 0x78, 0xDA, 0x63, 0xF8, 0xCF, 0xC0, 0xC0,
		 0x00, 0xC2, 0x0C, 0xFF, 0xFF, 0xFF, 0x67, 0x00, 0x00, 0x1E, 0xEF, 0x04, 0xFC, 0x73, 0x1C, 0x53,
		 0xCC, 0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4E, 0x44, 0xAE, 0x42, 0x60, 0x82}
	};
	constexpr std::array<uint8_t, 340> JPEG_GREY{
		{0xFF, 0xD8, 0xFF, 0xE0, 0x00, 0x10, 0x4A, 0x46, 0x49, 0x46, 0x00, 0x01, 0x01, 0x00, 0x00, 0x01, 0x00,
		 0x01, 0x00, 0x00, 0xFF, 0xDB, 0x00, 0x43, 0x00, 0x02, 0x01, 0x01, 0x01, 0x01, 0x01, 0x02, 0x01, 0x01,
		 0x01, 0x02, 0x02, 0x02, 0x02, 0x02, 0x04, 0x03, 0x02, 0x02, 0x02, 0x02, 0x05, 0x04, 0x04, 0x03, 0x04,
		 0x06, 0x05, 0x06, 0x06, 0x06, 0x05, 0x06, 0x06, 0x06, 0x07, 0x09, 0x08, 0x06, 0x07, 0x09, 0x07, 0x06,
		 0x06, 0x08, 0x0B, 0x08, 0x09, 0x0A, 0x0A, 0x0A, 0x0A, 0x0A, 0x06, 0x08, 0x0B, 0x0C, 0x0B, 0x0A, 0x0C,
		 0x09, 0x0A, 0x0A, 0x0A, 0xFF, 0xC0, 0x00, 0x0B, 0x08, 0x00, 0x10, 0x00, 0x10, 0x01, 0x01, 0x11, 0x00,
		 0xFF, 0xC4, 0x00, 0x1F, 0x00, 0x00, 0x01, 0x05, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x00, 0x00, 0x00,
		 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B, 0xFF,
		 0xC4, 0x00, 0xB5, 0x10, 0x00, 0x02, 0x01, 0x03, 0x03, 0x02, 0x04, 0x03, 0x05, 0x05, 0x04, 0x04, 0x00,
		 0x00, 0x01, 0x7D, 0x01, 0x02, 0x03, 0x00, 0x04, 0x11, 0x05, 0x12, 0x21, 0x31, 0x41, 0x06, 0x13, 0x51,
		 0x61, 0x07, 0x22, 0x71, 0x14, 0x32, 0x81, 0x91, 0xA1, 0x08, 0x23, 0x42, 0xB1, 0xC1, 0x15, 0x52, 0xD1,
		 0xF0, 0x24, 0x33, 0x62, 0x72, 0x82, 0x09, 0x0A, 0x16, 0x17, 0x18, 0x19, 0x1A, 0x25, 0x26, 0x27, 0x28,
		 0x29, 0x2A, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3A, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49, 0x4A,
		 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59, 0x5A, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69, 0x6A, 0x73,
		 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7A, 0x83, 0x84, 0x85, 0x86, 0x87, 0x88, 0x89, 0x8A, 0x92, 0x93,
		 0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9A, 0xA2, 0xA3, 0xA4, 0xA5, 0xA6, 0xA7, 0xA8, 0xA9, 0xAA, 0xB2,
		 0xB3, 0xB4, 0xB5, 0xB6, 0xB7, 0xB8, 0xB9, 0xBA, 0xC2, 0xC3, 0xC4, 0xC5, 0xC6, 0xC7, 0xC8, 0xC9, 0xCA,
		 0xD2, 0xD3, 0xD4, 0xD5, 0xD6, 0xD7, 0xD8, 0xD9, 0xDA, 0xE1, 0xE2, 0xE3, 0xE4, 0xE5, 0xE6, 0xE7, 0xE8,
		 0xE9, 0xEA, 0xF1, 0xF2, 0xF3, 0xF4, 0xF5, 0xF6, 0xF7, 0xF8, 0xF9, 0xFA, 0xFF, 0xDA, 0x00, 0x08, 0x01,
		 0x01, 0x00, 0x00, 0x3F, 0x00, 0xF8, 0xBE, 0xBF, 0x4A, 0x2B, 0xF0, 0x6E, 0xBF, 0xB1, 0x4A, 0xFF, 0xD9}
	};
	using namespace engine;
	assets::TextureData Source() {
		assets::TextureData image;
		image.Width = 2;
		image.Height = 1;
		image.Pixels = {
			std::byte{255},
			std::byte{0},
			std::byte{0},
			std::byte{255},
			std::byte{0},
			std::byte{0},
			std::byte{255},
			std::byte{128}
		};
		return image;
	}
}

TEST_CASE("Image graph bake exports an ordinary texture with authored flipped pixels", "[imagegraph]") {
	using namespace engine;
	const auto source = Source();
	core::ByteWriter encoded;
	REQUIRE(assets::Texture::Write(encoded, source));
	imagegraph::Document document;
	document.Nodes = {
		{"source", imagegraph::Source{"source.atex"}, {}},
		{"flipped", imagegraph::Flip{true, false}, {"source"}}
	};
	document.Outputs = {{"sprite", "flipped"}};
	const auto resolver = [&](std::string_view name, imagegraph::Image &out, std::string &failure) {
		return bake::DecodeImageGraphSource(name, encoded.Bytes(), out, failure);
	};
	assets::TextureData result;
	imagegraph::Diagnostic diagnostic;
	REQUIRE(bake::BakeImageGraph(document, "sprite", resolver, result, diagnostic));
	CHECK(result.Width == 2);
	CHECK(result.Height == 1);
	const std::vector<std::byte> expected{
		std::byte{0},
		std::byte{0},
		std::byte{255},
		std::byte{128},
		std::byte{255},
		std::byte{0},
		std::byte{0},
		std::byte{255}
	};
	CHECK(result.Pixels == expected);
	core::ByteWriter baked;
	REQUIRE(assets::Texture::Write(baked, result));
	core::ByteReader reader(baked.Bytes());
	assets::TextureData ordinary;
	REQUIRE(assets::Texture::Read(reader, ordinary));
	CHECK(reader.AtEnd());
	CHECK(ordinary.Pixels == expected);
}

TEST_CASE("Image graph source adapter preserves images on malformed or unbounded inputs", "[imagegraph]") {
	using namespace engine;
	imagegraph::Image previous{1, 1, {std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4}}};
	const auto before = previous.Pixels;
	std::string failure;
	const std::array<std::byte, 4> malformed{std::byte{'A'}, std::byte{'T'}, std::byte{'X'}, std::byte{'1'}};
	CHECK_FALSE(bake::DecodeImageGraphSource("bad.atex", malformed, previous, failure));
	CHECK_FALSE(failure.empty());
	CHECK(previous.Pixels == before);
	core::ByteWriter texture;
	REQUIRE(assets::Texture::Write(texture, Source()));
	auto trailing = texture.Bytes();
	std::vector<std::byte> bytes(trailing.begin(), trailing.end());
	bytes.push_back(std::byte{0});
	CHECK_FALSE(bake::DecodeImageGraphSource("trailing.atex", bytes, previous, failure));
	CHECK(previous.Pixels == before);
	// A second IHDR must not bypass the pre-decode dimension check.
	std::vector<std::byte> png{
		std::byte{0x89},
		std::byte{'P'},
		std::byte{'N'},
		std::byte{'G'},
		std::byte{13},
		std::byte{10},
		std::byte{26},
		std::byte{10}
	};
	const auto header = [&](uint32_t width) {
		png.insert(
			png.end(),
			{std::byte{0},
			 std::byte{0},
			 std::byte{0},
			 std::byte{13},
			 std::byte{'I'},
			 std::byte{'H'},
			 std::byte{'D'},
			 std::byte{'R'}}
		);
		for (int shift = 24; shift >= 0; shift -= 8)
			png.push_back(std::byte((width >> shift) & 255));
		png.insert(
			png.end(),
			{std::byte{0},
			 std::byte{0},
			 std::byte{0},
			 std::byte{1},
			 std::byte{8},
			 std::byte{6},
			 std::byte{0},
			 std::byte{0},
			 std::byte{0},
			 std::byte{0},
			 std::byte{0},
			 std::byte{0},
			 std::byte{0}}
		);
	};
	header(1);
	header(8192);
	CHECK_FALSE(bake::DecodeImageGraphSource("large.png", png, previous, failure));
	CHECK(failure.find("dimensions") != std::string::npos);
	CHECK(previous.Pixels == before);
}

TEST_CASE("Image graph texture copy and failed evaluation preserve the caller result", "[imagegraph]") {
	using namespace engine;
	assets::TextureData output = Source();
	const auto before = output.Pixels;
	std::string failure;
	CHECK_FALSE(bake::ImageGraphTexture({}, output, failure));
	CHECK(output.Pixels == before);
	imagegraph::Document document;
	document.Nodes = {{"source", imagegraph::Source{"missing.png"}, {}}};
	document.Outputs = {{"sprite", "source"}};
	imagegraph::Diagnostic diagnostic;
	const auto missing = [](std::string_view, imagegraph::Image &, std::string &reason) {
		reason = "missing source";
		return false;
	};
	CHECK_FALSE(bake::BakeImageGraph(document, {}, missing, output, diagnostic));
	CHECK(output.Pixels == before);
	CHECK(diagnostic.Message.find("missing") != std::string::npos);
	imagegraph::Image valid{2, 1, before};
	REQUIRE(bake::ImageGraphTexture(valid, output, failure));
	valid.Pixels[0] = std::byte{0};
	CHECK(output.Pixels == before);
}

TEST_CASE("Image graph source converts ordinary single-channel texture pixels for display", "[imagegraph]") {
	using namespace engine;
	assets::TextureData source;
	source.Width = 2;
	source.Height = 1;
	source.Format = assets::TextureFormat::R8;
	source.Pixels = {std::byte{32}, std::byte{192}};
	core::ByteWriter encoded;
	REQUIRE(assets::Texture::Write(encoded, source));
	imagegraph::Image decoded;
	std::string failure;
	REQUIRE(bake::DecodeImageGraphSource("grey.atex", encoded.Bytes(), decoded, failure));
	const std::vector<std::byte> expected{
		std::byte{99},
		std::byte{99},
		std::byte{99},
		std::byte{255},
		std::byte{225},
		std::byte{225},
		std::byte{225},
		std::byte{255}
	};
	CHECK(decoded.Pixels == expected);
}

TEST_CASE("Image graph raster admission accepts bounded PNG JPEG and GIF source pixels", "[imagegraph]") {
	using namespace engine;
	imagegraph::Image decoded;
	std::string failure;
	REQUIRE(bake::DecodeImageGraphSource("colour.png", std::as_bytes(std::span(PNG_RGB)), decoded, failure));
	CHECK(decoded.Width == 2);
	CHECK(decoded.Height == 2);
	const std::vector<std::byte> expected{
		std::byte{255},
		std::byte{0},
		std::byte{0},
		std::byte{255},
		std::byte{0},
		std::byte{255},
		std::byte{0},
		std::byte{255},
		std::byte{0},
		std::byte{0},
		std::byte{255},
		std::byte{255},
		std::byte{255},
		std::byte{255},
		std::byte{0},
		std::byte{255}
	};
	CHECK(decoded.Pixels == expected);
	REQUIRE(bake::DecodeImageGraphSource("grey.jpg", std::as_bytes(std::span(JPEG_GREY)), decoded, failure));
	CHECK(decoded.Width == 16);
	CHECK(decoded.Height == 16);
	const auto value = std::to_integer<uint8_t>(decoded.Pixels[(4 * 16 + 4) * 4]);
	CHECK(value >= 73);
	CHECK(value <= 79);
	CHECK(decoded.Pixels[(4 * 16 + 4) * 4 + 3] == std::byte{255});
	const std::array<uint8_t, 35> gif{'G', 'I', 'F', '8', '9', 'a', 2,	 0,	   1,	 0, 0x80, 0,
									  0,   255, 0,	 0,	  0,   0,	255, 0x2c, 0,	 0, 0,	  0,
									  2,   0,	1,	 0,	  0,   2,	2,	 0x44, 0x0a, 0, 0x3b};
	REQUIRE(bake::DecodeImageGraphSource("frame.gif", std::as_bytes(std::span(gif)), decoded, failure));
	CHECK(decoded.Width == 2);
	CHECK(decoded.Height == 1);
	CHECK(decoded.Pixels[0] == std::byte{255});
	CHECK(decoded.Pixels[4 + 2] == std::byte{255});
}

TEST_CASE("Image graph texture source encodes linear RGB while preserving straight alpha", "[imagegraph]") {
	using namespace engine;
	const auto decode = [](assets::TextureFormat format, std::vector<std::byte> pixels) {
		assets::TextureData source;
		source.Width = source.Height = 1;
		source.Format = format;
		source.Pixels = std::move(pixels);
		core::ByteWriter encoded;
		REQUIRE(assets::Texture::Write(encoded, source));
		imagegraph::Image decoded;
		std::string failure;
		REQUIRE(bake::DecodeImageGraphSource("source.atex", encoded.Bytes(), decoded, failure));
		return decoded.Pixels;
	};
	const std::vector<std::byte> linear{std::byte{128}, std::byte{128}, std::byte{128}, std::byte{128}};
	const std::vector<std::byte> srgb{std::byte{188}, std::byte{188}, std::byte{188}, std::byte{128}};
	CHECK(decode(assets::TextureFormat::RGBA8_LINEAR, linear) == srgb);
	CHECK(decode(assets::TextureFormat::RGBA8, linear) == linear);
	const std::vector<std::byte> packed{std::byte{0x88}, std::byte{0x88}};
	const std::vector<std::byte> packedSrgb{std::byte{193}, std::byte{193}, std::byte{193}, std::byte{136}};
	const std::vector<std::byte> packedEncoded{
		std::byte{136}, std::byte{136}, std::byte{136}, std::byte{136}
	};
	CHECK(decode(assets::TextureFormat::RGBA4_UNORM, packed) == packedSrgb);
	CHECK(decode(assets::TextureFormat::RGBA4_SRGB, packed) == packedEncoded);
	const std::vector<std::byte> half{
		std::byte{0},
		std::byte{0x38},
		std::byte{0},
		std::byte{0x38},
		std::byte{0},
		std::byte{0x38},
		std::byte{0},
		std::byte{0x34}
	};
	const std::vector<std::byte> floatingExpected{
		std::byte{188}, std::byte{188}, std::byte{188}, std::byte{64}
	};
	CHECK(decode(assets::TextureFormat::RGBA16_FLOAT, half) == floatingExpected);
	const std::vector<std::byte> floating{
		std::byte{0},
		std::byte{0},
		std::byte{0},
		std::byte{0x3f},
		std::byte{0},
		std::byte{0},
		std::byte{0},
		std::byte{0x3f},
		std::byte{0},
		std::byte{0},
		std::byte{0},
		std::byte{0x3f},
		std::byte{0},
		std::byte{0},
		std::byte{0x80},
		std::byte{0x3e}
	};
	CHECK(decode(assets::TextureFormat::RGBA32_FLOAT, floating) == floatingExpected);
	const std::vector<std::byte> grey{std::byte{188}, std::byte{188}, std::byte{188}, std::byte{255}};
	CHECK(decode(assets::TextureFormat::R8, {std::byte{128}}) == grey);
	CHECK(decode(assets::TextureFormat::R16_FLOAT, {std::byte{0}, std::byte{0x38}}) == grey);
	CHECK(
		decode(
			assets::TextureFormat::R32_FLOAT, {std::byte{0}, std::byte{0}, std::byte{0}, std::byte{0x3f}}
		) == grey
	);
}

TEST_CASE("Image graph GIF admission refuses an over-wide atlas in the decoder", "[imagegraph]") {
	using namespace engine;
	std::vector<uint8_t> gif{'G', 'I', 'F', '8', '9', 'a', 0, 16, 1, 0, 0x80, 0, 0, 255, 0, 0, 0, 0, 255};
	for (int frame = 0; frame < 2; ++frame)
		gif.insert(gif.end(), {0x2c, 0, 0, 0, 0, 2, 0, 1, 0, 0, 2, 2, 0x44, 0x0a, 0});
	gif.push_back(0x3b);
	imagegraph::Image previous{1, 1, {std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4}}};
	const auto before = previous.Pixels;
	std::string failure;
	CHECK_FALSE(bake::DecodeImageGraphSource("wide.gif", std::as_bytes(std::span(gif)), previous, failure));
	// This comes from the atlas admission gate before allocation, rather than post-decode dimensions.
	CHECK(failure.find("flipbook") != std::string::npos);
	CHECK(previous.Pixels == before);
}
