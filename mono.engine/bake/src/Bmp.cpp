#include "Decoders.hpp"

#include <cstdint>
#include <cstring>
#include <vector>

namespace engine::bake {

	namespace {
		// The bitmap file header is fourteen bytes and every DIB header this
		// reads is at least forty.
		constexpr size_t FILE_HEADER_BYTES = 14;
		constexpr uint32_t MINIMUM_INFO_HEADER_BYTES = 40;

		// BI_RGB and BI_BITFIELDS. Run-length and JPEG-in-BMP are refused: they
		// are separate decoders wearing this container, and a bake tool that
		// half-read one would produce a plausible wrong image.
		constexpr uint32_t COMPRESSION_NONE = 0;
		constexpr uint32_t COMPRESSION_BITFIELDS = 3;

		uint16_t ReadLittleEndian16(std::span<const std::byte> bytes, size_t offset) {
			return static_cast<uint16_t>(
				static_cast<uint16_t>(bytes[offset]) | (static_cast<uint16_t>(bytes[offset + 1]) << 8)
			);
		}

		uint32_t ReadLittleEndian32(std::span<const std::byte> bytes, size_t offset) {
			return static_cast<uint32_t>(bytes[offset]) | (static_cast<uint32_t>(bytes[offset + 1]) << 8) |
				   (static_cast<uint32_t>(bytes[offset + 2]) << 16) |
				   (static_cast<uint32_t>(bytes[offset + 3]) << 24);
		}
	}

	bool ReadBmp(std::span<const std::byte> bytes, assets::TextureData &out, std::string &failure) {
		if (bytes.size() < FILE_HEADER_BYTES + MINIMUM_INFO_HEADER_BYTES) {
			failure = "bmp: shorter than a header";
			return false;
		}
		if (static_cast<char>(bytes[0]) != 'B' || static_cast<char>(bytes[1]) != 'M') {
			failure = "bmp: wrong signature";
			return false;
		}

		const uint32_t pixelOffset = ReadLittleEndian32(bytes, 10);
		const uint32_t headerBytes = ReadLittleEndian32(bytes, FILE_HEADER_BYTES);
		if (headerBytes < MINIMUM_INFO_HEADER_BYTES ||
			static_cast<uint64_t>(FILE_HEADER_BYTES) + headerBytes > bytes.size()) {
			failure = "bmp: unsupported or truncated info header";
			return false;
		}

		const int32_t signedWidth = static_cast<int32_t>(ReadLittleEndian32(bytes, FILE_HEADER_BYTES + 4));
		const int32_t signedHeight = static_cast<int32_t>(ReadLittleEndian32(bytes, FILE_HEADER_BYTES + 8));
		const uint16_t bitCount = ReadLittleEndian16(bytes, FILE_HEADER_BYTES + 14);
		const uint32_t compression = ReadLittleEndian32(bytes, FILE_HEADER_BYTES + 16);

		if (compression != COMPRESSION_NONE && compression != COMPRESSION_BITFIELDS) {
			failure = "bmp: only uncompressed bitmaps are supported";
			return false;
		}
		if (bitCount != 8 && bitCount != 24 && bitCount != 32) {
			failure = "bmp: only 8, 24 and 32 bits a pixel are supported";
			return false;
		}

		// **A negative height means top-down**, and it is the one field of this
		// format that is signed. Reading it unsigned turns a top-down image
		// into a four-billion-row one, which is a bounds check away from an
		// allocation nobody asked for.
		const bool topDown = signedHeight < 0;
		if (signedWidth <= 0 || signedHeight == 0) {
			failure = "bmp: dimensions are zero or negative";
			return false;
		}

		const uint32_t width = static_cast<uint32_t>(signedWidth);
		const uint32_t height = topDown ? static_cast<uint32_t>(-static_cast<int64_t>(signedHeight))
										: static_cast<uint32_t>(signedHeight);

		if (width > assets::Texture::MAXIMUM_DIMENSION || height > assets::Texture::MAXIMUM_DIMENSION) {
			failure = "bmp: dimensions past the ceiling";
			return false;
		}

		// The palette sits between the info header and the pixels. Its size is
		// derived from where the pixels start rather than from the header's
		// own colour count, because the two disagree in real files and the
		// offset is the one the pixels are actually at.
		std::span<const std::byte> palette;
		if (bitCount == 8) {
			const size_t paletteStart = FILE_HEADER_BYTES + headerBytes;
			if (pixelOffset < paletteStart || pixelOffset > bytes.size()) {
				failure = "bmp: palette runs past the pixels";
				return false;
			}
			palette = bytes.subspan(paletteStart, pixelOffset - paletteStart);
			if (palette.size() < 4) {
				failure = "bmp: an 8-bit bitmap with no palette";
				return false;
			}
		}

		// Rows are padded to a four-byte boundary, and the padding is part of
		// the stride rather than of the row.
		const size_t bytesPerPixel = bitCount / 8;
		const size_t rowBytes = static_cast<size_t>(width) * bytesPerPixel;
		const size_t stride = (rowBytes + 3) & ~size_t{3};

		if (pixelOffset > bytes.size()) {
			failure = "bmp: pixel offset past the end of the file";
			return false;
		}
		if (stride * static_cast<uint64_t>(height) > static_cast<uint64_t>(bytes.size() - pixelOffset)) {
			failure = "bmp: pixels run past the end of the file";
			return false;
		}

		assets::TextureData decoded;
		decoded.Width = width;
		decoded.Height = height;
		decoded.Format = assets::TextureFormat::RGBA8;
		decoded.Pixels.resize(static_cast<size_t>(width) * height * 4);

		// **A 32-bit BI_RGB bitmap's fourth byte is officially unused**, and
		// most writers leave it zero. Taken literally that makes every such
		// file fully transparent, which is the single most common way a BMP
		// loader produces a blank screen. So the alpha channel is only believed
		// when some pixel actually uses it.
		bool anyAlpha = false;
		if (bitCount == 32) {
			for (uint32_t row = 0; row < height && !anyAlpha; row++) {
				const std::byte *source = bytes.data() + pixelOffset + static_cast<size_t>(row) * stride;
				for (uint32_t column = 0; column < width; column++) {
					if (static_cast<uint8_t>(source[column * 4 + 3]) != 0) {
						anyAlpha = true;
						break;
					}
				}
			}
		}

		for (uint32_t row = 0; row < height; row++) {
			const uint32_t sourceRow = topDown ? row : height - 1 - row;
			const std::byte *source = bytes.data() + pixelOffset + static_cast<size_t>(sourceRow) * stride;
			std::byte *destination = decoded.Pixels.data() + static_cast<size_t>(row) * width * 4;

			for (uint32_t column = 0; column < width; column++) {
				uint8_t red = 0, green = 0, blue = 0, alpha = 255;

				if (bitCount == 8) {
					const size_t entry = static_cast<uint8_t>(source[column]);
					if (entry * 4 + 2 >= palette.size()) {
						failure = "bmp: palette index past the end of the palette";
						return false;
					}
					// Palette entries are stored blue, green, red, reserved.
					blue = static_cast<uint8_t>(palette[entry * 4]);
					green = static_cast<uint8_t>(palette[entry * 4 + 1]);
					red = static_cast<uint8_t>(palette[entry * 4 + 2]);
				} else {
					const std::byte *pixel = source + static_cast<size_t>(column) * bytesPerPixel;
					blue = static_cast<uint8_t>(pixel[0]);
					green = static_cast<uint8_t>(pixel[1]);
					red = static_cast<uint8_t>(pixel[2]);
					if (bitCount == 32 && anyAlpha) {
						alpha = static_cast<uint8_t>(pixel[3]);
					}
				}

				destination[column * 4] = static_cast<std::byte>(red);
				destination[column * 4 + 1] = static_cast<std::byte>(green);
				destination[column * 4 + 2] = static_cast<std::byte>(blue);
				destination[column * 4 + 3] = static_cast<std::byte>(alpha);
			}
		}

		out = std::move(decoded);
		return true;
	}
}
