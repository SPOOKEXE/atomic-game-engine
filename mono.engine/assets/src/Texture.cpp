#include <engine/assets/Texture.hpp>

#include <algorithm>
#include <cmath>

namespace engine::assets {

	bool TextureData::IsValid() const {
		if (Width == 0 || Height == 0) {
			return false;
		}
		const uint32_t side = FlipbookSide;
		if ((side != 0 && side != 1 && side != 2 && side != 4 && side != 8 && side != 16) ||
			(side == 0 && FlipbookFrames != 0) ||
			(side != 0 && (FlipbookFrames == 0 || FlipbookFrames > side * side))) {
			return false;
		}
		if (!FlipbookFrameDurations.empty()) {
			if (FlipbookFrameDurations.size() != FlipbookFrames || FlipbookFrameRate != 0.0f) {
				return false;
			}
			float end = 0.0f;
			for (const float duration : FlipbookFrameDurations) {
				const float next = end + duration;
				if (!std::isfinite(duration) || duration <= 0.0f || !std::isfinite(next) || next <= end) {
					return false;
				}
				end = next;
			}
		}

		// **Computed in 64 bits and compared against the vector's size**, so a
		// width and height whose product overflows 32 bits cannot describe a
		// small buffer as a large image.
		const uint64_t expected = static_cast<uint64_t>(Width) * static_cast<uint64_t>(Height) *
								  static_cast<uint64_t>(BytesPerPixel(Format));
		if (expected != static_cast<uint64_t>(Pixels.size())) {
			return false;
		}

		// **A chain is either complete-shaped or refused, never nearly right.** A
		// level one byte short is a texture a GPU reads off the end of, and no
		// consumer below this point has anything left to check it against.
		if (LevelCount() > MipLevelCount(Width, Height)) {
			return false;
		}
		for (size_t index = 0; index < Mips.size(); index++) {
			const uint32_t level = static_cast<uint32_t>(index) + 1;
			const uint64_t bytes = static_cast<uint64_t>(MipExtent(Width, level)) *
								   static_cast<uint64_t>(MipExtent(Height, level)) *
								   static_cast<uint64_t>(BytesPerPixel(Format));
			if (bytes != static_cast<uint64_t>(Mips[index].size())) {
				return false;
			}
		}
		return true;
	}

	bool Texture::Write(core::ByteWriter &writer, const TextureData &data) {
		if (!data.IsValid()) {
			return false;
		}
		if (data.Width > MAXIMUM_DIMENSION || data.Height > MAXIMUM_DIMENSION) {
			return false;
		}
		const uint32_t side = data.FlipbookSide;
		if ((side != 0 && side != 1 && side != 2 && side != 4 && side != 8 && side != 16) ||
			(side == 0 && data.FlipbookFrames != 0) ||
			(side != 0 && (data.FlipbookFrames == 0 || data.FlipbookFrames > side * side))) {
			return false;
		}

		writer.WriteUInt32(MAGIC);
		writer.WriteUInt16(data.FlipbookFrameDurations.empty() ? 4 : VERSION);
		writer.WriteUInt8(static_cast<uint8_t>(data.Format));
		writer.WriteUInt32(data.Width);
		writer.WriteUInt32(data.Height);

		// **Before the pixels, because the pixels have no length of their own.**
		// Everything after them would be unreachable: the reader takes the rest
		// of the payload as the image, which is exactly what stops a second
		// count from disagreeing with the first.
		writer.WriteUInt8(data.FlipbookSide);
		writer.WriteUInt16(data.FlipbookFrames);
		writer.WriteFloat(data.FlipbookFrameRate);
		if (!data.FlipbookFrameDurations.empty()) {
			writer.WriteUInt16(static_cast<uint16_t>(data.FlipbookFrameDurations.size()));
			for (const float duration : data.FlipbookFrameDurations)
				writer.WriteFloat(duration);
		}

		// The count and not the levels' sizes, which `MipExtent` derives. Bounded
		// by `MipLevelCount` and therefore by `MAXIMUM_DIMENSION`, so the cast is
		// safe for anything `IsValid` accepted.
		writer.WriteUInt8(static_cast<uint8_t>(data.LevelCount()));

		// Raw, with no length of its own: the dimensions, the format and the
		// level count say how many bytes follow, and a second count would be a
		// second answer that a corrupt file could make disagree with the first.
		writer.WriteRaw(data.Pixels.data(), data.Pixels.size());
		for (const std::vector<std::byte> &level : data.Mips) {
			writer.WriteRaw(level.data(), level.size());
		}
		return true;
	}

	bool Texture::Read(core::ByteReader &reader, TextureData &out) {
		if (reader.ReadUInt32() != MAGIC) {
			return false;
		}
		// **1 through 5 are all read.** A v1 file is a still image and a v2 file
		// is a one-level texture, which is what their zeroes and their absent
		// count already mean - see `VERSION`.
		const uint16_t version = reader.ReadUInt16();
		if (version == 0 || version > VERSION) {
			return false;
		}

		const uint8_t format = reader.ReadUInt8();
		if (format > static_cast<uint8_t>(TextureFormat::RGBA8_LINEAR)) {
			// **Range-checked before the cast**, for `ReadMessage`'s reason: a
			// cast of an out-of-range byte produces a value no switch handles,
			// and every consumer downstream then reads something the type says
			// cannot exist.
			return false;
		}

		const uint32_t width = reader.ReadUInt32();
		const uint32_t height = reader.ReadUInt32();

		uint8_t side = 0;
		uint16_t frames = 0;
		float frameRate = 0.0f;
		if (version >= 2) {
			side = reader.ReadUInt8();
			frames = version >= 4 ? reader.ReadUInt16() : reader.ReadUInt8();
			frameRate = reader.ReadFloat();
		}
		std::vector<float> durations;
		if (version >= 5) {
			const uint16_t count = reader.ReadUInt16();
			if (reader.Failed() || count == 0 || count != frames || count > 256 || frameRate != 0.0f ||
				reader.Remaining() < static_cast<size_t>(count) * sizeof(float) + 1) {
				return false;
			}
			durations.reserve(count);
			float end = 0.0f;
			for (uint16_t index = 0; index < count; index++) {
				const float duration = reader.ReadFloat();
				const float next = end + duration;
				if (!std::isfinite(duration) || duration <= 0.0f || !std::isfinite(next) || next <= end) {
					return false;
				}
				durations.push_back(duration);
				end = next;
			}
		}

		// A file from before the chain existed is a texture with one level.
		uint8_t levels = 1;
		if (version >= 3) {
			levels = reader.ReadUInt8();
		}

		if (reader.Failed()) {
			return false;
		}
		if (width == 0 || height == 0 || width > MAXIMUM_DIMENSION || height > MAXIMUM_DIMENSION) {
			return false;
		}
		if (version >= 4 &&
			((side != 0 && side != 1 && side != 2 && side != 4 && side != 8 && side != 16) ||
			 (side == 0 && frames != 0) || (side != 0 && (frames == 0 || frames > side * side)))) {
			return false;
		}

		// **Refused rather than clamped, unlike the frame count.** A level count
		// decides how many bytes to allocate and where each one starts, which
		// puts it with the dimensions rather than with the flipbook fields.
		if (levels == 0 || levels > MipLevelCount(width, height)) {
			return false;
		}

		const uint32_t pixelStride = BytesPerPixel(static_cast<TextureFormat>(format));

		// The whole chain, summed in 64 bits before a byte is allocated. This is
		// the decompression-bomb check: a header claiming a gigabyte over a
		// forty-byte file must cost a comparison rather than a gigabyte, and a
		// chain makes that a sum rather than one product.
		uint64_t total = 0;
		for (uint32_t level = 0; level < levels; level++) {
			total += static_cast<uint64_t>(MipExtent(width, level)) *
					 static_cast<uint64_t>(MipExtent(height, level)) * static_cast<uint64_t>(pixelStride);
		}
		if (total > static_cast<uint64_t>(reader.Remaining())) {
			return false;
		}

		const uint64_t bytes =
			static_cast<uint64_t>(width) * static_cast<uint64_t>(height) * static_cast<uint64_t>(pixelStride);
		const std::span<const std::byte> pixels = reader.ReadRawView(static_cast<size_t>(bytes));
		if (reader.Failed()) {
			return false;
		}

		// Read before anything is assigned, for the reason below: a caller
		// reusing a `TextureData` must never see a mixture of two images.
		std::vector<std::vector<std::byte>> mips;
		mips.reserve(levels - 1u);
		for (uint32_t level = 1; level < levels; level++) {
			const size_t levelBytes = static_cast<size_t>(MipExtent(width, level)) *
									  static_cast<size_t>(MipExtent(height, level)) * pixelStride;
			const std::span<const std::byte> view = reader.ReadRawView(levelBytes);
			if (reader.Failed()) {
				return false;
			}
			mips.emplace_back(view.begin(), view.end());
		}

		// Assigned only once everything has been checked, so a caller reusing a
		// `TextureData` across a load loop cannot act on a mixture of the last
		// good image and a bad one.
		out.Width = width;
		out.Height = height;
		out.Format = static_cast<TextureFormat>(format);

		// Legacy v2/v3 files keep their clamping rule. Version 4 validates above
		// so a new authored frame count cannot silently become a shorter animation.
		out.FlipbookSide = side;
		out.FlipbookFrames = side == 0 ? 0
									   : static_cast<uint16_t>(std::min<uint32_t>(
											 frames == 0 ? static_cast<uint32_t>(side) * side : frames,
											 static_cast<uint32_t>(side) * side
										 ));

		// A negative or non-finite rate reads as "unknown" rather than being
		// refused, for the reason above - and because a consumer already has to
		// handle zero.
		out.FlipbookFrameRate = frameRate > 0.0f && frameRate < 1000.0f ? frameRate : 0.0f;
		out.FlipbookFrameDurations = std::move(durations);

		out.Pixels.assign(pixels.begin(), pixels.end());
		out.Mips = std::move(mips);
		return true;
	}
}
