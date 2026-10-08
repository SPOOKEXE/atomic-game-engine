#include "Decoders.hpp"

#include <engine/assets/TexturePixel.hpp>
#include <engine/bake/Image.hpp>
#include <engine/bake/ImageGraph.hpp>
#include <engine/core/Bytes.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <new>
#include <utility>

namespace engine::bake {
	namespace {
		uint32_t Unsigned(std::span<const std::byte> bytes, size_t at, bool bigEndian, size_t count = 4) {
			uint32_t value = 0;
			for (size_t index = 0; index < count; ++index) {
				const size_t offset = bigEndian ? index : count - 1 - index;
				value = (value << 8) | std::to_integer<uint8_t>(bytes[at + offset]);
			}
			return value;
		}
		bool Dimensions(uint32_t width, uint32_t height, std::string &failure) {
			if (width == 0 || height == 0 || width > imagegraph::Limits::MaximumDimension ||
				height > imagegraph::Limits::MaximumDimension ||
				uint64_t(width) * height * 4 > imagegraph::Limits::MaximumImageBytes) {
				failure = "image graph source dimensions exceed the image budget";
				return false;
			}
			return true;
		}
		// Inspect the decoder's dimension carriers before it allocates decoded pixels.
		// Full format validation remains with the existing decoder, including CRCs.
		bool RasterDimensions(std::span<const std::byte> bytes, ImageFormat format, std::string &failure) {
			switch (format) {
			case ImageFormat::Png: {
				size_t offset = 8;
				bool found = false;
				while (bytes.size() - std::min(offset, bytes.size()) >= 12) {
					const uint32_t length = Unsigned(bytes, offset, true);
					if (length > bytes.size() - offset - 12) break;
					const bool header =
						bytes[offset + 4] == std::byte{'I'} && bytes[offset + 5] == std::byte{'H'} &&
						bytes[offset + 6] == std::byte{'D'} && bytes[offset + 7] == std::byte{'R'};
					if (header) {
						if (length != 13) break;
						if (!Dimensions(
								Unsigned(bytes, offset + 8, true), Unsigned(bytes, offset + 12, true), failure
							))
							return false;
						found = true;
					}
					offset += size_t(length) + 12;
					if (offset == bytes.size() && found) return true;
				}
				break;
			}
			case ImageFormat::Bmp:
				if (bytes.size() >= 26) {
					const uint32_t storedHeight = Unsigned(bytes, 22, false);
					const uint32_t height = (storedHeight & 0x80000000u) ? 0u - storedHeight : storedHeight;
					return Dimensions(Unsigned(bytes, 18, false), height, failure);
				}
				break;
			case ImageFormat::Gif:
				if (bytes.size() >= 10)
					return Dimensions(Unsigned(bytes, 6, false, 2), Unsigned(bytes, 8, false, 2), failure);
				break;
			case ImageFormat::Jpeg: {
				size_t offset = 2;
				bool found = false;
				while (offset < bytes.size()) {
					if (bytes[offset++] != std::byte{0xff}) break;
					while (offset < bytes.size() && bytes[offset] == std::byte{0xff})
						++offset;
					if (offset == bytes.size()) break;
					const uint8_t marker = std::to_integer<uint8_t>(bytes[offset++]);
					if (marker == 0xda || marker == 0xd9) {
						if (found) return true;
						break;
					}
					if (marker == 0x01 || (marker >= 0xd0 && marker <= 0xd7)) continue;
					if (bytes.size() - offset < 2) break;
					const uint32_t length = Unsigned(bytes, offset, true, 2);
					if (length < 2 || length > bytes.size() - offset) break;
					if (marker == 0xc0) {
						if (length < 8) break;
						if (!Dimensions(
								Unsigned(bytes, offset + 5, true, 2),
								Unsigned(bytes, offset + 3, true, 2),
								failure
							))
							return false;
						found = true;
					}
					offset += length;
				}
				break;
			}
			case ImageFormat::Unknown:
			case ImageFormat::Svg:
				failure = "image graph source is not a supported static raster";
				return false;
			}
			failure = "image graph source has no valid bounded raster dimensions";
			return false;
		}
	}

	bool DecodeImageGraphSource(
		std::string_view name,
		std::span<const std::byte> encoded,
		imagegraph::Image &out,
		std::string &failure
	) {
		(void)name;
		if (encoded.empty() || encoded.size() > imagegraph::Limits::MaximumImageBytes) {
			failure = "image graph encoded source exceeds the image budget or is empty";
			return false;
		}
		try {
			assets::TextureData decoded;
			if (encoded.size() >= 4 && Unsigned(encoded, 0, false) == assets::Texture::MAGIC) {
				if (encoded.size() < 15) {
					failure = "image graph source is a truncated texture asset";
					return false;
				}
				if (!Dimensions(Unsigned(encoded, 7, false), Unsigned(encoded, 11, false), failure))
					return false;
				core::ByteReader reader(encoded);
				if (!assets::Texture::Read(reader, decoded) || !reader.AtEnd()) {
					failure = "image graph source is a malformed texture asset";
					return false;
				}
			} else {
				const auto format = ImageFormatOfBytes(encoded);
				if (!RasterDimensions(encoded, format, failure)) return false;
				const bool imported = format == ImageFormat::Gif
										  ? ReadGifBounded(
												encoded,
												imagegraph::Limits::MaximumDimension,
												imagegraph::Limits::MaximumImageBytes / 4,
												decoded,
												failure
											)
										  : ReadImage(encoded, decoded, failure);
				if (!imported) return false;
			}
			if (!decoded.IsValid() || !Dimensions(decoded.Width, decoded.Height, failure)) return false;
			imagegraph::Image converted;
			converted.Width = decoded.Width;
			converted.Height = decoded.Height;
			if (decoded.Format == assets::TextureFormat::RGBA8) {
				converted.Pixels = std::move(decoded.Pixels);
			} else {
				converted.Pixels.resize(uint64_t(decoded.Width) * decoded.Height * 4);
				const size_t stride = assets::BytesPerPixel(decoded.Format);
				for (size_t index = 0; index < converted.Pixels.size() / 4; ++index) {
					std::array<float, 4> display{};
					if (!assets::LoadTexturePixelForDisplay(
							decoded.Format, std::span(decoded.Pixels).subspan(index * stride, stride), display
						)) {
						failure = "image graph source contains an invalid display pixel";
						return false;
					}
					assets::TexturePixel pixel{display[0], display[1], display[2], display[3]};
					// The graph stores encoded sRGB, while numeric texture formats sample linearly.
					if (!assets::IsSRGB(decoded.Format)) {
						for (size_t channel = 0; channel < 3; ++channel) {
							const double linear = pixel[channel];
							pixel[channel] = linear <= 0.0031308
												 ? 12.92 * linear
												 : 1.055 * std::pow(linear, 1.0 / 2.4) - 0.055;
						}
					}
					if (!assets::StoreTexturePixel(
							assets::TextureFormat::RGBA8,
							pixel,
							std::span(converted.Pixels).subspan(index * 4, 4)
						)) {
						failure = "image graph source pixel conversion failed";
						return false;
					}
				}
			}
			out = std::move(converted);
			return true;
		} catch (const std::bad_alloc &) {
			failure = "image graph source allocation failed";
			return false;
		}
	}

	bool ImageGraphTexture(const imagegraph::Image &image, assets::TextureData &out, std::string &failure) {
		if (!image.IsValid()) {
			failure = "image graph result is not a valid bounded image";
			return false;
		}
		try {
			assets::TextureData converted;
			converted.Width = image.Width;
			converted.Height = image.Height;
			converted.Format = assets::TextureFormat::RGBA8;
			converted.Pixels = image.Pixels;
			out = std::move(converted);
			return true;
		} catch (const std::bad_alloc &) {
			failure = "image graph texture allocation failed";
			return false;
		}
	}

	bool BakeImageGraph(
		const imagegraph::Document &document,
		std::string_view output,
		const imagegraph::SourceResolver &sources,
		assets::TextureData &out,
		imagegraph::Diagnostic &diagnostic
	) {
		imagegraph::Plan plan;
		imagegraph::Image image;
		if (!imagegraph::Compile(document, plan, diagnostic) ||
			!imagegraph::Evaluate(document, plan, output, sources, image, diagnostic))
			return false;
		assets::TextureData converted;
		converted.Width = image.Width;
		converted.Height = image.Height;
		converted.Format = assets::TextureFormat::RGBA8;
		converted.Pixels = std::move(image.Pixels);
		out = std::move(converted);
		return true;
	}
}
