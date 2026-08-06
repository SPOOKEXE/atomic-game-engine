#include "Decoders.hpp"

#include <engine/bake/Image.hpp>

#include <algorithm>
#include <array>
#include <cstdint>

namespace engine::bake {

	namespace {
		constexpr std::array<uint8_t, 8> PNG_SIGNATURE{0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A};

		bool StartsWith(std::span<const std::byte> bytes, std::span<const uint8_t> prefix) {
			if (bytes.size() < prefix.size()) {
				return false;
			}
			for (size_t index = 0; index < prefix.size(); index++) {
				if (static_cast<uint8_t>(bytes[index]) != prefix[index]) {
					return false;
				}
			}
			return true;
		}
	}

	ImageFormat ImageFormatOfBytes(std::span<const std::byte> bytes) {
		if (StartsWith(bytes, PNG_SIGNATURE)) {
			return ImageFormat::Png;
		}
		if (bytes.size() >= 2 && static_cast<char>(bytes[0]) == 'B' && static_cast<char>(bytes[1]) == 'M') {
			return ImageFormat::Bmp;
		}

		// The start-of-image marker. Two bytes rather than the longer JFIF or
		// Exif signature further in, because a bare JPEG carries neither and
		// refusing those would refuse most of what a renderer pipeline emits.
		if (bytes.size() >= 3 && static_cast<uint8_t>(bytes[0]) == 0xFF &&
			static_cast<uint8_t>(bytes[1]) == 0xD8 && static_cast<uint8_t>(bytes[2]) == 0xFF) {
			return ImageFormat::Jpeg;
		}
		return ImageFormat::Unknown;
	}

	std::string_view Describe(ImageFormat format) {
		switch (format) {
		case ImageFormat::Png:
			return "png";
		case ImageFormat::Bmp:
			return "bmp";
		case ImageFormat::Jpeg:
			return "jpeg";
		case ImageFormat::Unknown:
			break;
		}
		return "unknown";
	}

	bool ReadImage(std::span<const std::byte> bytes, assets::TextureData &out, std::string &failure) {
		switch (ImageFormatOfBytes(bytes)) {
		case ImageFormat::Png:
			return ReadPng(bytes, out, failure);
		case ImageFormat::Bmp:
			return ReadBmp(bytes, out, failure);
		case ImageFormat::Jpeg:
			return ReadJpeg(bytes, out, failure);
		case ImageFormat::Unknown:
			break;
		}
		failure = "image: not a format this reads";
		return false;
	}

	bool ResizeImage(
		const assets::TextureData &source, uint32_t width, uint32_t height, assets::TextureData &out
	) {
		if (!source.IsValid() || width == 0 || height == 0) {
			return false;
		}
		if (width > assets::Texture::MAXIMUM_DIMENSION || height > assets::Texture::MAXIMUM_DIMENSION) {
			return false;
		}

		const uint32_t channels = assets::BytesPerPixel(source.Format);

		assets::TextureData resized;
		resized.Width = width;
		resized.Height = height;
		resized.Format = source.Format;
		resized.Pixels.resize(static_cast<size_t>(width) * height * channels);

		for (uint32_t row = 0; row < height; row++) {
			// The source rows this destination row averages over. Computed as a
			// half-open range from the exact edges, so every source row belongs
			// to exactly one destination row and none is counted twice — which
			// is what stops a downscale from brightening or darkening the image
			// along one edge.
			const uint32_t firstRow =
				static_cast<uint32_t>(static_cast<uint64_t>(row) * source.Height / height);
			const uint32_t lastRow = std::max(
				firstRow + 1, static_cast<uint32_t>(static_cast<uint64_t>(row + 1) * source.Height / height)
			);

			for (uint32_t column = 0; column < width; column++) {
				const uint32_t firstColumn =
					static_cast<uint32_t>(static_cast<uint64_t>(column) * source.Width / width);
				const uint32_t lastColumn = std::max(
					firstColumn + 1,
					static_cast<uint32_t>(static_cast<uint64_t>(column + 1) * source.Width / width)
				);

				for (uint32_t channel = 0; channel < channels; channel++) {
					uint32_t total = 0;
					uint32_t count = 0;

					for (uint32_t sourceRow = firstRow; sourceRow < lastRow && sourceRow < source.Height;
						 sourceRow++) {
						for (uint32_t sourceColumn = firstColumn;
							 sourceColumn < lastColumn && sourceColumn < source.Width;
							 sourceColumn++) {
							const size_t offset =
								(static_cast<size_t>(sourceRow) * source.Width + sourceColumn) * channels +
								channel;
							total += static_cast<uint8_t>(source.Pixels[offset]);
							count++;
						}
					}

					const size_t destination =
						(static_cast<size_t>(row) * width + column) * channels + channel;
					resized.Pixels[destination] = static_cast<std::byte>(count == 0 ? 0 : total / count);
				}
			}
		}

		out = std::move(resized);
		return true;
	}
}
