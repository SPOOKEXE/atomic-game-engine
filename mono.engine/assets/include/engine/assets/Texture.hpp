#pragma once

// What an `AssetKind::Texture`'s bytes are.
//
// **The gap was never a missing decoder.** `ImageLabel` drew the missing-texture
// marker because `AssetKind::Texture` named a kind and nothing said what a
// texture *is* — so there was nothing for a backend to sample even once the
// bytes had arrived. Vendoring a PNG decoder would have answered a different
// question: how to read somebody else's format, when what was needed was to
// have one.
//
// So this is the format, and it is deliberately the dullest possible: a header
// and the pixels a GPU wants, in the order it wants them. **A runtime does not
// decode.** Turning a PNG into this is a publishing step — the same division
// `Chunker` and `Manifest` already draw, where the origin does the work once
// and every client does none.
//
// That is not a shortcut around the decoder; it is why a decoder belongs in
// `mono.tools` rather than in a game binary. A client that decoded PNGs would
// pay for a Huffman tree on the frame a texture streamed in, and a shipped game
// would carry a decompressor for a format it never has to read.
//
// **Uncompressed here, compressed in transit.** `delivery::GroupCodec` runs
// zstd over whatever a group holds, so a raw sheet costs its real size on disk
// and its compressed size on the wire — and the client's decompression is the
// one it was already doing for every other asset rather than a second one for
// this kind alone.
//
// @tier L8 · shared

#include <engine/core/Bytes.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace engine::assets {

	// How the pixels are laid out.
	//
	// **A closed list that grows, and the ordinal is on the wire**, so an entry
	// may be added at the end and never reordered — `ecs::PropertyType`'s rule
	// and for the same reason.
	//
	// @since v0.8
	enum class TextureFormat : uint8_t {
		// Eight bits per channel, red-green-blue-alpha, non-premultiplied.
		// What an interface image is and what a colour texture is.
		RGBA8 = 0,

		// One channel. A mask, a coverage sheet, a height field.
		R8 = 1,
	};

	// How many bytes one pixel of a format takes.
	//
	// @param format The layout.
	// @return The stride of a single pixel.
	constexpr uint32_t BytesPerPixel(TextureFormat format) {
		return format == TextureFormat::R8 ? 1u : 4u;
	}

	// One axis of a mip level, halving and stopping at one.
	//
	// **A level's shape is derived, not stored**, and that is the same argument
	// `Mesh::Write` makes for the bounding box: a stored width and height per
	// level is a second copy of a fact the base dimensions already carry, and it
	// is the copy an attacker gets to choose. Deriving it means a level is either
	// exactly the size the rule says or the file is refused.
	//
	// @param extent The width or height at level zero.
	// @param level  Zero for the base image.
	// @return The extent at that level, never zero.
	constexpr uint32_t MipExtent(uint32_t extent, uint32_t level) {
		const uint32_t shifted = level >= 32 ? 0u : extent >> level;
		return shifted == 0 ? 1u : shifted;
	}

	// How many levels a full chain over these dimensions runs to.
	//
	// **It runs until both axes are one, not until the shorter one is.** A 64x1
	// strip has seven levels; stopping at the first axis to reach one would give
	// it a single level and leave it aliasing exactly as it did before.
	//
	// @param width  The image's width.
	// @param height The image's height.
	// @return The count, level zero included. Never zero.
	constexpr uint32_t MipLevelCount(uint32_t width, uint32_t height) {
		uint32_t levels = 1;
		while (width > 1 || height > 1) {
			width = MipExtent(width, 1);
			height = MipExtent(height, 1);
			levels++;
		}
		return levels;
	}

	// A decoded image, ready to upload.
	//
	// @since v0.8
	struct TextureData {
		// The image's shape and how its bytes are laid out.
		//
		// **The format is carried rather than assumed** because `R8` exists: a
		// mask uploaded as RGBA would be four times the device memory, and one
		// sampled as RGBA without being widened would draw red.
		//@{
		uint32_t Width = 0;
		uint32_t Height = 0;
		TextureFormat Format = TextureFormat::RGBA8;
		//@}

		// Row-major, top row first, `Width * BytesPerPixel` per row and no
		// padding between them.
		//
		// **No row alignment**, which is a decision rather than an omission: a
		// stride is a property of the *device* a texture is being uploaded to,
		// and baking one into the format would make the file depend on whichever
		// GPU the publisher happened to have. A backend that needs 256-byte rows
		// pads on upload, where it knows the number.
		std::vector<std::byte> Pixels;

		// The smaller copies, level one first, or empty for an image with no
		// chain.
		//
		// **A vector of levels rather than one buffer holding the whole chain**,
		// and the reason is what `Pixels` already means. A dozen call sites read
		// `Pixels` as *the image* and check it against `Width * Height *
		// BytesPerPixel` — `IsValid`, the GPU upload, `ResizeImage`, the opaque
		// pass, the thumbnailer. Concatenating would make every one of them read
		// a buffer a third too long while still compiling, and `IsValid` would
		// have to stop meaning what it means. Levels beside the base cost a
		// handful of allocations on a path that already decoded a PNG, and they
		// cost nothing to a reader who does not know they are there.
		//
		// Each level's dimensions come from `MipExtent`, so a level holds exactly
		// `MipExtent(Width, n) * MipExtent(Height, n) * BytesPerPixel` bytes and
		// `IsValid` refuses anything else.
		//
		// @since v0.14
		std::vector<std::vector<std::byte>> Mips;

		// The flipbook grid's side, or zero for a still image.
		//
		// **A sheet of animation frames is still one texture, and that is the
		// whole reason this is three fields rather than a new asset kind.** A
		// GIF decodes to a square power-of-two grid of cells — `bake/Gif.cpp`
		// carries that argument — so what a sampler needs is unchanged and what
		// is *lost* without these three is how to play it: a 4x4 sheet and a
		// 4x4 tile atlas are the same pixels.
		//
		// @since v0.10
		uint8_t FlipbookSide = 0;

		// How many of the grid's cells hold a frame.
		//
		// **Not `Side * Side`, because a real animation rarely fills a square.**
		// A 24-frame GIF lands in an 8x8 with forty cells empty, and a player
		// that walked all sixty-four would spend five eighths of every loop
		// showing nothing. Mirrors `effects::ParticleEmitter::FlipbookFrames`,
		// which is where the same number ends up.
		//
		// @since v0.10
		uint8_t FlipbookFrames = 0;

		// Frames a second the source was authored at, or zero when unknown.
		//
		// **Read from the source rather than assumed.** A GIF states a delay
		// per frame and this is what those delays average to; a sheet drawn by
		// hand states nothing, which is what zero means. A consumer that gets
		// zero picks its own rate — for a particle that is "one loop over the
		// lifetime", which is what the engine did before there was anything to
		// ask.
		//
		// **One rate for the sheet, and a GIF may not have one.** Per-frame
		// delays are a thing GIF permits and encoders occasionally use; a
		// flipbook has a single rate by construction, so a varying one is
		// averaged and that is a real approximation rather than a lossless
		// conversion.
		//
		// @since v0.10
		float FlipbookFrameRate = 0.0f;

		// Whether this describes an image at all.
		//
		// @return `true` when the dimensions are non-zero, the pixel count
		//         matches them exactly, and every level in `Mips` is exactly the
		//         size its position implies.
		bool IsValid() const;

		// How many levels this image has, the base included.
		//
		// @return One for an image with no chain.
		uint32_t LevelCount() const {
			return 1u + static_cast<uint32_t>(Mips.size());
		}

		// Whether this image is a sheet of animation frames.
		//
		// @return `true` when the grid has a side and at least one frame in it.
		bool IsFlipbook() const {
			return FlipbookSide > 0 && FlipbookFrames > 0;
		}
	};

	// Reading and writing the texture format.
	//
	// Static, because a texture has no state — the same shape `Packet` has for
	// the same reason.
	//
	// @since v0.8
	class Texture {
	  public:
		// "ATX1", and the reason every format here has one: a file that is not
		// this format has to fail as "not this format" rather than as a
		// plausible width of nine hundred million.
		static constexpr uint32_t MAGIC = 0x31585441;

		// The version. Bumped when the layout changes, never reused.
		//
		// **3 adds the mip chain, 2 added the three flipbook fields, and 1 is
		// still read.** Not compatibility for its own sake — this is pre-release
		// and the standing rule is that a format break is acceptable — but
		// because every absent case has an obviously right answer: a v1 file is a
		// still image and a v2 file is a texture with one level, which is what
		// zero and one already mean. Refusing them would make every baked texture
		// in every store on disk unreadable to buy nothing.
		//
		// The level count is a byte after the flipbook rate and before the
		// pixels, for the reason the flipbook triple sits there: the pixels run
		// to the end of the payload, so anything written after them would be
		// unreachable. **The count is bounded by `MipLevelCount` of the
		// dimensions rather than by a constant of its own**, which is the tighter
		// bound and the one that stays true if `MAXIMUM_DIMENSION` moves.
		static constexpr uint16_t VERSION = 3;

		// The largest image this will read.
		//
		// **A bound on what a corrupt header can make a reader allocate**, and
		// the same reasoning `Packet::MAXIMUM_PAYLOAD_BYTES` and
		// `GroupCodec::MAXIMUM_PAYLOAD_BYTES` carry: four bytes from a file are
		// an out-of-memory kill without one. 16384 on a side at RGBA8 is a
		// gigabyte, which is far past any real texture and far below anything
		// that would take a machine down before the check ran.
		static constexpr uint32_t MAXIMUM_DIMENSION = 16384;

		// Writes an image.
		//
		// @param writer Where the bytes go.
		// @param data   The image. An invalid one writes nothing.
		// @return `false` when `data` is not a valid image.
		static bool Write(core::ByteWriter &writer, const TextureData &data);

		// Reads an image, refusing anything that is not one.
		//
		// Refuses a wrong magic, an unknown version, a format outside the enum,
		// a dimension of zero or past `MAXIMUM_DIMENSION`, a level count of zero
		// or past what the dimensions allow, and a pixel count that disagrees
		// with the dimensions. **The whole chain's size is summed and checked
		// against the bytes actually present before anything is allocated**,
		// which is the difference between a refusal and a decompression bomb.
		//
		// @param reader The bytes to parse.
		// @param out    Filled in on success, left alone otherwise — so a caller
		//               reusing one cannot act on a mixture of the last good
		//               image and a bad one.
		// @return `false` on anything malformed. Drop it and count it.
		static bool Read(core::ByteReader &reader, TextureData &out);
	};
}
