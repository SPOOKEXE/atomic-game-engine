#include "IconExport.hpp"

#include <engine/imagegraph/Document.hpp>

#include <algorithm>
#include <utility>

namespace engine::imagegraphexport::runner {
	namespace {
		constexpr uint32_t MAXIMUM_ICON_DIMENSION = 256;
		constexpr uint32_t DIRECTORY_BYTES = 22;
		constexpr uint32_t BITMAP_HEADER_BYTES = 40;
		void Little(std::vector<uint8_t> &bytes, uint32_t value, unsigned count) {
			for (unsigned index = 0; index < count; ++index)
				bytes.push_back(static_cast<uint8_t>(value >> (index * 8)));
		}
	}

	bool
	EncodeIcon(const engine::imagegraph::Image &image, std::vector<uint8_t> &bytes, std::string &failure) {
		using namespace engine::imagegraph;
		bytes.clear();
		failure.clear();
		if (!ValidSurfaceLayout(image, MAXIMUM_ICON_DIMENSION, Limits::MaximumOutputBytes) ||
			!FiniteSurfaceSamples(image)) {
			failure = "ICO requires a finite valid image with dimensions from 1 through 256";
			return false;
		}
		const uint32_t colourBytes = image.Width * image.Height * 4;
		const uint32_t maskStride = ((image.Width + 31) / 32) * 4;
		const uint32_t maskBytes = maskStride * image.Height;
		const uint32_t payloadBytes = BITMAP_HEADER_BYTES + colourBytes + maskBytes;
		std::vector<uint8_t> encoded;
		encoded.reserve(DIRECTORY_BYTES + payloadBytes);
		Little(encoded, 0, 2);
		Little(encoded, 1, 2);
		Little(encoded, 1, 2);
		// ICO directory stores 256 as zero; the DIB header retains the full dimensions.
		encoded.push_back(static_cast<uint8_t>(image.Width));
		encoded.push_back(static_cast<uint8_t>(image.Height));
		Little(encoded, 0, 2);
		Little(encoded, 1, 2);
		Little(encoded, 32, 2);
		Little(encoded, payloadBytes, 4);
		Little(encoded, DIRECTORY_BYTES, 4);
		Little(encoded, BITMAP_HEADER_BYTES, 4);
		Little(encoded, image.Width, 4);
		// DIB height covers both the colour bitmap and the monochrome mask.
		Little(encoded, image.Height * 2, 4);
		Little(encoded, 1, 2);
		Little(encoded, 32, 2);
		Little(encoded, 0, 4);
		Little(encoded, colourBytes + maskBytes, 4);
		for (unsigned field = 0; field < 4; ++field)
			Little(encoded, 0, 4);
		const size_t colourStart = encoded.size();
		for (uint32_t row = 0; row < image.Height; ++row) {
			for (uint32_t x = 0; x < image.Width; ++x) {
				SurfacePixel pixel{};
				if (!LoadSurfacePixel(image, x, image.Height - row - 1, pixel)) {
					failure = "ICO could not read a validated surface pixel";
					return false;
				}
				for (unsigned channel : {2u, 1u, 0u, 3u})
					encoded.push_back(static_cast<uint8_t>(std::clamp(pixel[channel], 0.0, 1.0) * 255));
			}
		}
		const size_t maskStart = encoded.size();
		encoded.resize(maskStart + maskBytes, 0);
		for (uint32_t row = 0; row < image.Height; ++row)
			for (uint32_t x = 0; x < image.Width; ++x)
				if (encoded[colourStart + (size_t(row) * image.Width + x) * 4 + 3] == 0)
					encoded[maskStart + size_t(row) * maskStride + x / 8] |=
						static_cast<uint8_t>(0x80u >> (x % 8));
		bytes = std::move(encoded);
		return true;
	}
}
