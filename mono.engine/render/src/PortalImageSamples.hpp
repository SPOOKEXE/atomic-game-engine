#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace engine::render {

	// Wire words are little endian and need not be aligned for a native load.
	inline uint32_t PortalSampleWord(const std::byte *bytes) {
		return std::to_integer<uint32_t>(bytes[0]) | (std::to_integer<uint32_t>(bytes[1]) << 8) |
			   (std::to_integer<uint32_t>(bytes[2]) << 16) | (std::to_integer<uint32_t>(bytes[3]) << 24);
	}

	// Positive finite binary32 values, including positive zero. Negative zero
	// is not a valid depth sentinel.
	inline bool ValidPortalDepthSamples(std::span<const std::byte> samples) {
		if (samples.size() % 4 != 0) return false;
		bool valid = true;
		for (size_t offset = 0; offset < samples.size(); offset += 4)
			valid &= PortalSampleWord(samples.data() + offset) < 0x7f800000u;
		return valid;
	}

	// Baseline RGBA admits every finite binary32 value, including either zero.
	inline bool ValidPortalBaselineSamples(std::span<const std::byte> samples) {
		if (samples.size() % 16 != 0) return false;
		bool valid = true;
		for (size_t offset = 0; offset < samples.size(); offset += 4)
			valid &= (PortalSampleWord(samples.data() + offset) & 0x7fffffffu) < 0x7f800000u;
		return valid;
	}

	// Response RGB is finite and nonnegative; alpha additionally cannot exceed
	// one. Numeric nonnegativity admits negative zero in all four channels.
	inline bool ValidPortalResponseSamples(std::span<const std::byte> samples) {
		if (samples.size() % 16 != 0) return false;
		bool valid = true;
		for (size_t offset = 0; offset < samples.size(); offset += 16) {
			for (size_t channel = 0; channel < 3; ++channel) {
				const uint32_t word = PortalSampleWord(samples.data() + offset + channel * 4);
				valid &= (word < 0x7f800000u) | (word == 0x80000000u);
			}
			const uint32_t alpha = PortalSampleWord(samples.data() + offset + 12);
			valid &= (alpha <= 0x3f800000u) | (alpha == 0x80000000u);
		}
		return valid;
	}
}
