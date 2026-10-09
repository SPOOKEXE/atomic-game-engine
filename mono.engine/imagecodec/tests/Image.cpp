#include "Fixtures.hpp"
#include "Images.hpp"

#include <engine/imagecodec/Image.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdlib>
#include <limits>
#include <string>
#include <vector>

TEST_SUITE_ID("engine.imagecodec.image")
using namespace engine::imagecodec;
using namespace fixtures;
namespace {
	struct Pixel {
		int R = 0, G = 0, B = 0, A = 0;
		bool operator==(const Pixel &) const = default;
	};
	Pixel At(const Image &image, uint32_t x, uint32_t y) {
		const size_t offset = (static_cast<size_t>(y) * image.Width + x) * 4;
		return {
			static_cast<int>(image.Pixels[offset]),
			static_cast<int>(image.Pixels[offset + 1]),
			static_cast<int>(image.Pixels[offset + 2]),
			static_cast<int>(image.Pixels[offset + 3])
		};
	}
	bool ReadImage(std::span<const std::byte> bytes, Image &out, std::string &failure) {
		return Decode(
			bytes, !bytes.empty() && bytes[0] == std::byte{0x89} ? Format::Png : Format::Jpeg, out, failure
		);
	}
	Image Decoded(std::span<const uint8_t> bytes) {
		Image out;
		std::string failure;
		REQUIRE(ReadImage(Bytes(bytes), out, failure));
		REQUIRE(failure.empty());
		return out;
	}
}

TEST_CASE("the Sub and Paeth filters are reversed correctly", "[imagecodec]") {
	// The values below are the specification's arithmetic done by hand. A Paeth
	// predictor computed in bytes rather than in `int` wraps on the subtraction
	// and produces a picture that decodes cleanly and looks like noise, so the
	// second row is the one worth checking a number at a time.
	const Image image = Decoded(PNG_RGBA_FILTERED);

	CHECK(At(image, 0, 0) == Pixel{10, 20, 30, 40});
	CHECK(At(image, 1, 0) == Pixel{15, 25, 35, 45});
	CHECK(At(image, 0, 1) == Pixel{11, 21, 31, 41});
	CHECK(At(image, 1, 1) == Pixel{17, 27, 37, 47});
}

TEST_CASE("a palette png expands through its palette and transparency", "[imagecodec]") {
	const Image image = Decoded(PNG_PALETTE);

	CHECK(At(image, 0, 0) == Pixel{9, 8, 7, 255});
	CHECK(At(image, 1, 0) == Pixel{6, 5, 4, 128});
}

TEST_CASE("a grey-alpha png widens to four channels", "[imagecodec]") {
	const Image image = Decoded(PNG_GREY_ALPHA);

	// Widened rather than kept as two channels, so a renderer never has to ask
	// how many channels an arbitrary texture has.
	CHECK(At(image, 0, 0) == Pixel{100, 100, 100, 200});
	CHECK(At(image, 1, 0) == Pixel{50, 50, 50, 25});
}

TEST_CASE("sixteen-bit samples are truncated to their high byte", "[imagecodec]") {
	const Image image = Decoded(PNG_SIXTEEN_BIT);
	CHECK(At(image, 0, 0) == Pixel{0xAA, 0xCC, 0xEE, 255});
}

TEST_CASE("an interlaced png is refused by name", "[imagecodec]") {
	Image image;
	std::string failure;

	// Half-reading one produces a picture that is recognisably the right image
	// and wrong everywhere, which is worse than not reading it.
	CHECK_FALSE(ReadImage(Bytes(PNG_INTERLACED), image, failure));
	CHECK(failure.find("interlaced") != std::string::npos);
}

TEST_CASE("a corrupt png chunk is refused rather than decoded", "[imagecodec]") {
	std::vector<uint8_t> corrupt(PNG_RGB.begin(), PNG_RGB.end());

	// One flipped byte inside the compressed data. Without the chunk checksum
	// this is a picture of static rather than a refusal, and the file it came
	// from is a truncated download nobody would think to suspect.
	corrupt[50] ^= 0xFF;

	Image image;
	std::string failure;
	CHECK_FALSE(ReadImage(Bytes(corrupt), image, failure));
	CHECK_FALSE(failure.empty());
}

TEST_CASE("png inflation stops at the header size and still rejects short streams", "[imagecodec]") {
	Image image = Decoded(PNG_RGB);
	const Image original = image;
	std::string failure;

	SECTION("excess output is refused by the bounded sink") {
		CHECK_FALSE(ReadImage(Bytes(PNG_EXCESS_INFLATION), image, failure));
		CHECK(failure == "png: inflated data exceeds the header size");
	}
	SECTION("short output still fails the exact size check") {
		CHECK_FALSE(ReadImage(Bytes(PNG_SHORT_INFLATION), image, failure));
		CHECK(failure == "png: inflated size disagrees with the header");
	}

	CHECK(image.Width == original.Width);
	CHECK(image.Height == original.Height);
	CHECK(image.Pixels == original.Pixels);
}

TEST_CASE("a 4:4:4 jpeg decodes to within the transform's own rounding", "[imagecodec]") {
	const Image image = Decoded(JPEG_444);

	CHECK(image.Width == 16);
	CHECK(image.Height == 16);

	// The expected values are libjpeg's, through Pillow, and the tolerance is
	// three levels - which is what an inverse DCT done in floats differs from
	// one done in fixed point by. A wider tolerance here would stop catching
	// the thing worth catching: a chroma plane sampled the wrong way, which is
	// tens of levels out on exactly these edges.
	const auto near = [](Pixel measured, Pixel expected) {
		CHECK(std::abs(measured.R - expected.R) <= 3);
		CHECK(std::abs(measured.G - expected.G) <= 3);
		CHECK(std::abs(measured.B - expected.B) <= 3);
		CHECK(measured.A == 255);
	};

	near(At(image, 4, 4), {254, 0, 0, 255});
	near(At(image, 12, 4), {0, 255, 1, 255});
	near(At(image, 4, 12), {0, 0, 254, 255});
	near(At(image, 12, 12), {255, 255, 0, 255});
}

TEST_CASE("a 4:2:0 jpeg upsamples chroma the way libjpeg does", "[imagecodec]") {
	// The case that made the first upsampling wrong. Nearest-neighbour chroma
	// passes a flat-field test and fails here by twenty-eight levels, because
	// the quadrant boundary is a chroma edge and nothing else in a test image
	// is.
	const Image image = Decoded(JPEG_420);

	const auto near = [](Pixel measured, Pixel expected) {
		CHECK(std::abs(measured.R - expected.R) <= 3);
		CHECK(std::abs(measured.G - expected.G) <= 3);
		CHECK(std::abs(measured.B - expected.B) <= 3);
	};

	near(At(image, 4, 4), {251, 2, 0, 255});
	near(At(image, 12, 4), {1, 255, 1, 255});
	near(At(image, 4, 12), {2, 0, 252, 255});
	near(At(image, 12, 12), {255, 254, 0, 255});
}

TEST_CASE("a greyscale jpeg widens to four channels", "[imagecodec]") {
	const Image image = Decoded(JPEG_GREY);

	// The luma of pure red, green, blue and yellow under BT.601.
	CHECK(std::abs(At(image, 4, 4).R - 76) <= 3);
	CHECK(std::abs(At(image, 12, 4).R - 150) <= 3);
	CHECK(std::abs(At(image, 4, 12).R - 29) <= 3);
	CHECK(std::abs(At(image, 12, 12).R - 226) <= 3);

	CHECK(At(image, 4, 4).R == At(image, 4, 4).G);
	CHECK(At(image, 4, 4).G == At(image, 4, 4).B);
	CHECK(At(image, 4, 4).A == 255);
}

TEST_CASE("a progressive jpeg is refused by name", "[imagecodec]") {
	Image image;
	std::string failure;

	// Read as baseline it produces a blurred version of the right picture,
	// which looks like a quality setting rather than a bug.
	CHECK_FALSE(ReadImage(Bytes(JPEG_PROGRESSIVE), image, failure));
	CHECK(failure.find("progressive") != std::string::npos);
	CHECK(image.Pixels.empty());
}

TEST_CASE("a truncated jpeg is refused or whole, never half", "[imagecodec]") {
	for (size_t length = 0; length < JPEG_444.size(); length++) {
		Image image;
		std::string failure;
		if (ReadImage(Bytes(std::span(JPEG_444.data(), length)), image, failure)) {
			// grug return whole image or refuse, never partly filled pixels.
			REQUIRE(
				(image.Width != 0 && image.Height != 0 &&
				 image.Pixels.size() == static_cast<size_t>(image.Width) * image.Height * 4)
			);
			CHECK(image.Width == 16);
			CHECK(image.Height == 16);
		} else {
			CHECK(image.Pixels.empty());
			CHECK_FALSE(failure.empty());
		}
	}
}

TEST_CASE("shared PNG import preserves encoded colours and adapter byte order", "[imagecodec]") {
	const Image image = Decoded(PNG_RGB);
	CHECK(image.Width == 2);
	CHECK(image.Height == 2);
	CHECK(At(image, 0, 0) == Pixel{255, 0, 0, 255});
	CHECK(At(image, 1, 0) == Pixel{0, 255, 0, 255});
	CHECK(At(image, 0, 1) == Pixel{0, 0, 255, 255});
	CHECK(At(image, 1, 1) == Pixel{255, 255, 0, 255});
}

TEST_CASE("base64 accepts canonical vectors and roundtrips every byte", "[imagecodec]") {
	for (const auto &[plain, encoded] : std::array<std::pair<std::string_view, std::string_view>, 7>{
			 {{"", ""},
			  {"f", "Zg=="},
			  {"fo", "Zm8="},
			  {"foo", "Zm9v"},
			  {"foob", "Zm9vYg=="},
			  {"fooba", "Zm9vYmE="},
			  {"foobar", "Zm9vYmFy"}}
		 }) {
		std::vector<std::byte> decoded;
		std::string result, failure;
		REQUIRE(DecodeBase64(encoded, decoded, failure));
		CHECK(
			decoded == std::vector<std::byte>(
						   std::as_bytes(std::span(plain)).begin(), std::as_bytes(std::span(plain)).end()
					   )
		);
		REQUIRE(EncodeBase64(decoded, result, failure));
		CHECK(result == encoded);
	}
	std::vector<std::byte> all(256);
	for (size_t i = 0; i < all.size(); i++)
		all[i] = static_cast<std::byte>(i);
	std::string text, failure;
	std::vector<std::byte> restored;
	REQUIRE(EncodeBase64(all, text, failure));
	REQUIRE(DecodeBase64(text, restored, failure));
	CHECK(restored == all);
}

TEST_CASE("noncanonical base64 and excess bytes preserve prior outputs", "[imagecodec]") {
	const std::vector<std::byte> original{std::byte{71}};
	for (std::string_view malformed :
		 {"A",
		  "AAA",
		  "=AAA",
		  "A=AA",
		  "AA=A",
		  "AA==AAAA",
		  "AAA=AAAA",
		  "AB==",
		  "AAB=",
		  "AA-_",
		  "AA\nA",
		  "AA A",
		  "data:image/png;base64,AA=="}) {
		std::vector<std::byte> out = original;
		std::string failure;
		CAPTURE(malformed);
		CHECK_FALSE(DecodeBase64(malformed, out, failure));
		CHECK(out == original);
		CHECK_FALSE(failure.empty());
	}
	std::vector<std::byte> out = original;
	std::string failure;
	CHECK_FALSE(DecodeBase64("AAAA", out, failure, 2));
	CHECK(out == original);
	REQUIRE(DecodeBase64("AAA=", out, failure, 2));
	CHECK(out.size() == 2);
	std::string text = "prior";
	CHECK_FALSE(EncodeBase64(out, text, failure, 1));
	CHECK(text == "prior");
}

TEST_CASE("raw RGBA8 enforces exact dimensions and 1080p budget atomically", "[imagecodec]") {
	const Image original = Decoded(PNG_RGB);
	Image out = original;
	std::string failure;
	std::vector<std::byte> pixels(8294400, std::byte{128});
	REQUIRE(DecodeRaw(1920, 1080, pixels, out, failure));
	CHECK(out.Width == 1920);
	CHECK(out.Height == 1080);
	CHECK(out.Pixels == pixels);
	std::string base64;
	REQUIRE(EncodeBase64(pixels, base64, failure));
	CHECK(base64.size() == 11059200);
	std::vector<std::byte> restored;
	REQUIRE(DecodeBase64(base64, restored, failure));
	CHECK(restored == pixels);
	base64.append("AAAA");
	CHECK_FALSE(DecodeBase64(base64, restored, failure));
	CHECK(restored == pixels);
	out = original;
	for (auto [width, height] : std::array<std::pair<uint32_t, uint32_t>, 5>{
			 {{0, 1}, {1921, 1080}, {1920, 1081}, {UINT32_MAX, UINT32_MAX}, {2, 1}}
		 }) {
		CAPTURE(width, height);
		CHECK_FALSE(DecodeRaw(width, height, pixels, out, failure));
		CHECK(out.Pixels == original.Pixels);
		CHECK(out.Width == original.Width);
	}
	const std::array<std::byte, 8> raw{
		std::byte{0},
		std::byte{100},
		std::byte{255},
		std::byte{0},
		std::byte{20},
		std::byte{30},
		std::byte{40},
		std::byte{128}
	};
	REQUIRE(DecodeRaw(2, 1, raw, out, failure));
	CHECK(At(out, 0, 0) == Pixel{0, 100, 255, 0});
	CHECK(At(out, 1, 0) == Pixel{20, 30, 40, 128});
}

TEST_CASE("encoded imports check byte and RGBA8 ceilings and explicit format", "[imagecodec]") {
	Image out = Decoded(PNG_RGB);
	const Image original = out;
	std::string failure;
	Limits limits;
	limits.MaximumEncodedBytes = PNG_RGB.size() - 1;
	CHECK_FALSE(Decode(Bytes(PNG_RGB), Format::Png, out, failure, limits));
	limits = Limits{};
	limits.MaximumPixelBytes = 15;
	CHECK_FALSE(Decode(Bytes(PNG_RGB), Format::Png, out, failure, limits));
	limits = Limits{};
	limits.MaximumWidth = 1;
	CHECK_FALSE(Decode(Bytes(PNG_RGB), Format::Png, out, failure, limits));
	CHECK_FALSE(Decode(Bytes(PNG_RGB), Format::Jpeg, out, failure));
	CHECK_FALSE(Decode(Bytes(JPEG_GREY), Format::Png, out, failure));
	CHECK_FALSE(Decode(Bytes(PNG_RGB), static_cast<Format>(255), out, failure));
	CHECK(out.Pixels == original.Pixels);
	CHECK(out.Width == original.Width);
}

TEST_CASE("truncated JPEG entropy refuses before committing pixels", "[imagecodec]") {
	Image image = Decoded(PNG_RGB);
	const auto original = image.Pixels;
	std::string failure;
	const auto bytes = Bytes(JPEG_GREY);
	for (size_t length = 0; length < bytes.size(); length++) {
		Image out = image;
		const bool decoded = Decode(bytes.first(length), Format::Jpeg, out, failure);
		if (!decoded)
			CHECK(out.Pixels == original);
		else
			CHECK(out.Pixels.size() == static_cast<size_t>(out.Width) * out.Height * 4);
	}
}

TEST_CASE("PNG and JPEG accept exact 1080p and refuse oversized headers", "[imagecodec]") {
	for (Format format : {Format::Png, Format::Jpeg}) {
		const auto pixels =
			format == Format::Png ? fixtures::GreyPng(1920, 1080) : fixtures::GreyJpeg(1920, 1080);
		Image out;
		std::string failure;
		REQUIRE(Decode(pixels, format, out, failure));
		CHECK(out.Width == 1920);
		CHECK(out.Height == 1080);
		CHECK(out.Pixels.size() == 8294400);
		CHECK(At(out, 0, 0) == Pixel{128, 128, 128, 255});
		CHECK(At(out, 1919, 1079) == Pixel{128, 128, 128, 255});
		const auto original = out.Pixels;
		for (auto [width, height] :
			 std::array<std::pair<uint16_t, uint16_t>, 2>{{{1921, 1080}, {1920, 1081}}}) {
			const auto oversized =
				format == Format::Png ? fixtures::GreyPng(width, height) : fixtures::GreyJpeg(width, height);
			CHECK_FALSE(Decode(oversized, format, out, failure));
			CHECK(out.Pixels == original);
		}
	}
}

TEST_CASE("malformed shared image streams preserve pixels under every fixture truncation", "[imagecodec]") {
	const Image original = Decoded(PNG_RGB);
	for (Format format : {Format::Png, Format::Jpeg}) {
		const auto bytes = format == Format::Png ? fixtures::GreyPng(16, 16) : fixtures::GreyJpeg(16, 16);
		for (size_t length = 0; length < bytes.size(); length++) {
			Image out = original;
			std::string failure;
			if (!Decode(std::span(bytes).first(length), format, out, failure)) {
				CHECK(out.Pixels == original.Pixels);
				CHECK_FALSE(failure.empty());
			}
		}
		for (size_t offset = 0; offset < bytes.size(); offset++) {
			auto corrupt = bytes;
			corrupt[offset] ^= std::byte{0xff};
			Image out = original;
			std::string failure;
			if (!Decode(corrupt, format, out, failure)) CHECK(out.Pixels == original.Pixels);
		}
	}
}

TEST_CASE(
	"JPEG entropy ending before declared blocks refuses instead of synthesizing zeroes", "[imagecodec]"
) {
	auto bytes = fixtures::GreyJpeg(16, 16);
	// grug four blocks need one entropy byte. remove byte and leave real EOI.
	bytes.erase(bytes.end() - 3);
	Image out = Decoded(PNG_RGB);
	const Image original = out;
	std::string failure;
	CHECK_FALSE(Decode(bytes, Format::Jpeg, out, failure));
	CHECK(failure == "jpeg: malformed entropy-coded data");
	CHECK(out.Pixels == original.Pixels);
}

TEST_CASE("JPEG overflowing DC coefficient is refused before transform", "[imagecodec]") {
	auto bytes = fixtures::GreyJpeg(8, 8);
	// grug maximum 16-bit DC and quantizer make product exceed int32.
	std::vector<std::byte> quant(129, std::byte{0});
	quant[0] = std::byte{0x10};
	for (size_t index = 0; index < 64; index++)
		quant[2 + index * 2] = std::byte{1};
	quant[1] = quant[2] = std::byte{0xff};
	std::vector<std::byte> replacement;
	fixtures::Segment(replacement, 0xdb, quant);
	bytes.erase(bytes.begin() + 2, bytes.begin() + 71);
	bytes.insert(bytes.begin() + 2, replacement.begin(), replacement.end());
	for (size_t offset = 0; offset + 22 < bytes.size(); offset++) {
		if (bytes[offset] == std::byte{0xff} && bytes[offset + 1] == std::byte{0xc4}) {
			bytes[offset + 21] = std::byte{16};
			break;
		}
	}
	bytes.erase(bytes.end() - 3);
	bytes.insert(bytes.end() - 2, {std::byte{0x7f}, std::byte{0xff}, std::byte{0}, std::byte{0xbf}});
	Image out = Decoded(PNG_RGB);
	const Image original = out;
	std::string failure;
	CHECK_FALSE(Decode(bytes, Format::Jpeg, out, failure));
	CHECK(failure == "jpeg: malformed entropy-coded data");
	CHECK(out.Pixels == original.Pixels);
}

TEST_CASE("JPEG oversubscribed Huffman and nonsequential scan refuse", "[imagecodec]") {
	const auto original = Decoded(PNG_RGB);
	for (bool corruptTable : {false, true}) {
		auto bytes = fixtures::GreyJpeg(8, 8);
		for (size_t offset = 0; offset + 10 < bytes.size(); offset++) {
			if (bytes[offset] != std::byte{0xff}) continue;
			if (corruptTable && bytes[offset + 1] == std::byte{0xc4}) {
				bytes[offset + 5] = std::byte{3};
				break;
			}
			if (!corruptTable && bytes[offset + 1] == std::byte{0xda}) {
				bytes[offset + 7] = std::byte{1};
				break;
			}
		}
		Image out = original;
		std::string failure;
		CHECK_FALSE(Decode(bytes, Format::Jpeg, out, failure));
		CHECK(out.Pixels == original.Pixels);
	}
}

TEST_CASE("PNG and JPEG need complete end markers before replacing output", "[imagecodec]") {
	const Image original = Decoded(PNG_RGB);
	for (Format format : {Format::Png, Format::Jpeg}) {
		const auto bytes = format == Format::Png ? fixtures::GreyPng(8, 8) : fixtures::GreyJpeg(8, 8);
		const size_t endBytes = format == Format::Png ? 12 : 2;
		for (size_t missing = 1; missing <= endBytes; missing++) {
			Image out = original;
			std::string failure;
			CHECK_FALSE(Decode(std::span(bytes).first(bytes.size() - missing), format, out, failure));
			CHECK(out.Pixels == original.Pixels);
			CHECK(out.Width == original.Width);
		}
	}
}

TEST_CASE("PNG grey and RGB colour keys preserve exact 8-bit transparency", "[imagecodec]") {
	const Image grey = Decoded(PNG_GREY_KEY);
	CHECK(At(grey, 0, 0) == Pixel{128, 128, 128, 0});
	CHECK(At(grey, 1, 0) == Pixel{127, 127, 127, 255});
	const Image rgb = Decoded(PNG_RGB_KEY);
	CHECK(At(rgb, 0, 0) == Pixel{128, 10, 20, 0});
	CHECK(At(rgb, 1, 0) == Pixel{128, 10, 21, 255});
}

TEST_CASE("PNG 16-bit colour keys compare low bytes before channel reduction", "[imagecodec]") {
	const Image grey = Decoded(PNG_GREY16_KEY);
	CHECK(At(grey, 0, 0) == Pixel{128, 128, 128, 0});
	CHECK(At(grey, 1, 0) == Pixel{128, 128, 128, 255});
	const Image rgb = Decoded(PNG_RGB16_KEY);
	CHECK(At(rgb, 0, 0) == Pixel{0x12, 0x56, 0x9a, 0});
	CHECK(At(rgb, 1, 0) == Pixel{0x12, 0x56, 0x9a, 255});
}

TEST_CASE("PNG malformed transparency refuses complete CRC-valid chunks", "[imagecodec]") {
	const Image original = Decoded(PNG_RGB);
	const auto altered =
		[](std::span<const uint8_t> raw, std::span<const std::byte> transparency, bool replace = true) {
			const auto bytes = Bytes(raw);
			std::vector<std::byte> out(bytes.begin(), bytes.begin() + 33);
			const std::byte kind[]{std::byte{'t'}, std::byte{'R'}, std::byte{'N'}, std::byte{'S'}};
			if (!replace) fixtures::Chunk(out, kind, transparency);
			size_t offset = 33;
			if (replace)
				while (offset + 12 <= bytes.size()) {
					const uint32_t count = (static_cast<uint32_t>(bytes[offset]) << 24) |
										   (static_cast<uint32_t>(bytes[offset + 1]) << 16) |
										   (static_cast<uint32_t>(bytes[offset + 2]) << 8) |
										   static_cast<uint32_t>(bytes[offset + 3]);
					if (bytes[offset + 4] == std::byte{'t'} && bytes[offset + 5] == std::byte{'R'}) {
						fixtures::Chunk(out, kind, transparency);
						offset += count + 12;
						break;
					}
					out.insert(out.end(), bytes.begin() + offset, bytes.begin() + offset + count + 12);
					offset += count + 12;
				}
			out.insert(out.end(), bytes.begin() + offset, bytes.end());
			return out;
		};
	const std::array<std::byte, 6> tooLarge{
		std::byte{1}, std::byte{128}, std::byte{0}, std::byte{10}, std::byte{0}, std::byte{20}
	};
	const std::array<std::byte, 3> tooLongPalette{std::byte{255}, std::byte{128}, std::byte{0}};
	const std::array<std::byte, 2> key{std::byte{0}, std::byte{128}};
	const std::vector<std::vector<std::byte>> cases{
		altered(PNG_GREY_KEY, {}),
		altered(PNG_GREY_KEY, std::span(key).first(1)),
		altered(PNG_RGB_KEY, key),
		altered(PNG_RGB_KEY, tooLarge),
		altered(PNG_RGBA_FILTERED, key, false),
		altered(PNG_GREY_ALPHA, key, false),
		altered(PNG_GREY_KEY, key, false),
		altered(PNG_PALETTE, tooLongPalette)
	};
	for (const auto &bytes : cases) {
		Image out = original;
		std::string failure;
		CHECK_FALSE(Decode(bytes, Format::Png, out, failure));
		CHECK(out.Pixels == original.Pixels);
		CHECK_FALSE(failure.empty());
	}
}

TEST_CASE("JPEG restart intervals require the next ordered marker without scanning ahead", "[imagecodec]") {
	auto jpeg = fixtures::GreyJpeg(16, 8);
	size_t scan = 0;
	for (size_t offset = 0; offset + 1 < jpeg.size(); offset++)
		if (jpeg[offset] == std::byte{0xff} && jpeg[offset + 1] == std::byte{0xda}) {
			scan = offset;
			break;
		}
	REQUIRE(scan != 0);
	const std::byte interval[]{std::byte{0}, std::byte{1}};
	std::vector<std::byte> restart;
	fixtures::Segment(restart, 0xdd, interval);
	jpeg.insert(jpeg.begin() + scan, restart.begin(), restart.end());
	jpeg.erase(jpeg.end() - 3);
	jpeg.insert(jpeg.end() - 2, {std::byte{0x3f}, std::byte{0xff}, std::byte{0xd0}, std::byte{0x3f}});
	Image valid;
	std::string failure;
	REQUIRE(Decode(jpeg, Format::Jpeg, valid, failure));
	CHECK(At(valid, 0, 0) == Pixel{128, 128, 128, 255});
	CHECK(At(valid, 15, 7) == Pixel{128, 128, 128, 255});
	const Image original = Decoded(PNG_RGB);
	for (bool wrongSequence : {false, true}) {
		auto corrupt = jpeg;
		if (wrongSequence)
			corrupt[corrupt.size() - 4] = std::byte{0xd1};
		else
			corrupt.erase(corrupt.end() - 5, corrupt.end() - 3);
		Image out = original;
		CHECK_FALSE(Decode(corrupt, Format::Jpeg, out, failure));
		CHECK(failure == "jpeg: missing or out-of-sequence restart marker");
		CHECK(out.Pixels == original.Pixels);
	}
}
