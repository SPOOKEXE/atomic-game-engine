#pragma once

// Native single-image ICO profile. Publication belongs to the caller's staged export path.

#include <engine/imagegraph/Surface.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace engine::imagegraphexport::runner {
	// Encodes 1..256 pixel dimensions with straight BGRA and a transparent-pixel AND mask.
	// Failure clears bytes. Float samples are clamped and truncated to normalized eight-bit channels.
	bool
	EncodeIcon(const engine::imagegraph::Image &image, std::vector<uint8_t> &bytes, std::string &failure);
}
