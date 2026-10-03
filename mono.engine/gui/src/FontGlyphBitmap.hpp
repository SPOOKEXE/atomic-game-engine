#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace engine::gui::detail {
	struct FontGlyphBitmapView {
		std::span<const uint8_t> Storage;
		uint32_t Width = 0;
		uint32_t Height = 0;
		int32_t Pitch = 0;
		uint16_t GrayLevels = 256;
		bool Monochrome = false;
	};

	// FreeType stores negative-pitch bitmaps bottom-up. Publish tightly packed top-down coverage.
	inline bool CopyFontGlyphBitmap(const FontGlyphBitmapView &bitmap, std::span<uint8_t> output) {
		const uint64_t pixels = uint64_t(bitmap.Width) * bitmap.Height;
		if (pixels != output.size()) return false;
		if (!pixels) return true;
		const uint64_t pitch = bitmap.Pitch < 0 ? uint64_t(-int64_t(bitmap.Pitch)) : uint64_t(bitmap.Pitch);
		const uint64_t rowBytes = bitmap.Monochrome ? (uint64_t(bitmap.Width) + 7) / 8 : bitmap.Width;
		if (pitch < rowBytes || pitch * bitmap.Height > bitmap.Storage.size() ||
			(!bitmap.Monochrome && (bitmap.GrayLevels < 2 || bitmap.GrayLevels > 256)))
			return false;
		if (!bitmap.Monochrome)
			for (uint32_t y = 0; y < bitmap.Height; ++y)
				for (uint32_t x = 0; x < bitmap.Width; ++x)
					if (bitmap.Storage[size_t(y) * pitch + x] >= bitmap.GrayLevels) return false;
		for (uint32_t y = 0; y < bitmap.Height; ++y) {
			const size_t sourceY = bitmap.Pitch < 0 ? bitmap.Height - 1 - y : y;
			const auto row = bitmap.Storage.subspan(sourceY * pitch, static_cast<size_t>(rowBytes));
			for (uint32_t x = 0; x < bitmap.Width; ++x) {
				const uint32_t coverage =
					bitmap.Monochrome
						? ((row[x / 8] & (0x80 >> (x % 8))) ? 255 : 0)
						: (uint32_t(row[x]) * 255 + (bitmap.GrayLevels - 1) / 2) / (bitmap.GrayLevels - 1);
				output[size_t(y) * bitmap.Width + x] = static_cast<uint8_t>(coverage);
			}
		}
		return true;
	}
}
