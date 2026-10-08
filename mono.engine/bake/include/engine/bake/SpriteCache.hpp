#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace engine::bake {
	enum class SpriteCacheLayout : uint8_t { Rgba8TopDown, Bgra8TopDown, Rgba8BottomUp, Bgra8BottomUp };
	enum class SpriteCacheShape : uint8_t { Sprite, Array };
	struct SpriteCacheFrame {
		uint32_t Width = 0, Height = 0;
		std::vector<uint8_t> Rgba;
		bool operator==(const SpriteCacheFrame &) const = default;
	};
	struct SpriteCacheLimits {
		static constexpr uint32_t MaximumFrames = 256;
		static constexpr uint32_t MaximumDimension = 16384;
		static constexpr uint64_t MaximumPixels = 16 * 1024 * 1024;
		static constexpr uint64_t MaximumEncodedBytes = 1024 * 1024;
	};
	// A missing source leaf keeps its index. Empty arrays are valid cached array values.
	struct SurfaceCacheItem {
		SpriteCacheFrame Surface;
		std::vector<SurfaceCacheItem> Elements;
		bool IsArray = false;
		bool operator==(const SurfaceCacheItem &) const = default;
	};
	struct SurfaceCacheLimits {
		static constexpr uint32_t MaximumItems = 4096;
		static constexpr uint32_t MaximumDepth = 64;
	};
	enum class SurfaceCacheFailure : uint8_t { None, Malformed, LimitExceeded };
	// Decode the source's sparse, nested surface array without renumbering frame slots.
	// Layout remains an explicit host observation; refusal preserves the previous tree.
	// Owned tree capacities share maximumBytes. Vendor JSON residency is separately
	// bounded by the encoded text, nesting and parse-time item limits.
	bool ReadSurfaceCache(
		std::string_view text,
		SpriteCacheLayout layout,
		std::vector<SurfaceCacheItem> &result,
		std::string &failure,
		uint64_t maximumBytes,
		SurfaceCacheFailure *failureKind = nullptr
	);
	std::string_view SpriteCacheLayoutName(SpriteCacheLayout layout);
	std::optional<SpriteCacheLayout> ParseSpriteCacheLayoutName(std::string_view name);

	// Fixed lowercase BLAKE3 identity of exact bounded source cache text, independent of parsing.
	std::optional<std::array<char, 64>> SpriteCacheDataHash(std::string_view text);

	// Source sprite or flat sprite-array cache, with an explicitly observed platform byte layout.
	// Native owned capacities, prior result and fixed codec workspace share maximumBytes.
	// Borrowed inputs and vendor JSON/codec residency remain separately bounded.
	// Malformed, unsupported and over-budget inputs preserve the previous result.
	bool ReadSpriteCache(
		std::string_view text,
		SpriteCacheLayout layout,
		std::vector<SpriteCacheFrame> &result,
		std::string &failure,
		uint64_t maximumBytes,
		SpriteCacheShape shape = SpriteCacheShape::Array
	);
	bool WriteSpriteCache(
		std::span<const SpriteCacheFrame> frames,
		SpriteCacheLayout layout,
		std::string &result,
		std::string &failure,
		uint64_t maximumBytes,
		SpriteCacheShape shape = SpriteCacheShape::Array
	);
}
