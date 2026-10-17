#include "Decoders.hpp"

#include <engine/assets/Texture.hpp>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <span>
#include <string>
#include <string_view>

namespace {
	constexpr uint32_t SEED_WIDTH = 64;
	constexpr uint32_t SEED_HEIGHT = 48;

	void Require(bool condition) {
		if (!condition) std::abort();
	}

	void WriteSeed(const std::filesystem::path &directory) {
		constexpr std::string_view seed = "<svg width=\"16\" height=\"12\" viewBox=\"0 0 16 12\">"
										  "<rect x=\"1\" y=\"1\" width=\"14\" height=\"10\" fill=\"#2c8\"/>"
										  "</svg>";
		std::ofstream file(directory / "bounded.svg", std::ios::binary);
		file.write(seed.data(), static_cast<std::streamsize>(seed.size()));
		Require(file.good());
	}
}

extern "C" int LLVMFuzzerInitialize(int *count, char ***arguments) {
	if (*count != 3 || std::string_view((*arguments)[1]) != "--write-seeds") return 0;
	const std::filesystem::path directory((*arguments)[2]);
	std::filesystem::create_directories(directory);
	WriteSeed(directory);
	std::exit(0);
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *input, size_t size) {
	using namespace engine;
	const auto bytes = std::as_bytes(std::span(input, size));
	assets::TextureData image;
	std::string failure;
	const bool read = bake::ReadSvg(bytes, SEED_WIDTH, SEED_HEIGHT, image, failure);
	if (!read) {
		Require(image.Pixels.empty());
		return 0;
	}
	Require(image.Width == SEED_WIDTH && image.Height == SEED_HEIGHT);
	Require(image.Pixels.size() == static_cast<size_t>(SEED_WIDTH) * SEED_HEIGHT * 4);
	return 0;
}
