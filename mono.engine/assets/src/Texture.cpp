#include <engine/assets/Texture.hpp>

#include <algorithm>

namespace engine::assets {

	bool TextureData::IsValid() const {
		if (Width == 0 || Height == 0) {
			return false;
		}

		// **Computed in 64 bits and compared against the vector's size**, so a
		// width and height whose product overflows 32 bits cannot describe a
		// small buffer as a large image.
		const uint64_t expected = static_cast<uint64_t>(Width) * static_cast<uint64_t>(Height) *
								  static_cast<uint64_t>(BytesPerPixel(Format));
		return expected == static_cast<uint64_t>(Pixels.size());
	}

	bool Texture::Write(core::ByteWriter &writer, const TextureData &data) {
		if (!data.IsValid()) {
			return false;
		}
		if (data.Width > MAXIMUM_DIMENSION || data.Height > MAXIMUM_DIMENSION) {
			return false;
		}

		writer.WriteUInt32(MAGIC);
		writer.WriteUInt16(VERSION);
		writer.WriteUInt8(static_cast<uint8_t>(data.Format));
		writer.WriteUInt32(data.Width);
		writer.WriteUInt32(data.Height);

		// **Before the pixels, because the pixels have no length of their own.**
		// Everything after them would be unreachable: the reader takes the rest
		// of the payload as the image, which is exactly what stops a second
		// count from disagreeing with the first.
		writer.WriteUInt8(data.FlipbookSide);
		writer.WriteUInt8(data.FlipbookFrames);
		writer.WriteFloat(data.FlipbookFrameRate);

		// Raw, with no length of its own: the dimensions and the format say how
		// many bytes follow, and a second count would be a second answer that a
		// corrupt file could make disagree with the first.
		writer.WriteRaw(data.Pixels.data(), data.Pixels.size());
		return true;
	}

	bool Texture::Read(core::ByteReader &reader, TextureData &out) {
		if (reader.ReadUInt32() != MAGIC) {
			return false;
		}
		// **1 and 2 are both read**, and the difference is only the three
		// flipbook fields. A v1 file is a still image, which is what their
		// zeroes already mean — see `VERSION`.
		const uint16_t version = reader.ReadUInt16();
		if (version == 0 || version > VERSION) {
			return false;
		}

		const uint8_t format = reader.ReadUInt8();
		if (format > static_cast<uint8_t>(TextureFormat::R8)) {
			// **Range-checked before the cast**, for `ReadMessage`'s reason: a
			// cast of an out-of-range byte produces a value no switch handles,
			// and every consumer downstream then reads something the type says
			// cannot exist.
			return false;
		}

		const uint32_t width = reader.ReadUInt32();
		const uint32_t height = reader.ReadUInt32();

		uint8_t side = 0;
		uint8_t frames = 0;
		float frameRate = 0.0f;
		if (version >= 2) {
			side = reader.ReadUInt8();
			frames = reader.ReadUInt8();
			frameRate = reader.ReadFloat();
		}

		if (reader.Failed()) {
			return false;
		}
		if (width == 0 || height == 0 || width > MAXIMUM_DIMENSION || height > MAXIMUM_DIMENSION) {
			return false;
		}

		const uint64_t bytes = static_cast<uint64_t>(width) * static_cast<uint64_t>(height) *
							   static_cast<uint64_t>(BytesPerPixel(static_cast<TextureFormat>(format)));

		// **Checked against what is actually there before a byte is
		// allocated.** This is the decompression-bomb check: a header claiming a
		// gigabyte over a forty-byte file must cost a comparison rather than a
		// gigabyte.
		if (bytes > static_cast<uint64_t>(reader.Remaining())) {
			return false;
		}

		const std::span<const std::byte> pixels = reader.ReadRawView(static_cast<size_t>(bytes));
		if (reader.Failed()) {
			return false;
		}

		// Assigned only once everything has been checked, so a caller reusing a
		// `TextureData` across a load loop cannot act on a mixture of the last
		// good image and a bad one.
		out.Width = width;
		out.Height = height;
		out.Format = static_cast<TextureFormat>(format);

		// **A frame count past the grid is clamped rather than refused**, which
		// is the opposite of how every other field here is treated and is right
		// for the same reason the rest are not: the others decide how many bytes
		// to allocate, and this one decides which cell to sample. A wrong count
		// is a shorter animation; a wrong dimension is a buffer overrun.
		out.FlipbookSide = side;
		out.FlipbookFrames = side == 0 ? 0
									   : static_cast<uint8_t>(std::min<uint32_t>(
											 frames == 0 ? static_cast<uint32_t>(side) * side : frames,
											 static_cast<uint32_t>(side) * side
										 ));

		// A negative or non-finite rate reads as "unknown" rather than being
		// refused, for the reason above — and because a consumer already has to
		// handle zero.
		out.FlipbookFrameRate = frameRate > 0.0f && frameRate < 1000.0f ? frameRate : 0.0f;

		out.Pixels.assign(pixels.begin(), pixels.end());
		return true;
	}
}
