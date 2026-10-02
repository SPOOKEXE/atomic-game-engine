#pragma once

// @tier L0 · shared

#include <cstdint>

namespace engine::core {
	// Encodes IEEE binary32 as binary16, rounding to nearest with ties to even.
	// @param value The input value. Signed zero and infinities are preserved.
	// @return The IEEE binary16 bit pattern.
	// @since v0.26
	uint16_t EncodeFloat16(float value) noexcept;

	// Decodes an IEEE binary16 bit pattern as binary32.
	// @param bits The input bit pattern.
	// @return The decoded binary32 value.
	// @since v0.26
	float DecodeFloat16(uint16_t bits) noexcept;
} // namespace engine::core
