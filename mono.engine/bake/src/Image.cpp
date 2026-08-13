#include "Decoders.hpp"
#include "Extension.hpp"

#include <engine/bake/Image.hpp>

#include <array>
#include <cstdint>
#include <string>

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

		// `GIF87a` or `GIF89a`. Four bytes rather than six, because the version
		// changes nothing this decoder does — see `Gif.cpp`.
		if (bytes.size() >= 4 && static_cast<char>(bytes[0]) == 'G' && static_cast<char>(bytes[1]) == 'I' &&
			static_cast<char>(bytes[2]) == 'F' && static_cast<char>(bytes[3]) == '8') {
			return ImageFormat::Gif;
		}
		return ImageFormat::Unknown;
	}

	ImageFormat ImageFormatOfName(std::string_view name) {
		const std::string extension = ExtensionOf(name);
		if (extension == "png") {
			return ImageFormat::Png;
		}
		if (extension == "bmp") {
			return ImageFormat::Bmp;
		}
		if (extension == "jpg" || extension == "jpeg") {
			return ImageFormat::Jpeg;
		}
		if (extension == "gif") {
			return ImageFormat::Gif;
		}
		if (extension == "svg") {
			return ImageFormat::Svg;
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
		case ImageFormat::Gif:
			return "gif";
		case ImageFormat::Svg:
			return "svg";
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
		case ImageFormat::Gif:
			return ReadGif(bytes, out, failure);
		case ImageFormat::Svg:
			// Unreachable through the sniff above, which has no signature to
			// find, and here because the day one is added this has to say
			// something rather than fall through to "not a format this reads".
		case ImageFormat::Unknown:
			break;
		}
		failure = "image: not a format this reads";
		return false;
	}

	bool RasterizeSvg(
		std::span<const std::byte> bytes,
		uint32_t width,
		uint32_t height,
		assets::TextureData &out,
		std::string &failure
	) {
		// **The caller's target is checked here and the document's own size is
		// checked in `Svg.cpp`.** They are different numbers from different
		// people: this one is a pipeline's, and the reason it is refused rather
		// than clamped is `assets::ResizeImage`'s — a texture silently smaller
		// than what was asked for is a bake that looks like it worked.
		if ((width == 0) != (height == 0)) {
			failure = "svg: a raster target of " + std::to_string(width) + "x" + std::to_string(height) +
					  " — give both axes, or neither for the size the document declares";
			return false;
		}
		if (width > assets::Texture::MAXIMUM_DIMENSION || height > assets::Texture::MAXIMUM_DIMENSION) {
			failure = "svg: a raster target past " + std::to_string(assets::Texture::MAXIMUM_DIMENSION) +
					  " pixels on an axis";
			return false;
		}
		return ReadSvg(bytes, width, height, out, failure);
	}
}
