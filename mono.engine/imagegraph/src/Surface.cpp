#include <engine/core/Float16.hpp>
#include <engine/imagegraph/Surface.hpp>

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>

namespace engine::imagegraph {
	std::optional<SurfaceFormatInfo> DescribeSurfaceFormat(SurfaceFormat format) noexcept {
		switch (format) {
		case SurfaceFormat::RGBA8Unorm:
			return SurfaceFormatInfo{"rgba8_unorm", 4, 8, 4, false};
		case SurfaceFormat::RGBA4Unorm:
			return SurfaceFormatInfo{"rgba4_unorm", 4, 4, 2, false};
		case SurfaceFormat::RGBA16Float:
			return SurfaceFormatInfo{"rgba16_float", 4, 16, 8, true};
		case SurfaceFormat::RGBA32Float:
			return SurfaceFormatInfo{"rgba32_float", 4, 32, 16, true};
		case SurfaceFormat::R8Unorm:
			return SurfaceFormatInfo{"r8_unorm", 1, 8, 1, false};
		case SurfaceFormat::R16Float:
			return SurfaceFormatInfo{"r16_float", 1, 16, 2, true};
		case SurfaceFormat::R32Float:
			return SurfaceFormatInfo{"r32_float", 1, 32, 4, true};
		}
		return std::nullopt;
	}
	std::optional<SurfaceFormat> SourceSurfaceFormat(int64_t choice) noexcept {
		static constexpr SurfaceFormat formats[]{
			SurfaceFormat::RGBA4Unorm,
			SurfaceFormat::RGBA8Unorm,
			SurfaceFormat::RGBA16Float,
			SurfaceFormat::RGBA32Float,
			SurfaceFormat::R8Unorm,
			SurfaceFormat::R16Float,
			SurfaceFormat::R32Float
		};
		if (choice < 2 || choice > 8) return std::nullopt;
		return formats[choice - 2];
	}
	std::optional<SurfaceLayout> CheckedSurfaceLayout(
		uint32_t width, uint32_t height, SurfaceFormat format, uint64_t maximumBytes
	) noexcept {
		const auto info = DescribeSurfaceFormat(format);
		if (!info || width == 0 || height == 0) return std::nullopt;
		const uint64_t row = uint64_t(width) * info->BytesPerPixel;
		if (row > maximumBytes / height) return std::nullopt;
		return SurfaceLayout{row, row * height};
	}
	bool ValidSurfaceLayout(const Image &image, uint32_t maximumDimension, uint64_t maximumBytes) noexcept {
		const auto layout = CheckedSurfaceLayout(image.Width, image.Height, image.Format, maximumBytes);
		return image.Width <= maximumDimension && image.Height <= maximumDimension && layout &&
			   layout->Bytes == image.Pixels.size();
	}
	namespace {
		uint32_t Word(const uint8_t *bytes, uint8_t count) noexcept {
			uint32_t result = 0;
			for (uint8_t i = 0; i < count; ++i)
				result |= uint32_t(bytes[i]) << (i * 8);
			return result;
		}
		void PutWord(uint8_t *bytes, uint32_t word, uint8_t count) noexcept {
			for (uint8_t i = 0; i < count; ++i)
				bytes[i] = uint8_t(word >> (i * 8));
		}
		std::optional<size_t> PixelOffset(const Image &image, uint32_t x, uint32_t y) noexcept {
			const auto layout =
				CheckedSurfaceLayout(image.Width, image.Height, image.Format, image.Pixels.size());
			if (!layout || layout->Bytes != image.Pixels.size() || x >= image.Width || y >= image.Height)
				return std::nullopt;
			return size_t(
				y * layout->RowBytes + uint64_t(x) * DescribeSurfaceFormat(image.Format)->BytesPerPixel
			);
		}
	}
	uint16_t EncodeHalf(float value) noexcept {
		return core::EncodeFloat16(value);
	}
	float DecodeHalf(uint16_t bits) noexcept {
		return core::DecodeFloat16(bits);
	}
	bool LoadSurfacePixel(const Image &image, uint32_t x, uint32_t y, SurfacePixel &pixel) noexcept {
		const auto offset = PixelOffset(image, x, y);
		if (!offset) return false;
		const auto info = *DescribeSurfaceFormat(image.Format);
		SurfacePixel result{0, 0, 0, 1};
		const uint8_t *bytes = image.Pixels.data() + *offset;
		for (uint8_t channel = 0; channel < info.Channels; ++channel) {
			if (info.BitsPerChannel == 4)
				result[channel] = ((Word(bytes, 2) >> (channel * 4)) & 15) / 15.0;
			else if (!info.FloatingPoint)
				result[channel] = bytes[channel] / 255.0;
			else if (info.BitsPerChannel == 16)
				result[channel] = DecodeHalf(uint16_t(Word(bytes + channel * 2, 2)));
			else
				result[channel] = std::bit_cast<float>(Word(bytes + channel * 4, 4));
		}
		pixel = result;
		return true;
	}
	bool StoreSurfacePixel(Image &image, uint32_t x, uint32_t y, const SurfacePixel &pixel) noexcept {
		const auto offset = PixelOffset(image, x, y);
		if (!offset) return false;
		const auto info = *DescribeSurfaceFormat(image.Format);
		for (uint8_t channel = 0; channel < info.Channels; ++channel) {
			if (!std::isfinite(pixel[channel])) return false;
			if (info.FloatingPoint &&
				std::abs(pixel[channel]) >
					(info.BitsPerChannel == 16 ? 65504.0 : double(std::numeric_limits<float>::max())))
				return false;
		}
		std::array<uint8_t, 16> encoded{};
		uint16_t packed = 0;
		for (uint8_t channel = 0; channel < info.Channels; ++channel) {
			if (info.BitsPerChannel == 4)
				packed |= uint16_t(std::lround(std::clamp(pixel[channel], 0.0, 1.0) * 15)) << (channel * 4);
			else if (!info.FloatingPoint)
				encoded[channel] = uint8_t(std::lround(std::clamp(pixel[channel], 0.0, 1.0) * 255));
			else if (info.BitsPerChannel == 16)
				PutWord(encoded.data() + channel * 2, EncodeHalf(float(pixel[channel])), 2);
			else
				PutWord(encoded.data() + channel * 4, std::bit_cast<uint32_t>(float(pixel[channel])), 4);
		}
		if (info.BitsPerChannel == 4) PutWord(encoded.data(), packed, 2);
		std::copy_n(encoded.data(), info.BytesPerPixel, image.Pixels.data() + *offset);
		return true;
	}
	bool FiniteSurfaceSamples(const Image &image) noexcept {
		const auto info = DescribeSurfaceFormat(image.Format);
		const auto layout =
			CheckedSurfaceLayout(image.Width, image.Height, image.Format, image.Pixels.size());
		if (!info || !layout || layout->Bytes != image.Pixels.size()) return false;
		if (!info->FloatingPoint) return true;
		const uint8_t step = info->BitsPerChannel / 8;
		for (size_t offset = 0; offset < image.Pixels.size(); offset += step) {
			const uint32_t bits = Word(image.Pixels.data() + offset, step);
			if (step == 2 ? (bits & 0x7c00) == 0x7c00 : (bits & 0x7f800000) == 0x7f800000) return false;
		}
		return true;
	}
	uint64_t SurfaceHash(const Image &image) noexcept {
		uint64_t hash = 14695981039346656037ull;
		const auto append = [&](uint8_t byte) {
			hash ^= byte;
			hash *= 1099511628211ull;
		};
		if (image.Format != SurfaceFormat::RGBA8Unorm) {
			const auto info = DescribeSurfaceFormat(image.Format);
			if (info)
				for (char character : info->Name)
					append(uint8_t(character));
			append(0);
			for (uint32_t dimension : {image.Width, image.Height})
				for (uint8_t shift = 0; shift < 32; shift += 8)
					append(uint8_t(dimension >> shift));
		}
		for (uint8_t byte : image.Pixels)
			append(byte);
		return hash;
	}
}
