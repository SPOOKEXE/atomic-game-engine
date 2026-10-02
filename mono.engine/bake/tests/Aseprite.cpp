#include "fixtures/Aseprite.hpp"

#include <engine/bake/Aseprite.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <nlohmann/json.hpp>
TEST_SUITE_ID("engine.bake.aseprite")
using namespace engine::bake;
namespace {
	template <size_t N> auto Bytes(const std::array<unsigned char, N> &bytes) {
		return std::span(reinterpret_cast<const std::byte *>(bytes.data()), bytes.size());
	}
	uint32_t Little(const std::vector<std::byte> &bytes, size_t at, size_t length) {
		uint32_t value = 0;
		for (size_t index = 0; index < length; ++index)
			value |= uint32_t(std::to_integer<uint8_t>(bytes.at(at + index))) << (index * 8);
		return value;
	}
	void Store(std::vector<std::byte> &bytes, size_t at, uint32_t value, size_t length) {
		for (size_t index = 0; index < length; ++index)
			bytes.at(at + index) = std::byte((value >> (index * 8)) & 255);
	}
	std::vector<std::byte> RawCel(size_t pixelBytes) {
		const auto source = Bytes(ase_fixture::Point);
		std::vector<std::byte> bytes(source.begin(), source.end());
		size_t cel = 144;
		while (Little(bytes, cel + 4, 2) != 0x2005)
			cel += Little(bytes, cel, 4);
		const size_t oldLength = Little(bytes, cel, 4);
		bytes.erase(bytes.begin() + cel + 26, bytes.begin() + cel + oldLength);
		bytes.insert(bytes.begin() + cel + 26, pixelBytes, std::byte{0});
		Store(bytes, cel + 13, 0, 2);
		Store(bytes, cel + 22, 1, 2);
		Store(bytes, cel + 24, 1, 2);
		Store(bytes, cel, 26 + pixelBytes, 4);
		Store(bytes, 128, Little(bytes, 128, 4) - oldLength + 26 + pixelBytes, 4);
		Store(bytes, 0, bytes.size(), 4);
		return bytes;
	}

}
TEST_CASE("Aseprite reads authored indexed animation, linked cels and layer crop", "[bake][aseprite]") {
	AsepriteDocument doc;
	std::string failure;
	REQUIRE(ReadAseprite(Bytes(ase_fixture::Point), doc, failure));
	CHECK(doc.Width == 10);
	CHECK(doc.Height == 10);
	REQUIRE(doc.Frames.size() == 2);
	REQUIRE(doc.Layers.size() == 1);
	REQUIRE(doc.Frames[0].Cels.size() == 1);
	REQUIRE(doc.Frames[1].Cels.size() == 1);
	CHECK(doc.Frames[1].Cels[0].Pixels.Pixels == doc.Frames[0].Cels[0].Pixels.Pixels);
	engine::assets::TextureData full, cropped;
	REQUIRE(RenderAseprite(doc, 0, {}, false, true, full, failure));
	CHECK(full.Pixels.size() == 400);
	REQUIRE(RenderAseprite(doc, 0, doc.Layers[0].Name, true, true, cropped, failure));
	CHECK(cropped.Width == 5);
	CHECK(cropped.Height == 5);
	CHECK(cropped.Pixels.size() == 100);
}
TEST_CASE("Aseprite reads authored tags and embedded tilemap", "[bake][aseprite]") {
	AsepriteDocument doc;
	std::string failure;
	REQUIRE(ReadAseprite(Bytes(ase_fixture::Tags), doc, failure));
	CHECK(doc.Frames.size() == 12);
	CHECK(doc.Tags.size() == 3);
	CHECK(doc.Layers.size() == 3);
	for (const auto &tag : doc.Tags) {
		CHECK(tag.First <= tag.Last);
		CHECK(tag.Last < doc.Frames.size());
	}
	REQUIRE(ReadAseprite(Bytes(ase_fixture::Tilemap), doc, failure));
	REQUIRE(doc.Tilesets.size() == 1);
	CHECK(doc.Width == 6);
	CHECK(doc.Height == 6);
	CHECK(doc.Tilesets[0].Count == 4);
	CHECK(doc.Layers[0].Type == 2);
	CHECK(doc.Tilesets[0].Width == 2);
	CHECK(doc.Tilesets[0].Height == 2);
	engine::assets::TextureData image;
	REQUIRE(RenderAseprite(doc, 0, {}, false, true, image, failure));
	CHECK(image.Width == 6);
	CHECK(image.Pixels.size() == 144);
}
TEST_CASE(
	"Aseprite malformed counts, truncated chunks and decoded budgets preserve the prior document",
	"[bake][aseprite]"
) {
	AsepriteDocument doc;
	doc.Width = 99;
	std::string failure;
	CHECK_FALSE(ReadAseprite(Bytes(ase_fixture::Point), doc, failure, 4));
	CHECK(doc.Width == 99);
	for (size_t n : {size_t(0), size_t(127), ase_fixture::Point.size() - 1}) {
		CHECK_FALSE(ReadAseprite(Bytes(ase_fixture::Point).first(n), doc, failure));
		CHECK(doc.Width == 99);
	}
	auto corrupt = ase_fixture::Point;
	corrupt[6] = 255;
	corrupt[7] = 255;
	CHECK_FALSE(ReadAseprite(Bytes(corrupt), doc, failure));
	CHECK(doc.Width == 99);
	corrupt = ase_fixture::Point;
	corrupt[128] = 255;
	corrupt[129] = 255;
	corrupt[130] = 255;
	corrupt[131] = 127;
	CHECK_FALSE(ReadAseprite(Bytes(corrupt), doc, failure));
	CHECK(doc.Width == 99);
}

TEST_CASE(
	"Aseprite inspection retains actual header frames chunks and compressed buffers", "[bake][aseprite]"
) {
	AsepriteDocument doc;
	std::string failure;
	REQUIRE(ReadAseprite(Bytes(ase_fixture::Point), doc, failure));
	const auto data = nlohmann::ordered_json::parse(doc.InspectionJson);
	CHECK(data["Width"] == 10);
	CHECK(data["Height"] == 10);
	CHECK(data["Color depth"] == 8);
	REQUIRE(data["Frames"].size() == 2);
	bool compressed = false, layer = false;
	for (const auto &frame : data["Frames"]) {
		CHECK(frame["Magic number"] == 0xf1fa);
		for (const auto &chunk : frame["Chunks"]) {
			if (chunk["Type"] == 0x2004) {
				layer = true;
				CHECK(chunk["Name"] == doc.Layers[0].Name);
			}
			if (chunk["Type"] == 0x2005 && chunk["Cel type"] == 2) {
				compressed = true;
				CHECK_FALSE(chunk["Buffer"]["$buffer_hex"].get<std::string>().empty());
				CHECK(chunk["Width"] == 5);
			}
		}
	}
	CHECK(compressed);
	CHECK(layer);
	REQUIRE(ReadAseprite(Bytes(ase_fixture::Tags), doc, failure));
	const auto tags = nlohmann::ordered_json::parse(doc.InspectionJson);
	bool foundTags = false;
	for (const auto &chunk : tags["Frames"][0]["Chunks"])
		if (chunk["Type"] == 0x2018) {
			foundTags = true;
			CHECK(chunk["Tags"].size() == 3);
			CHECK(chunk["Tags"][0]["Name"] == doc.Tags[0].Name);
		}
	CHECK(foundTags);
}

TEST_CASE("Aseprite raw cel byte counts are exact before pixel inspection", "[bake][aseprite]") {
	AsepriteDocument doc;
	std::string failure;
	const auto valid = RawCel(1);
	REQUIRE(ReadAseprite(valid, doc, failure));
	CHECK(doc.Frames[0].Cels[0].Pixels.Pixels.size() == 4);
	for (size_t bytes : {size_t{0}, size_t{2}, size_t{3}, size_t{5}}) {
		const auto malformed = RawCel(bytes);
		doc.Width = 99;
		doc.InspectionJson = "retained";
		CHECK_FALSE(ReadAseprite(malformed, doc, failure));
		CHECK(doc.Width == 99);
		CHECK(doc.InspectionJson == "retained");
	}
}
TEST_CASE("Aseprite chunk headers and payloads stay within their declared frame", "[bake][aseprite]") {
	const auto source = Bytes(ase_fixture::Point);
	std::vector<std::byte> original(source.begin(), source.end());
	const size_t frameEnd = 128 + Little(original, 128, 4);
	size_t last = 144;
	while (last + Little(original, last, 4) < frameEnd)
		last += Little(original, last, 4);
	for (size_t headerBytes : {size_t{0}, size_t{3}, size_t{5}, size_t{6}}) {
		auto malformed = original;
		Store(malformed, 128, last - 128 + headerBytes, 4);
		AsepriteDocument doc;
		doc.Width = 99;
		std::string failure;
		CHECK_FALSE(ReadAseprite(malformed, doc, failure));
		CHECK(doc.Width == 99);
	}
}
