#include "../tests/Images.hpp"

#include <engine/imagecodec/Image.hpp>
#include <engine/testing/Bench.hpp>

#include <stdexcept>

TEST_SUITE_ID("engine.imagecodec.bench.image")

namespace {
	std::string Base64(bool jpeg) {
		const auto bytes = jpeg ? fixtures::GreyJpeg(1920, 1080) : fixtures::GreyPng(1920, 1080);
		std::string encoded, failure;
		if (!engine::imagecodec::EncodeBase64(bytes, encoded, failure)) throw std::runtime_error(failure);
		return encoded;
	}
	void Import(std::string_view encoded, engine::imagecodec::Format format) {
		std::vector<std::byte> bytes;
		engine::imagecodec::Image image;
		std::string failure;
		if (!engine::imagecodec::DecodeBase64(encoded, bytes, failure) ||
			!engine::imagecodec::Decode(bytes, format, image, failure))
			throw std::runtime_error(failure);
		engine::testing::Consume(image.Pixels.data());
	}
}

BENCH("base64 + PNG, 1920x1080 flat grey, encoded sRGB RGBA8", 1) {
	static const std::string encoded = Base64(false);
	Import(encoded, engine::imagecodec::Format::Png);
}
BENCH("base64 + JPEG, 1920x1080 flat grey, encoded sRGB RGBA8", 1) {
	static const std::string encoded = Base64(true);
	Import(encoded, engine::imagecodec::Format::Jpeg);
}
BENCH("base64 + raw RGBA8, 1920x1080", 1) {
	static const std::string encoded = [] {
		std::vector<std::byte> pixels(8294400, std::byte{128});
		std::string text, failure;
		if (!engine::imagecodec::EncodeBase64(pixels, text, failure)) throw std::runtime_error(failure);
		return text;
	}();
	std::vector<std::byte> bytes;
	engine::imagecodec::Image image;
	std::string failure;
	if (!engine::imagecodec::DecodeBase64(encoded, bytes, failure) ||
		!engine::imagecodec::DecodeRaw(1920, 1080, bytes, image, failure))
		throw std::runtime_error(failure);
	engine::testing::Consume(image.Pixels.data());
}
