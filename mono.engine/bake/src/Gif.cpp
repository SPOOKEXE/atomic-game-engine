// GIF87a/GIF89a into one flipbook sheet.
//
// **The output is a grid and not a list of frames, and that is the whole design
// decision.** `ROADMAP.md` v0.10 asks for GIF support and for flipbook particle
// animation in the same version, and they are the same feature seen from two
// ends: a GIF is a short looping animation, and the thing this engine can already
// *draw* animated is a flipbook - `effects::FlipbookLayout`, a square
// power-of-two grid sampled by cell. So a decoded GIF is laid out as that grid
// and becomes an ordinary texture, which every path in the engine already
// handles.
//
// The alternative was an animated texture type: a `TextureData` per frame, a
// player, a clock, and a second thing for the renderer to bind per draw. That is
// a real feature and it is not this one - and it would leave a GIF unable to be a
// particle, which is the case that actually asked for it.
//
// **What that costs is stated rather than hidden.** A grid is square and a power
// of two on each side, so a 12-frame GIF lands in a 4x4 with four cells unused
// and a 70-frame GIF is truncated to 64. Both are reported through `failure`
// being left alone and the frame count being what it is; a caller that cares
// reads the sheet's dimensions.
//
// ## What is supported
//
// Everything a GIF that came out of a real encoder uses: LZW with the clear and
// end codes, interlacing, local and global palettes, transparency, and the three
// disposal methods that matter. **Not** the plain-text extension, which nothing
// has emitted since about 1995.

#include "Decoders.hpp"

#include <engine/core/Log.hpp>

#include <algorithm>
#include <cstring>
#include <vector>

namespace engine::bake {

	namespace {
		// The largest sheet this will build, on a side.
		//
		// **8x8 cells, matching `effects::FlipbookLayout`'s widest.** A GIF with
		// more frames than that is truncated rather than refused: a 100-frame
		// animation played as its first 64 is a shorter animation, and refusing it
		// would turn a usable asset into a failed import.
		constexpr uint32_t MAX_SIDE = 8;

		// What a delay of 0 or 1 is taken to mean, in hundredths of a second.
		//
		// **The de-facto rule and not the specification's.** GIF says zero is
		// "no delay"; every browser since Netscape has read 0 and 1 as 100ms,
		// so an encoder emitting zero expected 10fps rather than an infinite
		// frame rate - and reading it literally would divide by zero.
		constexpr uint16_t DEFAULT_HUNDREDTHS = 10;

		// A cursor over the file that cannot run off the end.
		//
		// **Every read is checked, which is the whole of what a decoder owes.** A
		// GIF is a length-prefixed format read from disk, and `assets::Wav`'s rule
		// applies unchanged: a length running past the end is a refusal and never
		// a clamp, because a clamp turns a truncated file into a shorter animation
		// that plays.
		struct Reader {
			std::span<const std::byte> Bytes;
			size_t At = 0;

			bool Has(size_t count) const {
				return At + count <= Bytes.size();
			}

			uint8_t Byte() {
				return At < Bytes.size() ? static_cast<uint8_t>(Bytes[At++]) : 0;
			}

			uint16_t Short() {
				const uint16_t low = Byte();
				return static_cast<uint16_t>(low | (Byte() << 8));
			}

			void Skip(size_t count) {
				At = std::min(At + count, Bytes.size());
			}
		};

		// One RGBA pixel.
		struct Pixel {
			uint8_t R = 0;
			uint8_t G = 0;
			uint8_t B = 0;
			uint8_t A = 0;
		};

		// Reads a colour table of `count` entries.
		bool ReadPalette(Reader &reader, uint32_t count, std::vector<Pixel> &out) {
			if (!reader.Has(static_cast<size_t>(count) * 3)) {
				return false;
			}
			out.resize(count);
			for (uint32_t index = 0; index < count; index++) {
				out[index].R = reader.Byte();
				out[index].G = reader.Byte();
				out[index].B = reader.Byte();
				out[index].A = 255;
			}
			return true;
		}

		// Concatenates the sub-blocks of one data stream.
		//
		// A GIF's image data and its extensions are both a chain of length-prefixed
		// blocks terminated by a zero length, which is why one function serves
		// both.
		bool ReadBlocks(Reader &reader, std::vector<uint8_t> &out) {
			out.clear();
			while (true) {
				if (!reader.Has(1)) {
					return false;
				}
				const uint8_t length = reader.Byte();
				if (length == 0) {
					return true;
				}
				if (!reader.Has(length)) {
					return false;
				}
				for (uint8_t index = 0; index < length; index++) {
					out.push_back(reader.Byte());
				}
			}
		}

		// LZW, as GIF spells it.
		//
		// **Codes are little-endian across bytes and the width grows with the
		// dictionary**, which is the part that is easy to get subtly wrong: the
		// width increases *after* the code that filled the table, not before, and a
		// decoder that increments early reads every subsequent code shifted by one
		// bit and produces plausible garbage.
		//
		// The dictionary is a pair of parallel arrays rather than a vector of
		// vectors: an entry is "a previous entry plus one byte", so a prefix index
		// and a suffix byte describe it in five bytes instead of an allocation.
		bool InflateGif(
			const std::vector<uint8_t> &data, uint8_t minimumWidth, size_t expected, std::vector<uint8_t> &out
		) {
			if (minimumWidth < 2 || minimumWidth > 11) {
				return false;
			}

			const uint16_t clearCode = static_cast<uint16_t>(1u << minimumWidth);
			const uint16_t endCode = static_cast<uint16_t>(clearCode + 1);

			constexpr size_t TABLE = 4096;
			std::vector<uint16_t> prefix(TABLE, 0);
			std::vector<uint8_t> suffix(TABLE, 0);

			out.clear();
			out.reserve(expected);

			uint16_t next = static_cast<uint16_t>(endCode + 1);
			uint8_t width = static_cast<uint8_t>(minimumWidth + 1);
			int32_t previous = -1;

			uint32_t bits = 0;
			uint32_t held = 0;
			size_t at = 0;

			// Scratch for walking an entry back to its first byte. An entry is at
			// most the table's depth, so this never grows past 4096.
			std::vector<uint8_t> unwound;

			while (true) {
				while (held < width) {
					if (at >= data.size()) {
						// Ran out of codes. **Not a failure**: an encoder that
						// omitted the end code is common enough that refusing would
						// reject real files, and what has been decoded is a whole
						// number of pixels.
						return true;
					}
					bits |= static_cast<uint32_t>(data[at++]) << held;
					held += 8;
				}

				const auto code = static_cast<uint16_t>(bits & ((1u << width) - 1));
				bits >>= width;
				held -= width;

				if (code == clearCode) {
					next = static_cast<uint16_t>(endCode + 1);
					width = static_cast<uint8_t>(minimumWidth + 1);
					previous = -1;
					continue;
				}
				if (code == endCode) {
					return true;
				}

				unwound.clear();

				uint16_t walk = code;
				if (code == next && previous >= 0) {
					// **The self-referential case, which every LZW decoder needs and
					// which no encoder will let you skip.** A code may name the entry
					// it is about to create; its expansion is the previous entry plus
					// that entry's own first byte.
					walk = static_cast<uint16_t>(previous);
					unwound.push_back(0); // Placeholder, filled once the first byte is known.
				} else if (code > next) {
					return false;
				}

				while (walk > endCode) {
					if (walk >= TABLE) {
						return false;
					}
					unwound.push_back(suffix[walk]);
					walk = prefix[walk];

					// A malformed table can point at itself. Bounded by the table's
					// size, which is what stops a crafted file spinning here.
					if (unwound.size() > TABLE) {
						return false;
					}
				}

				const auto first = static_cast<uint8_t>(walk);
				unwound.push_back(first);

				if (code == next && previous >= 0) {
					// Fill the placeholder that was pushed first and is therefore
					// last once reversed.
					unwound[0] = first;
				}

				for (size_t index = unwound.size(); index-- > 0;) {
					out.push_back(unwound[index]);
				}

				if (previous >= 0 && next < TABLE) {
					prefix[next] = static_cast<uint16_t>(previous);
					suffix[next] = first;
					next++;

					// **After the entry is added, not before.** See the note above.
					if (next == (1u << width) && width < 12) {
						width++;
					}
				}

				previous = code;

				if (out.size() > expected * 2 + 64) {
					// Far more output than the frame can hold. A corrupt or hostile
					// stream, refused rather than allowed to grow.
					return false;
				}
			}
		}

		// The row a frame's `y`th decoded line belongs on.
		//
		// GIF's interlace is four passes at decreasing stride. **A table rather
		// than a chain of ifs**, because the four passes are the format and reading
		// them as data is how a reader checks them against the spec.
		uint32_t InterlacedRow(uint32_t y, uint32_t height) {
			struct Pass {
				uint32_t Start;
				uint32_t Stride;
			};
			static constexpr Pass PASSES[] = {{0, 8}, {4, 8}, {2, 4}, {1, 2}};

			uint32_t seen = 0;
			for (const Pass &pass : PASSES) {
				const uint32_t rows =
					pass.Start < height ? (height - pass.Start + pass.Stride - 1) / pass.Stride : 0;
				if (y < seen + rows) {
					return pass.Start + (y - seen) * pass.Stride;
				}
				seen += rows;
			}
			return y;
		}
	}

	bool ReadGif(std::span<const std::byte> bytes, assets::TextureData &out, std::string &failure) {
		Reader reader{bytes, 0};

		if (!reader.Has(13)) {
			failure = "shorter than a GIF header";
			return false;
		}

		// `GIF87a` or `GIF89a`. The version is read and not acted on: 89a adds the
		// extensions this decoder already skips or reads, and a file claiming 87a
		// while using them is one every other decoder accepts too.
		if (std::memcmp(bytes.data(), "GIF8", 4) != 0) {
			failure = "not a GIF";
			return false;
		}
		reader.Skip(6);

		const uint32_t canvasWidth = reader.Short();
		const uint32_t canvasHeight = reader.Short();
		if (canvasWidth == 0 || canvasHeight == 0) {
			failure = "a GIF with no canvas";
			return false;
		}

		const uint8_t packed = reader.Byte();
		reader.Skip(2); // Background index and pixel aspect ratio; neither is used.

		std::vector<Pixel> global;
		if ((packed & 0x80) != 0) {
			const uint32_t size = 1u << ((packed & 0x07) + 1);
			if (!ReadPalette(reader, size, global)) {
				failure = "a global colour table running past the end";
				return false;
			}
		}

		// **The canvas is composited into and each frame is captured off it**,
		// which is what makes disposal methods work at all: a GIF frame is usually
		// a *patch* over the previous image rather than a whole picture.
		std::vector<Pixel> canvas(static_cast<size_t>(canvasWidth) * canvasHeight);
		std::vector<std::vector<Pixel>> frames;

		int32_t transparentIndex = -1;
		uint8_t disposal = 0;

		// The delay the last graphic control block named, in hundredths of a
		// second, and the sum of the ones that were actually used.
		//
		// **Summed rather than kept per frame, because a flipbook has one
		// rate.** GIF permits a different delay on every frame and encoders
		// occasionally use it; a grid sampled by cell index cannot express that,
		// so the total over the whole animation is what a single rate is derived
		// from. That is a real approximation and `TextureData::FlipbookFrameRate`
		// says so rather than pretending the conversion is lossless.
		uint16_t pendingDelay = 0;
		uint64_t totalHundredths = 0;

		std::vector<uint8_t> blocks;
		std::vector<uint8_t> indices;
		std::vector<Pixel> local;

		// Counted past the cap so the line below can say what was lost rather
		// than only what was kept.
		uint32_t seenImages = 0;

		while (reader.Has(1) && frames.size() < MAX_SIDE * MAX_SIDE) {
			const uint8_t marker = reader.Byte();

			if (marker == 0x3B) {
				break; // Trailer.
			}

			if (marker == 0x21) {
				const uint8_t label = reader.Byte();
				if (label == 0xF9) {
					// Graphic control: transparency and disposal, which are the two
					// things that change how the next frame composites.
					const uint8_t length = reader.Byte();
					if (length != 4 || !reader.Has(5)) {
						failure = "a malformed graphic control block";
						return false;
					}
					const uint8_t flags = reader.Byte();

					// **The delay, which used to be skipped and is the whole of
					// the frame rate.** It is hundredths of a second and it
					// applies to the frame that follows this block, so it is
					// held here and banked when that frame is captured.
					pendingDelay = reader.Short();

					const uint8_t index = reader.Byte();
					reader.Byte(); // Block terminator.

					disposal = static_cast<uint8_t>((flags >> 2) & 0x07);
					transparentIndex = (flags & 0x01) != 0 ? static_cast<int32_t>(index) : -1;
				} else {
					// Comment, application or plain text. Skipped whole.
					if (!ReadBlocks(reader, blocks)) {
						failure = "an extension block running past the end";
						return false;
					}
				}
				continue;
			}

			if (marker != 0x2C) {
				failure = "an unrecognised GIF block";
				return false;
			}

			// An image descriptor.
			seenImages++;
			if (!reader.Has(9)) {
				failure = "an image descriptor running past the end";
				return false;
			}
			const uint32_t left = reader.Short();
			const uint32_t top = reader.Short();
			const uint32_t width = reader.Short();
			const uint32_t height = reader.Short();
			const uint8_t imagePacked = reader.Byte();

			if (width == 0 || height == 0 || left + width > canvasWidth || top + height > canvasHeight) {
				failure = "an image frame outside the canvas";
				return false;
			}

			const std::vector<Pixel> *palette = &global;
			if ((imagePacked & 0x80) != 0) {
				const uint32_t size = 1u << ((imagePacked & 0x07) + 1);
				if (!ReadPalette(reader, size, local)) {
					failure = "a local colour table running past the end";
					return false;
				}
				palette = &local;
			}

			if (palette->empty()) {
				failure = "a GIF frame with no colour table";
				return false;
			}

			const bool interlaced = (imagePacked & 0x40) != 0;

			if (!reader.Has(1)) {
				failure = "an image with no data";
				return false;
			}
			const uint8_t minimumWidth = reader.Byte();
			if (!ReadBlocks(reader, blocks)) {
				failure = "image data running past the end";
				return false;
			}

			const size_t pixels = static_cast<size_t>(width) * height;
			if (!InflateGif(blocks, minimumWidth, pixels, indices) || indices.size() < pixels) {
				failure = "a GIF frame that would not decompress";
				return false;
			}

			// **The canvas before this frame is kept, for disposal 3.** Copied only
			// when that disposal is asked for, because it is rare and the copy is
			// the whole canvas.
			std::vector<Pixel> restore;
			if (disposal == 3) {
				restore = canvas;
			}

			for (uint32_t y = 0; y < height; y++) {
				const uint32_t row = interlaced ? InterlacedRow(y, height) : y;
				for (uint32_t x = 0; x < width; x++) {
					const uint8_t index = indices[static_cast<size_t>(y) * width + x];
					if (transparentIndex >= 0 && index == transparentIndex) {
						// **Left alone rather than written clear**, which is what
						// makes a patch a patch: a transparent pixel in a frame
						// means "whatever was underneath".
						continue;
					}
					if (index >= palette->size()) {
						continue;
					}
					canvas[static_cast<size_t>(top + row) * canvasWidth + (left + x)] = (*palette)[index];
				}
			}

			frames.push_back(canvas);

			// **A delay of 0 or 1 means "as fast as the viewer can", and every
			// browser reads that as 100ms.** Following the de-facto rule rather
			// than the specification is the right call here: an encoder writing
			// zero expected the thing everybody actually does, and taking it
			// literally would divide by zero or claim a hundred frames a second.
			totalHundredths += pendingDelay < 2 ? DEFAULT_HUNDREDTHS : pendingDelay;
			pendingDelay = 0;

			if (disposal == 2) {
				// Restore to background: the frame's own rectangle goes clear.
				for (uint32_t y = 0; y < height; y++) {
					for (uint32_t x = 0; x < width; x++) {
						canvas[static_cast<size_t>(top + y) * canvasWidth + (left + x)] = Pixel{};
					}
				}
			} else if (disposal == 3) {
				canvas = restore;
			}
		}

		if (frames.empty()) {
			failure = "a GIF with no frames";
			return false;
		}

		// **The truncation is documented in the file comment and silent at run
		// time.** A 70-frame GIF becomes a 64-frame one and the animation is
		// simply shorter, which is the kind of thing a person reports as the
		// import being broken.
		if (reader.Has(1) && frames.size() >= MAX_SIDE * MAX_SIDE) {
			ENGINE_WARN(
				"gif truncated at {} frames; the grid holds no more and the rest of the file is not read",
				frames.size()
			);
		}
		ENGINE_DEBUG(
			"gif: {} frame(s) kept of {} image descriptor(s), {}x{} canvas",
			frames.size(),
			seenImages,
			canvasWidth,
			canvasHeight
		);

		// **The grid is the next power of two that fits, and square.** That is
		// `effects::FlipbookLayout`'s shape and not a choice this decoder gets to
		// make - a sheet the particle path cannot describe is a sheet it cannot
		// draw.
		uint32_t side = 1;
		while (side * side < frames.size() && side < MAX_SIDE) {
			side *= 2;
		}

		const uint32_t sheetWidth = canvasWidth * side;
		const uint32_t sheetHeight = canvasHeight * side;

		// The same ceiling every other decoder here uses, so a hostile GIF cannot
		// ask for a gigabyte by claiming a large canvas and sixty-four frames.
		constexpr uint64_t MAX_PIXELS = 64ull * 1024 * 1024;
		if (static_cast<uint64_t>(sheetWidth) * sheetHeight > MAX_PIXELS) {
			failure = "a GIF whose flipbook would be larger than the pixel ceiling";
			return false;
		}

		out.Width = sheetWidth;
		out.Height = sheetHeight;
		out.Format = assets::TextureFormat::RGBA8;

		// **The grid, how much of it holds a frame, and how fast it plays.**
		// Without these three the sheet is just pixels: a 4x4 flipbook and a 4x4
		// tile atlas are the same image, and every scene that wanted to play one
		// would have to be told the numbers by hand. `TextureData` carries why
		// they belong on the texture rather than on whatever draws it.
		out.FlipbookSide = static_cast<uint8_t>(side);
		out.FlipbookFrames = static_cast<uint8_t>(frames.size());

		// Total duration over frame count, which is a mean and is stated as one.
		// A GIF whose frames each name a different delay cannot be a flipbook
		// without this approximation, and the alternative - refusing it - would
		// turn a usable asset into a failed import for a property nothing in the
		// engine could have used anyway.
		const double seconds = static_cast<double>(totalHundredths) / 100.0;
		out.FlipbookFrameRate =
			seconds > 0.0 ? static_cast<float>(static_cast<double>(frames.size()) / seconds) : 0.0f;

		out.Pixels.assign(static_cast<size_t>(sheetWidth) * sheetHeight * 4, std::byte{0});

		for (size_t frame = 0; frame < frames.size(); frame++) {
			const uint32_t cellX = static_cast<uint32_t>(frame % side) * canvasWidth;
			const uint32_t cellY = static_cast<uint32_t>(frame / side) * canvasHeight;

			for (uint32_t y = 0; y < canvasHeight; y++) {
				for (uint32_t x = 0; x < canvasWidth; x++) {
					const Pixel &pixel = frames[frame][static_cast<size_t>(y) * canvasWidth + x];
					const size_t at = ((static_cast<size_t>(cellY + y) * sheetWidth) + (cellX + x)) * 4;
					out.Pixels[at + 0] = static_cast<std::byte>(pixel.R);
					out.Pixels[at + 1] = static_cast<std::byte>(pixel.G);
					out.Pixels[at + 2] = static_cast<std::byte>(pixel.B);
					out.Pixels[at + 3] = static_cast<std::byte>(pixel.A);
				}
			}
		}

		return true;
	}
}
