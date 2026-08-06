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

	// A decoded image, ready to upload.
	//
	// @since v0.8
	struct TextureData {
		uint32_t Width = 0;
		uint32_t Height = 0;
		TextureFormat Format = TextureFormat::RGBA8;

		// Row-major, top row first, `Width * BytesPerPixel` per row and no
		// padding between them.
		//
		// **No row alignment**, which is a decision rather than an omission: a
		// stride is a property of the *device* a texture is being uploaded to,
		// and baking one into the format would make the file depend on whichever
		// GPU the publisher happened to have. A backend that needs 256-byte rows
		// pads on upload, where it knows the number.
		std::vector<std::byte> Pixels;

		// Whether this describes an image at all.
		//
		// @return `true` when the dimensions are non-zero and the pixel count
		//         matches them exactly.
		bool IsValid() const;
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
		static constexpr uint16_t VERSION = 1;

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
		// a dimension of zero or past `MAXIMUM_DIMENSION`, and a pixel count
		// that disagrees with the dimensions. **The size is checked against the
		// bytes actually present before anything is allocated**, which is the
		// difference between a refusal and a decompression bomb.
		//
		// @param reader The bytes to parse.
		// @param out    Filled in on success, left alone otherwise — so a caller
		//               reusing one cannot act on a mixture of the last good
		//               image and a bad one.
		// @return `false` on anything malformed. Drop it and count it.
		static bool Read(core::ByteReader &reader, TextureData &out);
	};
}
