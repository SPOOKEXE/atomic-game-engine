#pragma once
#include <engine/imagecodec/Image.hpp>

namespace engine::imagecodec {
	bool ValidateExtent(uint32_t width, uint32_t height, const Limits &limits, std::string &failure);
	bool DecodePng(std::span<const std::byte> bytes, Image &out, std::string &failure, const Limits &limits);
	bool DecodeJpeg(std::span<const std::byte> bytes, Image &out, std::string &failure, const Limits &limits);
}
