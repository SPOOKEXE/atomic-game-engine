#include "IconExport.hpp"

#include "fixtures/IconTwoByTwo.hpp"

#include <engine/imagegraph/Surface.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

TEST_SUITE_ID("engine.imagegraphexport.icon_export")

namespace {
	using namespace engine::imagegraph;
	uint32_t Little(const std::vector<uint8_t> &bytes, size_t offset, unsigned count) {
		uint32_t value = 0;
		for (unsigned index = 0; index < count; ++index)
			value |= uint32_t(bytes.at(offset + index)) << (index * 8);
		return value;
	}
}

TEST_CASE("ICO writes bottom-up BGRA with alpha and a padded transparency mask") {
	Image image;
	image.Width = image.Height = 2;
	image.Pixels = {255, 0, 0, 255, 0, 255, 0, 128, 0, 0, 255, 0, 255, 255, 255, 255};
	std::vector<uint8_t> bytes;
	std::string failure;
	REQUIRE(engine::imagegraphexport::runner::EncodeIcon(image, bytes, failure));
	REQUIRE(bytes.size() == 86);
	CHECK(bytes == std::vector<uint8_t>(ICON_TWO_BY_TWO.begin(), ICON_TWO_BY_TWO.end()));
	CHECK(Little(bytes, 0, 2) == 0);
	CHECK(Little(bytes, 2, 2) == 1);
	CHECK(Little(bytes, 4, 2) == 1);
	CHECK(bytes[6] == 2);
	CHECK(bytes[7] == 2);
	CHECK(Little(bytes, 12, 2) == 32);
	CHECK(Little(bytes, 14, 4) == 64);
	CHECK(Little(bytes, 18, 4) == 22);
	CHECK(Little(bytes, 22, 4) == 40);
	CHECK(Little(bytes, 26, 4) == 2);
	CHECK(Little(bytes, 30, 4) == 4);
	CHECK(Little(bytes, 42, 4) == 24);
	const std::vector<uint8_t> expectedPixels = {255, 0,   0, 0,   255, 255, 255, 255, 0, 0, 255, 255,
												 0,	  255, 0, 128, 128, 0,	 0,	  0,   0, 0, 0,	  0};
	CHECK(std::vector<uint8_t>(bytes.begin() + 62, bytes.end()) == expectedPixels);
	CHECK(failure.empty());
}

TEST_CASE("ICO directory represents 256 dimensions as zero and masks cross byte boundaries") {
	Image image;
	image.Width = 256;
	image.Height = 1;
	image.Pixels.resize(1024, 255);
	image.Pixels[7 * 4 + 3] = 0;
	image.Pixels[8 * 4 + 3] = 0;
	image.Pixels[255 * 4 + 3] = 0;
	std::vector<uint8_t> bytes;
	std::string failure;
	REQUIRE(engine::imagegraphexport::runner::EncodeIcon(image, bytes, failure));
	CHECK(bytes[6] == 0);
	CHECK(bytes[7] == 1);
	CHECK(Little(bytes, 26, 4) == 256);
	CHECK(bytes.at(1086) == 1);
	CHECK(bytes.at(1087) == 128);
	CHECK(bytes.back() == 1);
}

TEST_CASE("ICO refuses oversized malformed and non-finite surfaces without leaving output") {
	Image image;
	image.Width = image.Height = 1;
	image.Pixels = {1, 2, 3, 255};
	std::vector<uint8_t> bytes = {123};
	std::string failure;
	image.Width = 257;
	CHECK_FALSE(engine::imagegraphexport::runner::EncodeIcon(image, bytes, failure));
	CHECK(bytes.empty());
	CHECK_FALSE(failure.empty());
	image.Width = 1;
	image.Pixels.pop_back();
	CHECK_FALSE(engine::imagegraphexport::runner::EncodeIcon(image, bytes, failure));
	CHECK(bytes.empty());
	image.Width = 0;
	CHECK_FALSE(engine::imagegraphexport::runner::EncodeIcon(image, bytes, failure));
	image.Width = 1;
	image.Format = SurfaceFormat::RGBA32Float;
	image.Pixels.resize(16);
	REQUIRE(StoreSurfacePixel(image, 0, 0, {2, -1, 0.5, 1}));
	REQUIRE(engine::imagegraphexport::runner::EncodeIcon(image, bytes, failure));
	CHECK(bytes.at(62) == 127);
	CHECK(bytes.at(63) == 0);
	CHECK(bytes.at(64) == 255);
	CHECK(bytes.at(65) == 255);
	// Bypass the surface writer's finite guard to model malformed decoded bytes.
	image.Pixels[0] = image.Pixels[1] = 0;
	image.Pixels[2] = 128;
	image.Pixels[3] = 127;
	CHECK_FALSE(engine::imagegraphexport::runner::EncodeIcon(image, bytes, failure));
	CHECK(bytes.empty());
}
