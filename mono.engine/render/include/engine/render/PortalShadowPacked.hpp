#pragma once

#include <engine/render/PortalShadowImage.hpp>

namespace engine::render {
	inline constexpr size_t PORTAL_SHADOW_PACKED_BLOCK_SAMPLES = 64;
	inline constexpr size_t PORTAL_SHADOW_PACKED_BLOCK_COUNT =
		PORTAL_SHADOW_BYTES / 4 / PORTAL_SHADOW_PACKED_BLOCK_SAMPLES;
	inline constexpr size_t PORTAL_SHADOW_PACKED_DESCRIPTOR_WORDS = PORTAL_SHADOW_PACKED_BLOCK_COUNT * 2;
	inline constexpr uint32_t PORTAL_SHADOW_PACKED_OFFSET_MASK = (1u << 26) - 1;
	inline constexpr size_t PORTAL_SHADOW_PACKED_MAX_BYTES =
		PORTAL_SHADOW_BYTES + PORTAL_SHADOW_PACKED_DESCRIPTOR_WORDS * 4;

	// Descriptor pairs precede payload: minimum sample bits, then absolute word
	// offset in low 26 bits and delta width in high 6 bits. Each block packs 64
	// consecutive row-major samples LSB-first into 2 * width words. Constants
	// have width zero and no payload. There is no trailing sentinel word.
	// Output changes only on success; byteBudget bounds the new output allocation.
	bool PackPortalShadow(
		std::span<const std::byte> depth, size_t byteBudget, std::vector<uint32_t> &words, std::string &error
	);
	// Checks the full layout and samples without allocating decoded storage.
	bool ValidPortalShadowPacked(std::span<const uint32_t> words);
	// Reconstructs canonical little-endian D32 bytes after complete validation.
	bool UnpackPortalShadow(
		std::span<const uint32_t> words, size_t byteBudget, std::vector<std::byte> &depth, std::string &error
	);
}
