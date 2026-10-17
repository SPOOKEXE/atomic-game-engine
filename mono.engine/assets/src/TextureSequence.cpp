#include <engine/assets/TextureSequence.hpp>
#include <engine/core/Bytes.hpp>

#include <cmath>
#include <utility>

namespace engine::assets {
	namespace {
		uint64_t FrameBytes(const TextureSequenceData &data) {
			return uint64_t(data.Width) * data.Height * 4;
		}
	}

	bool TextureSequenceData::IsValid() const {
		if (Width == 0 || Height == 0 || Width > Texture::MAXIMUM_DIMENSION ||
			Height > Texture::MAXIMUM_DIMENSION ||
			(Format != TextureFormat::RGBA8 && Format != TextureFormat::RGBA8_LINEAR) ||
			FrameDurations.empty() || FrameDurations.size() > TextureSequence::MAXIMUM_FRAMES)
			return false;
		const uint64_t frameBytes = FrameBytes(*this);
		const uint64_t totalBytes = frameBytes * FrameDurations.size();
		if (totalBytes > TextureSequence::MAXIMUM_PIXEL_BYTES || totalBytes != Pixels.size()) return false;
		float end = 0.0f;
		for (float duration : FrameDurations) {
			const float next = end + duration;
			if (!std::isfinite(duration) || duration <= 0.0f || !std::isfinite(next) || next <= end)
				return false;
			end = next;
		}
		return true;
	}

	std::span<const std::byte> TextureSequenceData::FramePixels(size_t index) const {
		if (index >= FrameDurations.size() || Width == 0 || Height == 0 ||
			Width > Texture::MAXIMUM_DIMENSION || Height > Texture::MAXIMUM_DIMENSION)
			return {};
		const uint64_t bytes = FrameBytes(*this);
		const uint64_t end = (uint64_t(index) + 1) * bytes;
		if (bytes > TextureSequence::MAXIMUM_PIXEL_BYTES || end > Pixels.size()) return {};
		return std::span(Pixels).subspan(static_cast<size_t>(end - bytes), static_cast<size_t>(bytes));
	}

	bool TextureSequence::Write(core::ByteWriter &writer, const TextureSequenceData &data) {
		if (!data.IsValid()) return false;
		writer.WriteUInt32(MAGIC);
		writer.WriteUInt16(VERSION);
		writer.WriteUInt8(static_cast<uint8_t>(data.Format));
		writer.WriteUInt32(data.Width);
		writer.WriteUInt32(data.Height);
		writer.WriteUInt32(static_cast<uint32_t>(data.FrameDurations.size()));
		for (float duration : data.FrameDurations)
			writer.WriteFloat(duration);
		writer.WriteRaw(data.Pixels.data(), data.Pixels.size());
		return true;
	}

	bool TextureSequence::Read(core::ByteReader &reader, TextureSequenceData &out) {
		if (reader.ReadUInt32() != MAGIC || reader.ReadUInt16() != VERSION) return false;
		TextureSequenceData decoded;
		const uint8_t format = reader.ReadUInt8();
		if (format != static_cast<uint8_t>(TextureFormat::RGBA8) &&
			format != static_cast<uint8_t>(TextureFormat::RGBA8_LINEAR))
			return false;
		decoded.Format = static_cast<TextureFormat>(format);
		decoded.Width = reader.ReadUInt32();
		decoded.Height = reader.ReadUInt32();
		const uint32_t frames = reader.ReadUInt32();
		if (reader.Failed() || decoded.Width == 0 || decoded.Height == 0 ||
			decoded.Width > Texture::MAXIMUM_DIMENSION || decoded.Height > Texture::MAXIMUM_DIMENSION ||
			frames == 0 || frames > MAXIMUM_FRAMES)
			return false;
		const uint64_t frameBytes = FrameBytes(decoded);
		const uint64_t pixelBytes = frameBytes * frames;
		const uint64_t payloadBytes = uint64_t(frames) * sizeof(float) + pixelBytes;
		if (pixelBytes > MAXIMUM_PIXEL_BYTES || payloadBytes != reader.Remaining()) return false;
		decoded.FrameDurations.reserve(frames);
		for (uint32_t frame = 0; frame < frames; ++frame)
			decoded.FrameDurations.push_back(reader.ReadFloat());
		if (reader.Failed()) return false;
		const auto pixels = reader.ReadRawView(static_cast<size_t>(pixelBytes));
		if (reader.Failed() || !reader.AtEnd()) return false;
		decoded.Pixels.assign(pixels.begin(), pixels.end());
		if (!decoded.IsValid()) return false;
		out = std::move(decoded);
		return true;
	}
}
