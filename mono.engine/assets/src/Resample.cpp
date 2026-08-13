#include <engine/assets/Resample.hpp>

#include <algorithm>
#include <bit>
#include <cstddef>
#include <utility>

namespace engine::assets {

	bool ResizeImage(const TextureData &source, uint32_t width, uint32_t height, TextureData &out) {
		if (!source.IsValid() || width == 0 || height == 0) {
			return false;
		}
		if (width > Texture::MAXIMUM_DIMENSION || height > Texture::MAXIMUM_DIMENSION) {
			return false;
		}

		const uint32_t channels = BytesPerPixel(source.Format);

		TextureData resized;
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

	uint32_t MipChainLevels(const TextureData &image) {
		if (!image.IsValid()) {
			return 0;
		}

		const uint32_t full = MipLevelCount(image.Width, image.Height);
		if (!image.IsFlipbook()) {
			return full;
		}

		// **A one-cell sheet has no interior boundary to bleed across**, and that
		// is every still GIF: `bake/Gif.cpp` gives a single frame a 1x1 grid, so
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

	bool BuildMipChain(TextureData &image) {
		const uint32_t levels = MipChainLevels(image);
		if (levels == 0) {
			return false;
		}

		// Cleared rather than appended to, so a graph run twice bakes the same
		// bytes both times.
		image.Mips.clear();
		image.Mips.reserve(levels - 1u);

		TextureData previous = image;

		for (uint32_t level = 1; level < levels; level++) {
			TextureData next;
			if (!ResizeImage(previous, MipExtent(image.Width, level), MipExtent(image.Height, level), next)) {
				return false;
			}
			image.Mips.push_back(next.Pixels);
			previous = std::move(next);
		}
		return true;
	}
}
