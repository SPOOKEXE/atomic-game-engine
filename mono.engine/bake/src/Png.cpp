#include "Decoders.hpp"

#include <algorithm>
#include <array>
#include <cryptopp/crc.h>
#include <cryptopp/filters.h>
#include <cryptopp/zlib.h>
#include <cstdint>
#include <cstring>
#include <vector>

namespace engine::bake {

	namespace {
		constexpr std::array<uint8_t, 8> SIGNATURE{0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A};

		// The largest compressed stream this will inflate.
		//
		// **The bound is on the input rather than only on the output**, because
		// the output bound is derived from the header and the header is the
		// thing an attacker wrote. A hundred megabytes of IDAT is far past any
		// real texture and is the ceiling a zip bomb has to fit inside.
		constexpr size_t MAXIMUM_COMPRESSED_BYTES = 128u * 1024u * 1024u;

		// PNG colour types, as the specification numbers them.
		enum : uint8_t {
			GREY = 0,
			RGB = 2,
			PALETTE = 3,
			GREY_ALPHA = 4,
			RGBA = 6,
		};

		uint32_t ReadBigEndian32(std::span<const std::byte> bytes, size_t offset) {
			return (static_cast<uint32_t>(bytes[offset]) << 24) |
				   (static_cast<uint32_t>(bytes[offset + 1]) << 16) |
				   (static_cast<uint32_t>(bytes[offset + 2]) << 8) | static_cast<uint32_t>(bytes[offset + 3]);
		}

		// How many samples one pixel of a colour type carries.
		uint32_t ChannelsOf(uint8_t colourType) {
			switch (colourType) {
			case GREY:
				return 1;
			case RGB:
				return 3;
			case PALETTE:
				return 1;
			case GREY_ALPHA:
				return 2;
			case RGBA:
				return 4;
			default:
				return 0;
			}
		}

		// The Paeth predictor, out of the specification verbatim.
		//
		// Written with `int` rather than the byte type on purpose: the three
		// differences are signed and the intermediate `p` can exceed a byte in
		// both directions, so doing this in `uint8_t` silently wraps and
		// produces an image that decodes without error and looks like static.
		uint8_t Paeth(int left, int above, int upperLeft) {
			const int estimate = left + above - upperLeft;
			const int fromLeft = std::abs(estimate - left);
			const int fromAbove = std::abs(estimate - above);
			const int fromUpperLeft = std::abs(estimate - upperLeft);

			if (fromLeft <= fromAbove && fromLeft <= fromUpperLeft) {
				return static_cast<uint8_t>(left);
			}
			return fromAbove <= fromUpperLeft ? static_cast<uint8_t>(above) : static_cast<uint8_t>(upperLeft);
		}

		// Reverses the per-scanline filter, in place, over the inflated stream.
		//
		// The stream is `height` rows of `1 + rowBytes`: one filter byte then
		// the row. Each filter refers to the byte `bytesPerPixel` to the left
		// and to the same position on the row above, both of which read as zero
		// where they fall outside the image.
		bool Unfilter(
			std::vector<uint8_t> &raw,
			uint32_t height,
			size_t rowBytes,
			size_t bytesPerPixel,
			std::string &failure
		) {
			const size_t stride = rowBytes + 1;

			for (uint32_t row = 0; row < height; row++) {
				const size_t start = row * stride;
				const uint8_t filter = raw[start];
				uint8_t *current = raw.data() + start + 1;

				// The row above, already unfiltered — which is why this runs
				// top to bottom and cannot be parallelised without changing the
				// algorithm.
				const uint8_t *above = row == 0 ? nullptr : raw.data() + (row - 1) * stride + 1;

				for (size_t index = 0; index < rowBytes; index++) {
					const int left = index >= bytesPerPixel ? current[index - bytesPerPixel] : 0;
					const int upper = above != nullptr ? above[index] : 0;
					const int upperLeft =
						(above != nullptr && index >= bytesPerPixel) ? above[index - bytesPerPixel] : 0;

					switch (filter) {
					case 0:
						break;
					case 1:
						current[index] = static_cast<uint8_t>(current[index] + left);
						break;
					case 2:
						current[index] = static_cast<uint8_t>(current[index] + upper);
						break;
					case 3:
						current[index] = static_cast<uint8_t>(current[index] + (left + upper) / 2);
						break;
					case 4:
						current[index] = static_cast<uint8_t>(current[index] + Paeth(left, upper, upperLeft));
						break;
					default:
						failure = "png: unknown scanline filter";
						return false;
					}
				}
			}
			return true;
		}

		// Inflates the concatenated IDAT stream to exactly `expected` bytes.
		//
		// **Exactly, and that is the check.** A zlib stream says nothing about
		// how much it will produce, so the only defence against a bomb is the
		// size the *header* implies — which is bounded because the dimensions
		// were bounded before this ran. A stream producing more or less than
		// its own header implies is malformed whichever direction it errs in.
		bool Inflate(
			const std::vector<uint8_t> &compressed,
			size_t expected,
			std::vector<uint8_t> &out,
			std::string &failure
		) {
			try {
				out.clear();
				out.reserve(expected);

				CryptoPP::ZlibDecompressor decompressor(new CryptoPP::VectorSink(out));
				decompressor.Put(compressed.data(), compressed.size());
				decompressor.MessageEnd();
			} catch (const CryptoPP::Exception &error) {
				// Caught rather than allowed to escape because a malformed
				// deflate stream is an ordinary property of an input file, and
				// the caller is a bake tool that has to name the file and move
				// on to the next one.
				failure = std::string("png: ") + error.what();
				return false;
			}

			if (out.size() != expected) {
				failure = "png: inflated size disagrees with the header";
				return false;
			}
			return true;
		}
	}

	bool ReadPng(std::span<const std::byte> bytes, assets::TextureData &out, std::string &failure) {
		if (bytes.size() < SIGNATURE.size()) {
			failure = "png: shorter than the signature";
			return false;
		}
		for (size_t index = 0; index < SIGNATURE.size(); index++) {
			if (static_cast<uint8_t>(bytes[index]) != SIGNATURE[index]) {
				failure = "png: wrong signature";
				return false;
			}
		}

		uint32_t width = 0;
		uint32_t height = 0;
		uint8_t depth = 0;
		uint8_t colourType = 0;
		bool haveHeader = false;

		std::vector<uint8_t> palette;
		std::vector<uint8_t> paletteAlpha;
		std::vector<uint8_t> compressed;

		size_t offset = SIGNATURE.size();
		while (offset + 12 <= bytes.size()) {
			const uint32_t length = ReadBigEndian32(bytes, offset);

			// Checked against what is left before the chunk is looked at, so a
			// length of four billion costs a comparison.
			if (static_cast<uint64_t>(length) + 12 > static_cast<uint64_t>(bytes.size() - offset)) {
				failure = "png: chunk runs past the end of the file";
				return false;
			}

			const std::span<const std::byte> type = bytes.subspan(offset + 4, 4);
			const std::span<const std::byte> data = bytes.subspan(offset + 8, length);

			// The CRC covers the type and the data. Checked because this runs
			// over files that came off somebody's disk before any content hash
			// existed to cover them — a truncated download is otherwise a
			// picture of static rather than a refusal.
			//
			// **Compared as numbers rather than as bytes, and that is not
			// style.** Crypto++ writes a CRC-32 digest in host order and PNG
			// stores it big-endian, so `CRC32::Verify` against the four bytes in
			// the file disagrees with itself on every little-endian machine —
			// which is every machine this builds on. Assembling both sides into
			// a `uint32_t` is what makes the check about the checksum rather
			// than about the byte order.
			CryptoPP::CRC32 crc;
			crc.Update(reinterpret_cast<const CryptoPP::byte *>(type.data()), type.size());
			crc.Update(reinterpret_cast<const CryptoPP::byte *>(data.data()), data.size());

			CryptoPP::byte digest[4] = {};
			crc.Final(digest);
			const uint32_t computed =
				static_cast<uint32_t>(digest[0]) | (static_cast<uint32_t>(digest[1]) << 8) |
				(static_cast<uint32_t>(digest[2]) << 16) | (static_cast<uint32_t>(digest[3]) << 24);

			if (computed != ReadBigEndian32(bytes, offset + 8 + length)) {
				failure = "png: chunk checksum does not match";
				return false;
			}

			const auto is = [&type](const char *name) {
				for (size_t index = 0; index < 4; index++) {
					if (static_cast<char>(type[index]) != name[index]) {
						return false;
					}
				}
				return true;
			};

			if (is("IHDR")) {
				if (length != 13) {
					failure = "png: malformed header chunk";
					return false;
				}
				width = ReadBigEndian32(data, 0);
				height = ReadBigEndian32(data, 4);
				depth = static_cast<uint8_t>(data[8]);
				colourType = static_cast<uint8_t>(data[9]);

				if (static_cast<uint8_t>(data[10]) != 0 || static_cast<uint8_t>(data[11]) != 0) {
					failure = "png: unknown compression or filter method";
					return false;
				}
				if (static_cast<uint8_t>(data[12]) != 0) {
					// Adam7 is a different unfilter over seven sub-images.
					// Refused by name rather than half-read, because a
					// progressive PNG decoded as a flat one produces a picture
					// that is recognisably the right image and wrong
					// everywhere, which is the worst kind of silent failure.
					failure = "png: interlaced images are not supported";
					return false;
				}
				haveHeader = true;
			} else if (is("PLTE")) {
				palette.assign(
					reinterpret_cast<const uint8_t *>(data.data()),
					reinterpret_cast<const uint8_t *>(data.data()) + data.size()
				);
			} else if (is("tRNS")) {
				paletteAlpha.assign(
					reinterpret_cast<const uint8_t *>(data.data()),
					reinterpret_cast<const uint8_t *>(data.data()) + data.size()
				);
			} else if (is("IDAT")) {
				if (compressed.size() + length > MAXIMUM_COMPRESSED_BYTES) {
					failure = "png: compressed stream is implausibly large";
					return false;
				}
				compressed.insert(
					compressed.end(),
					reinterpret_cast<const uint8_t *>(data.data()),
					reinterpret_cast<const uint8_t *>(data.data()) + data.size()
				);
			} else if (is("IEND")) {
				break;
			}

			offset += 12 + static_cast<size_t>(length);
		}

		if (!haveHeader) {
			failure = "png: no header chunk";
			return false;
		}
		if (width == 0 || height == 0 || width > assets::Texture::MAXIMUM_DIMENSION ||
			height > assets::Texture::MAXIMUM_DIMENSION) {
			failure = "png: dimensions are zero or past the ceiling";
			return false;
		}
		if (depth != 8 && depth != 16) {
			// Sub-byte depths pack several pixels into one byte, which is a
			// different unfilter stride and a different expansion. Refused
			// rather than approximated.
			failure = "png: only 8 and 16 bits a channel are supported";
			return false;
		}

		const uint32_t channels = ChannelsOf(colourType);
		if (channels == 0) {
			failure = "png: unknown colour type";
			return false;
		}
		if (colourType == PALETTE) {
			if (depth != 8) {
				failure = "png: only 8-bit palettes are supported";
				return false;
			}
			if (palette.empty() || palette.size() % 3 != 0) {
				failure = "png: palette is missing or malformed";
				return false;
			}
		}
		if (compressed.empty()) {
			failure = "png: no image data";
			return false;
		}

		const size_t bytesPerSample = depth / 8;
		const size_t bytesPerPixel = channels * bytesPerSample;
		const size_t rowBytes = static_cast<size_t>(width) * bytesPerPixel;
		const size_t expected = (rowBytes + 1) * static_cast<size_t>(height);

		std::vector<uint8_t> raw;
		if (!Inflate(compressed, expected, raw, failure)) {
			return false;
		}
		if (!Unfilter(raw, height, rowBytes, bytesPerPixel, failure)) {
			return false;
		}

		assets::TextureData decoded;
		decoded.Width = width;
		decoded.Height = height;
		decoded.Format = assets::TextureFormat::RGBA8;
		decoded.Pixels.resize(static_cast<size_t>(width) * height * 4);

		for (uint32_t row = 0; row < height; row++) {
			const uint8_t *source = raw.data() + row * (rowBytes + 1) + 1;
			std::byte *destination = decoded.Pixels.data() + static_cast<size_t>(row) * width * 4;

			for (uint32_t column = 0; column < width; column++) {
				const uint8_t *pixel = source + static_cast<size_t>(column) * bytesPerPixel;

				// Sixteen-bit samples are truncated to their high byte rather
				// than rounded. The engine's texture format has no 16-bit
				// layout, and dithering a colour ramp at bake time would be a
				// decision about *appearance* taken by a decoder.
				const auto sample = [&](size_t channel) { return pixel[channel * bytesPerSample]; };

				uint8_t red = 0, green = 0, blue = 0, alpha = 255;
				switch (colourType) {
				case GREY:
					red = green = blue = sample(0);
					break;
				case RGB:
					red = sample(0);
					green = sample(1);
					blue = sample(2);
					break;
				case PALETTE: {
					const size_t entry = sample(0);
					if (entry * 3 + 2 >= palette.size()) {
						failure = "png: palette index past the end of the palette";
						return false;
					}
					red = palette[entry * 3];
					green = palette[entry * 3 + 1];
					blue = palette[entry * 3 + 2];
					alpha = entry < paletteAlpha.size() ? paletteAlpha[entry] : 255;
					break;
				}
				case GREY_ALPHA:
					red = green = blue = sample(0);
					alpha = sample(1);
					break;
				case RGBA:
					red = sample(0);
					green = sample(1);
					blue = sample(2);
					alpha = sample(3);
					break;
				default:
					failure = "png: unknown colour type";
					return false;
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
