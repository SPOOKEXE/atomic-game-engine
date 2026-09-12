#include "PortalImageSamples.hpp"

#include <engine/core/Profiling.hpp>
#include <engine/render/PortalShadowPacked.hpp>

#include <algorithm>
#include <bit>
#include <limits>

namespace engine::render {
	namespace {
		constexpr uint32_t MAX_DEPTH_BITS = 0x3f800000u;
		static_assert(PORTAL_SHADOW_PACKED_MAX_BYTES / 4 <= PORTAL_SHADOW_PACKED_OFFSET_MASK);
		static_assert(PORTAL_SHADOW_PACKED_BLOCK_SAMPLES == 64);

		bool Fail(std::string &error, const char *message) {
			error = message;
			return false;
		}
		struct BlockRange {
			uint32_t Minimum = std::numeric_limits<uint32_t>::max(), Maximum = 0;
		};
		BlockRange RangeOf(const std::byte *samples) {
			BlockRange range;
			for (size_t sample = 0; sample < PORTAL_SHADOW_PACKED_BLOCK_SAMPLES; ++sample) {
				const auto word = PortalSampleWord(samples + sample * 4);
				range.Minimum = std::min(range.Minimum, word);
				range.Maximum = std::max(range.Maximum, word);
			}
			return range;
		}
		uint32_t DeltaAt(std::span<const uint32_t> words, size_t offset, uint32_t width, size_t sample) {
			if (!width) return 0;
			const size_t bit = sample * width;
			const size_t index = offset + bit / 32;
			const uint32_t shift = bit % 32;
			uint32_t delta = words[index] >> shift;
			if (shift + width > 32) delta |= words[index + 1] << (32 - shift);
			return width == 32 ? delta : delta & ((1u << width) - 1);
		}
	}

	bool PackPortalShadow(
		std::span<const std::byte> depth, size_t byteBudget, std::vector<uint32_t> &words, std::string &error
	) {
		ENGINE_PROFILE("pack portal shadow");
		if (depth.size() != PORTAL_SHADOW_BYTES) return Fail(error, "invalid shadow depth size");
		size_t wordCount = PORTAL_SHADOW_PACKED_DESCRIPTOR_WORDS;
		for (size_t block = 0; block < PORTAL_SHADOW_PACKED_BLOCK_COUNT; ++block) {
			const auto range = RangeOf(depth.data() + block * PORTAL_SHADOW_PACKED_BLOCK_SAMPLES * 4);
			if (range.Maximum > MAX_DEPTH_BITS) return Fail(error, "invalid shadow depth sample");
			wordCount += 2 * static_cast<size_t>(std::bit_width(range.Maximum - range.Minimum));
		}
		if (wordCount > byteBudget / 4) return Fail(error, "packed shadow exceeds byte budget");
		std::vector<uint32_t> packed(wordCount, 0);
		size_t offset = PORTAL_SHADOW_PACKED_DESCRIPTOR_WORDS;
		for (size_t block = 0; block < PORTAL_SHADOW_PACKED_BLOCK_COUNT; ++block) {
			const auto *samples = depth.data() + block * PORTAL_SHADOW_PACKED_BLOCK_SAMPLES * 4;
			const auto range = RangeOf(samples);
			const uint32_t width = std::bit_width(range.Maximum - range.Minimum);
			packed[block * 2] = range.Minimum;
			packed[block * 2 + 1] = static_cast<uint32_t>(offset) | (width << 26);
			if (!width) continue;
			for (size_t sample = 0; sample < PORTAL_SHADOW_PACKED_BLOCK_SAMPLES; ++sample) {
				const uint32_t delta = PortalSampleWord(samples + sample * 4) - range.Minimum;
				const size_t bit = sample * width;
				const size_t index = offset + bit / 32;
				const uint32_t shift = bit % 32;
				packed[index] |= delta << shift;
				if (shift + width > 32) packed[index + 1] |= delta >> (32 - shift);
			}
			offset += 2 * width;
		}
		words = std::move(packed);
		error.clear();
		return true;
	}

	bool ValidPortalShadowPacked(std::span<const uint32_t> words) {
		if (words.size() < PORTAL_SHADOW_PACKED_DESCRIPTOR_WORDS ||
			words.size() > PORTAL_SHADOW_PACKED_MAX_BYTES / 4)
			return false;
		size_t offset = PORTAL_SHADOW_PACKED_DESCRIPTOR_WORDS;
		for (size_t block = 0; block < PORTAL_SHADOW_PACKED_BLOCK_COUNT; ++block) {
			const uint32_t base = words[block * 2];
			const uint32_t descriptor = words[block * 2 + 1];
			const uint32_t width = descriptor >> 26;
			if (base > MAX_DEPTH_BITS || width > 32 ||
				(descriptor & PORTAL_SHADOW_PACKED_OFFSET_MASK) != offset ||
				2 * width > words.size() - offset)
				return false;
			uint32_t minimum = std::numeric_limits<uint32_t>::max(), maximum = 0;
			for (size_t sample = 0; sample < PORTAL_SHADOW_PACKED_BLOCK_SAMPLES; ++sample) {
				const uint32_t delta = DeltaAt(words, offset, width, sample);
				if (delta > MAX_DEPTH_BITS - base) return false;
				minimum = std::min(minimum, delta);
				maximum = std::max(maximum, delta);
			}
			if (minimum != 0 || static_cast<uint32_t>(std::bit_width(maximum)) != width) return false;
			offset += 2 * width;
		}
		return offset == words.size();
	}

	bool UnpackPortalShadow(
		std::span<const uint32_t> words, size_t byteBudget, std::vector<std::byte> &depth, std::string &error
	) {
		ENGINE_PROFILE("unpack portal shadow");
		if (byteBudget < PORTAL_SHADOW_BYTES) return Fail(error, "shadow depth exceeds byte budget");
		if (!ValidPortalShadowPacked(words)) return Fail(error, "invalid packed shadow");
		std::vector<std::byte> decoded(PORTAL_SHADOW_BYTES);
		for (size_t block = 0; block < PORTAL_SHADOW_PACKED_BLOCK_COUNT; ++block) {
			const uint32_t base = words[block * 2];
			const uint32_t descriptor = words[block * 2 + 1];
			const size_t offset = descriptor & PORTAL_SHADOW_PACKED_OFFSET_MASK;
			const uint32_t width = descriptor >> 26;
			for (size_t sample = 0; sample < PORTAL_SHADOW_PACKED_BLOCK_SAMPLES; ++sample) {
				const uint32_t word = base + DeltaAt(words, offset, width, sample);
				const size_t at = (block * PORTAL_SHADOW_PACKED_BLOCK_SAMPLES + sample) * 4;
				for (size_t byte = 0; byte < 4; ++byte)
					decoded[at + byte] = std::byte((word >> (byte * 8)) & 0xff);
			}
		}
		depth = std::move(decoded);
		error.clear();
		return true;
	}
}
