#pragma once
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
namespace engine::imagegraphexport {
	bool WriteGraphHostFile(const std::filesystem::path &, std::span<const uint8_t>, std::string &);
}
