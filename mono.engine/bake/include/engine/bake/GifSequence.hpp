#pragma once

#include <engine/assets/TextureSequence.hpp>

#include <cstddef>
#include <span>
#include <string>

namespace engine::bake {
	// Decodes every composited GIF frame in source order, preserving each
	// normalised hundredth-second delay. Failure leaves `out` unchanged.
	bool
	ReadGifSequence(std::span<const std::byte> bytes, assets::TextureSequenceData &out, std::string &failure);
}
