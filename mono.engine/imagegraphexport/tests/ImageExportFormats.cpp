#include "StillExport.hpp"

#include <engine/imagegraphexport/GraphExport.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <map>
#include <span>

TEST_SUITE_ID("engine.imagegraphexport.image_export_formats")
using namespace engine::imagegraph;
using namespace engine::imagegraphexport;
namespace {
	struct Reader {
		std::span<const uint8_t> Bytes;
		size_t Position = 0;
		uint64_t Word(size_t count) {
			REQUIRE(count <= 8);
			REQUIRE(Position <= Bytes.size());
			REQUIRE(count <= Bytes.size() - Position);
			uint64_t word = 0;
			for (size_t index = 0; index < count; ++index)
				word |= uint64_t(Bytes[Position++]) << (index * 8);
			return word;
		}
		std::string Text() {
			const size_t begin = Position;
			while (Position < Bytes.size() && Bytes[Position]) {
				REQUIRE(Position - begin < 256);
				++Position;
			}
			REQUIRE(Position < Bytes.size());
			std::string text(Bytes.begin() + begin, Bytes.begin() + Position);
			++Position;
			return text;
		}
		float Float() {
			return std::bit_cast<float>(uint32_t(Word(4)));
		}
	};
	struct Attribute {
		std::string Type;
		std::span<const uint8_t> Data;
	};
	// This reader checks the wire structure directly, without using the encoder's helpers.
	void InspectExr(const std::vector<uint8_t> &bytes, const std::array<SurfacePixel, 4> &pixels, bool mono) {
		Reader reader{bytes};
		REQUIRE(reader.Word(4) == 20000630);
		REQUIRE(reader.Word(4) == 2);
		std::map<std::string, Attribute> attributes;
		while (reader.Position < bytes.size() && bytes[reader.Position]) {
			const auto name = reader.Text();
			const auto type = reader.Text();
			const auto length = reader.Word(4);
			REQUIRE(length <= bytes.size() - reader.Position);
			REQUIRE(attributes
						.emplace(name, Attribute{type, std::span(bytes).subspan(reader.Position, length)})
						.second);
			reader.Position += length;
		}
		REQUIRE(reader.Word(1) == 0);
		REQUIRE(attributes.contains("channels"));
		CHECK(attributes.at("channels").Type == "chlist");
		Reader channels{attributes.at("channels").Data};
		std::vector<std::string> names;
		while (channels.Position < channels.Bytes.size() && channels.Bytes[channels.Position]) {
			names.push_back(channels.Text());
			CHECK(channels.Word(4) == 2);
			CHECK(channels.Word(1) == 0);
			CHECK(channels.Word(3) == 0);
			CHECK(channels.Word(4) == 1);
			CHECK(channels.Word(4) == 1);
		}
		REQUIRE(channels.Word(1) == 0);
		CHECK(channels.Position == channels.Bytes.size());
		CHECK(names == (mono ? std::vector<std::string>{"Y"} : std::vector<std::string>{"A", "B", "G", "R"}));
		for (const auto window : {"dataWindow", "displayWindow"}) {
			REQUIRE(attributes.contains(window));
			CHECK(attributes.at(window).Type == "box2i");
			Reader box{attributes.at(window).Data};
			CHECK(box.Word(4) == 0);
			CHECK(box.Word(4) == 0);
			CHECK(box.Word(4) == 1);
			CHECK(box.Word(4) == 1);
			CHECK(box.Position == box.Bytes.size());
		}
		REQUIRE(attributes.contains("compression"));
		REQUIRE(attributes.at("compression").Data.size() == 1);
		CHECK(attributes.at("compression").Data.front() == 0);
		std::array<uint64_t, 2> offsets{reader.Word(8), reader.Word(8)};
		const size_t scanlineBytes = names.size() * 2 * sizeof(float);
		CHECK(offsets[0] == reader.Position);
		CHECK(offsets[1] == offsets[0] + 8 + scanlineBytes);
		for (size_t y = 0; y < 2; ++y) {
			Reader row{bytes, size_t(offsets[y])};
			CHECK(row.Word(4) == y);
			CHECK(row.Word(4) == scanlineBytes);
			for (const auto &name : names) {
				const size_t component = name == "A" ? 3 : name == "B" ? 2 : name == "G" ? 1 : 0;
				for (size_t x = 0; x < 2; ++x)
					CHECK(row.Float() == static_cast<float>(pixels[y * 2 + x][component]));
			}
			CHECK(row.Position == offsets[y] + 8 + scanlineBytes);
		}
		CHECK(offsets[1] + 8 + scanlineBytes == bytes.size());
	}
	void AppendWord(std::vector<uint8_t> &bytes, uint32_t word, size_t count) {
		for (size_t index = 0; index < count; ++index)
			bytes.push_back(uint8_t(word >> (index * 8)));
	}
	Image FloatImage(SurfaceFormat format, const std::array<SurfacePixel, 4> &pixels, bool mono) {
		INFO("surface format " << int(format));
		Image image;
		image.Width = image.Height = 2;
		image.Format = format;
		const bool half = format == SurfaceFormat::R16Float || format == SurfaceFormat::RGBA16Float;
		for (const auto &pixel : pixels)
			for (size_t channel = 0; channel < (mono ? 1 : 4); ++channel) {
				const auto value = float(pixel[channel]);
				// The fixture values are exact binary fractions; literal half words avoid the production
				// converter.
				if (half) {
					const uint16_t word = value == .25f	  ? 0x3400
										  : value == -.5f ? 0xb800
										  : value == 2	  ? 0x4000
										  : value == .75f ? 0x3a00
										  : value == 1	  ? 0x3c00
														  : 0;
					AppendWord(image.Pixels, word, 2);
				} else
					AppendWord(image.Pixels, std::bit_cast<uint32_t>(value), 4);
			}
		return image;
	}
}

TEST_CASE(
	"Native EXR retains named planar samples across every numeric surface format", "[imagegraph][export]"
) {
	const std::array<SurfacePixel, 4> floating{
		{{.25, -.5, 2, .75}, {2, .25, -.5, 1}, {-.5, 2, .75, 0}, {.75, 1, .25, .25}}
	};
	for (const auto format :
		 {SurfaceFormat::RGBA16Float,
		  SurfaceFormat::RGBA32Float,
		  SurfaceFormat::R16Float,
		  SurfaceFormat::R32Float}) {
		INFO("surface format " << int(format));
		const bool mono = format == SurfaceFormat::R16Float || format == SurfaceFormat::R32Float;
		const auto image = FloatImage(format, floating, mono);
		std::vector<uint8_t> bytes;
		std::string failure;
		REQUIRE(runner::EncodeStill(".exr", image, bytes, failure));
		InspectExr(bytes, floating, mono);
	}
	for (const auto format : {SurfaceFormat::RGBA8Unorm, SurfaceFormat::RGBA4Unorm, SurfaceFormat::R8Unorm}) {
		INFO("surface format " << int(format));
		Image image;
		image.Width = image.Height = 2;
		image.Format = format;
		std::array<SurfacePixel, 4> expected{};
		if (format == SurfaceFormat::RGBA4Unorm) {
			image.Pixels = {0x21, 0x43, 0x65, 0x87, 0xa9, 0xcb, 0xed, 0x0f};
			for (size_t pixel = 0; pixel < 4; ++pixel)
				for (size_t channel = 0; channel < 4; ++channel)
					expected[pixel][channel] = double((pixel * 4 + channel + 1) % 16) / 15;
		} else if (format == SurfaceFormat::R8Unorm) {
			image.Pixels = {0, 128, 255, 42};
			for (size_t pixel = 0; pixel < 4; ++pixel)
				expected[pixel][0] = image.Pixels[pixel] / 255.0;
		} else {
			image.Pixels = {12, 34, 56, 78, 255, 0, 128, 255, 0, 10, 20, 0, 9, 8, 7, 6};
			for (size_t pixel = 0; pixel < 4; ++pixel)
				for (size_t channel = 0; channel < 4; ++channel)
					expected[pixel][channel] = image.Pixels[pixel * 4 + channel] / 255.0;
		}
		std::vector<uint8_t> bytes;
		std::string failure;
		REQUIRE(runner::EncodeStill(".exr", image, bytes, failure));
		InspectExr(bytes, expected, format == SurfaceFormat::R8Unorm);
	}
}

TEST_CASE("Native BMP writes padded bottom-up BGR rows with its black-alpha policy", "[imagegraph][export]") {
	Image image{1, 2, {255, 0, 128, 128, 12, 34, 56, 255}};
	std::vector<uint8_t> bytes;
	std::string failure;
	REQUIRE(runner::EncodeStill(".bmp", image, bytes, failure));
	Reader header{bytes};
	CHECK(header.Word(2) == 0x4d42);
	CHECK(header.Word(4) == 62);
	CHECK(header.Word(4) == 0);
	CHECK(header.Word(4) == 54);
	CHECK(header.Word(4) == 40);
	CHECK(header.Word(4) == 1);
	CHECK(header.Word(4) == 2);
	CHECK(header.Word(2) == 1);
	CHECK(header.Word(2) == 24);
	CHECK(header.Word(4) == 0);
	CHECK(header.Word(4) == 8);
	REQUIRE(bytes.size() == 62);
	CHECK(
		std::vector<uint8_t>(bytes.begin() + 54, bytes.end()) ==
		std::vector<uint8_t>{56, 34, 12, 0, 64, 0, 128, 0}
	);
	// This checks the native float-before-truncation policy, not GameMaker's intermediate RGBA8 conversion.
	const std::array<SurfacePixel, 4> samples{
		{{.25, -.5, 2, .75}, {.25, -.5, 2, .75}, {.25, -.5, 2, .75}, {.25, -.5, 2, .75}}
	};
	const auto floating = FloatImage(SurfaceFormat::RGBA32Float, samples, false);
	REQUIRE(runner::EncodeStill(".bmp", floating, bytes, failure));
	REQUIRE(bytes.size() == 70);
	CHECK(
		std::vector<uint8_t>(bytes.begin() + 54, bytes.begin() + 62) ==
		std::vector<uint8_t>{191, 0, 47, 191, 0, 47, 0, 0}
	);
	Image red{1, 1, {128}, 0, SurfaceFormat::R8Unorm};
	REQUIRE(runner::EncodeStill(".bmp", red, bytes, failure));
	REQUIRE(bytes.size() == 58);
	CHECK(std::vector<uint8_t>(bytes.begin() + 54, bytes.end()) == std::vector<uint8_t>{0, 0, 128, 0});
}

TEST_CASE("PNG conversion subformats retain exact literal target routes", "[imagegraph][export]") {
	GraphExportSettings settings;
	settings.Output = "image.png";
	settings.ImageEncoder = "/explicit/image encoder";
	settings.Quality = 37;
	for (const uint8_t subformat : {uint8_t{0}, uint8_t{1}}) {
		settings.PngSubformat = subformat;
		std::filesystem::path executable;
		std::vector<std::string> arguments;
		std::string failure;
		REQUIRE(BuildGraphEncoderArguments(
			settings, "/frames", "/out/image.png", 1, executable, arguments, failure
		));
		CHECK(executable == settings.ImageEncoder);
		CHECK(
			arguments == std::vector<std::string>{
							 "/frames/frame00000000.png",
							 "-quality",
							 "37",
							 subformat == 1 ? "PNG8:/out/image.png" : "/out/image.png"
						 }
		);
	}
}
