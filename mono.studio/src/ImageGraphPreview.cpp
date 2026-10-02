#include "ImageGraphPreview.hpp"

#include <engine/assets/TexturePixel.hpp>

#include <array>
#include <cstring>
#include <new>
#include <span>
#include <stdexcept>
#include <studio/ImageGraph.hpp>

namespace studio::detail {
	bool PrepareImageGraphPreviewRgba8(
		const engine::imagegraph::Image &image, std::vector<std::byte> &rgba8
	) noexcept {
		if (!engine::imagegraph::ValidSurfaceLayout(
				image, IMAGE_COMPOSER_PREVIEW_MAXIMUM_DIMENSION, IMAGE_COMPOSER_PREVIEW_SURFACE_MAXIMUM_BYTES
			) ||
			!engine::imagegraph::FiniteSurfaceSamples(image))
			return false;
		const auto byteCount = engine::imagegraph::CheckedSurfaceLayout(
			image.Width,
			image.Height,
			engine::imagegraph::SurfaceFormat::RGBA8Unorm,
			IMAGE_COMPOSER_PREVIEW_DISPLAY_MAXIMUM_BYTES
		);
		if (!byteCount || byteCount->Bytes > IMAGE_COMPOSER_PREVIEW_DISPLAY_MAXIMUM_BYTES) return false;

		try {
			std::vector<std::byte> candidate;
			candidate.reserve(static_cast<size_t>(byteCount->Bytes));
			if (candidate.capacity() > IMAGE_COMPOSER_PREVIEW_DISPLAY_MAXIMUM_BYTES) return false;
			candidate.resize(static_cast<size_t>(byteCount->Bytes));
			if (image.Format == engine::imagegraph::SurfaceFormat::RGBA8Unorm) {
				std::memcpy(candidate.data(), image.Pixels.data(), candidate.size());
			} else {
				const auto format = engine::imagegraph::DescribeSurfaceFormat(image.Format);
				if (!format) return false;
				for (uint32_t y = 0; y < image.Height; y++) {
					for (uint32_t x = 0; x < image.Width; x++) {
						engine::imagegraph::SurfacePixel pixel{};
						if (!engine::imagegraph::LoadSurfacePixel(image, x, y, pixel)) return false;
						if (format->Channels == 1) pixel[1] = pixel[2] = pixel[0];
						const size_t offset = (static_cast<size_t>(y) * image.Width + x) * 4;
						std::array<std::byte, 4> bytes{};
						if (!engine::assets::StoreTexturePixel(
								engine::assets::TextureFormat::RGBA8, pixel, bytes
							))
							return false;
						for (size_t channel = 0; channel < bytes.size(); channel++)
							candidate[offset + channel] = bytes[channel];
					}
				}
			}
			rgba8.swap(candidate);
			return true;
		} catch (const std::bad_alloc &) {
			return false;
		} catch (const std::length_error &) {
			return false;
		}
	}
} // namespace studio::detail
