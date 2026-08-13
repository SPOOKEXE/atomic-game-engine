#include "Decoders.hpp"
#include "Extension.hpp"

#include <engine/bake/Image.hpp>

#include <algorithm>
#include <array>
#include <bit>
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
		// than clamped is `ResizeImage`'s — a texture silently smaller than what
		// was asked for is a bake that looks like it worked.
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

		// **Carried across, because a resize does not change what the cells
		// are.** A flipbook sheet shrunk to fit a texture budget is the same
		// animation at a lower resolution — same grid, same frame count, same
		// rate — and dropping the three fields here would turn every imported
		// GIF larger than `--max-texture` back into an anonymous atlas. That is
		// the whole failure this note exists to prevent: it would look like the
		// decoder was broken, and the decoder would be fine.
		//
		// **A non-uniform resize is still legal and still carried.** The grid is
		// a count, not a pixel size, so cells stay cells whatever the aspect
		// becomes — anything sampling the sheet divides by `FlipbookSide` rather
		// than by a stored cell width.
		resized.FlipbookSide = source.FlipbookSide;
		resized.FlipbookFrames = source.FlipbookFrames;
		resized.FlipbookFrameRate = source.FlipbookFrameRate;

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

	uint32_t MipChainLevels(const assets::TextureData &image) {
		if (!image.IsValid()) {
			return 0;
		}

		const uint32_t full = assets::MipLevelCount(image.Width, image.Height);
		if (!image.IsFlipbook()) {
			return full;
		}

		// **A one-cell sheet has no interior boundary to bleed across**, and that
		// is every still GIF: `Gif.cpp` gives a single frame a 1x1 grid, so
		// treating it as a grid here would cost every imported still its chain
		// for a neighbour that does not exist.
		const uint32_t side = image.FlipbookSide;
		if (side == 1) {
			return full;
		}

		// A grid the sheet's dimensions do not divide evenly has no smaller sheet
		// holding the same cells, so it gets no chain rather than an approximate
		// one — `BuildMipChain` carries the argument.
		if (image.Width % side != 0 || image.Height % side != 0) {
			return 1;
		}

		// **The last level whose cells are still an exact halving**, which is the
		// largest power of two dividing both cell dimensions — and which lands
		// exactly on the level where a frame is one pixel when the cells are
		// themselves a power of two. One level further and a destination pixel
		// spans two frames.
		const uint32_t cellWidth = image.Width / side;
		const uint32_t cellHeight = image.Height / side;
		const uint32_t halvings =
			static_cast<uint32_t>(std::min(std::countr_zero(cellWidth), std::countr_zero(cellHeight)));
		return std::min(full, halvings + 1);
	}

	bool BuildMipChain(assets::TextureData &image) {
		const uint32_t levels = MipChainLevels(image);
		if (levels == 0) {
			return false;
		}

		// Cleared rather than appended to, so a graph run twice bakes the same
		// bytes both times.
		image.Mips.clear();
		image.Mips.reserve(levels - 1u);

		assets::TextureData previous = image;

		for (uint32_t level = 1; level < levels; level++) {
			assets::TextureData next;
			if (!ResizeImage(
					previous,
					assets::MipExtent(image.Width, level),
					assets::MipExtent(image.Height, level),
					next
				)) {
				return false;
			}
			image.Mips.push_back(next.Pixels);
			previous = std::move(next);
		}
		return true;
	}
}
