#pragma once

// Owned numeric surfaces. Device formats and texture handles remain in render.

#include <array>
#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

namespace engine::imagegraph {
	enum class SurfaceFormat : uint8_t {
		RGBA8Unorm,
		RGBA4Unorm,
		RGBA16Float,
		RGBA32Float,
		R8Unorm,
		R16Float,
		R32Float
	};
	struct SurfaceFormatInfo {
		std::string_view Name;
		uint8_t Channels, BitsPerChannel, BytesPerPixel;
		bool FloatingPoint;
	};
	std::optional<SurfaceFormatInfo> DescribeSurfaceFormat(SurfaceFormat format) noexcept;
	// Maps a concrete source Color Depth choice (2..8), not a device enum.
	std::optional<SurfaceFormat> SourceSurfaceFormat(int64_t choice) noexcept;
	struct SurfaceLayout {
		uint64_t RowBytes, Bytes;
	};
	// Rejects empty, unknown, overflowing or over-budget layouts before storage allocation.
	std::optional<SurfaceLayout> CheckedSurfaceLayout(
		uint32_t width, uint32_t height, SurfaceFormat format, uint64_t maximumBytes
	) noexcept;
	struct Image {
		uint32_t Width = 0, Height = 0;
		// Tightly packed owned bytes. Float words are little endian; RGBA4 uses R in its low nibble.
		std::vector<uint8_t> Pixels;
		uint64_t Hash = 0;
		SurfaceFormat Format = SurfaceFormat::RGBA8Unorm;
		bool operator==(const Image &) const = default;
	};
	bool ValidSurfaceLayout(const Image &image, uint32_t maximumDimension, uint64_t maximumBytes) noexcept;
	bool FiniteSurfaceSamples(const Image &image) noexcept;
	// Binary16 conversion rounds binary32 to nearest, ties even and preserves signed zero.
	uint16_t EncodeHalf(float value) noexcept;
	float DecodeHalf(uint16_t bits) noexcept;
	using SurfacePixel = std::array<double, 4>;
	// Native single-channel sampling returns (R,0,0,1); source device swizzle remains a parity gate.
	bool LoadSurfacePixel(const Image &image, uint32_t x, uint32_t y, SurfacePixel &pixel) noexcept;
	// Normalized formats clamp; floating formats retain finite signed/HDR samples without clamping.
	// Refuses nonfinite values or floating overflow atomically before modifying the pixel.
	bool StoreSurfacePixel(Image &image, uint32_t x, uint32_t y, const SurfacePixel &pixel) noexcept;
	// RGBA8 retains its existing byte hash. Other formats include their named format and dimensions.
	uint64_t SurfaceHash(const Image &image) noexcept;
}
