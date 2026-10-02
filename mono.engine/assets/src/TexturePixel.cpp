#include <engine/assets/TexturePixel.hpp>
#include <engine/core/Float16.hpp>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>

namespace engine::assets {
	namespace {
		struct PixelLayout {
			uint8_t Channels;
			uint8_t BitsPerChannel;
			uint8_t BytesPerPixel;
			bool FloatingPoint;
		};

		std::optional<PixelLayout> Layout(TextureFormat format) noexcept {
			switch (format) {
			case TextureFormat::R8:
				return PixelLayout{1, 8, 1, false};
			case TextureFormat::R16_FLOAT:
				return PixelLayout{1, 16, 2, true};
			case TextureFormat::R32_FLOAT:
				return PixelLayout{1, 32, 4, true};
			case TextureFormat::RGBA4_UNORM:
			case TextureFormat::RGBA4_SRGB:
				return PixelLayout{4, 4, 2, false};
			case TextureFormat::RGBA8:
			case TextureFormat::RGBA8_LINEAR:
				return PixelLayout{4, 8, 4, false};
			case TextureFormat::RGBA16_FLOAT:
				return PixelLayout{4, 16, 8, true};
			case TextureFormat::RGBA32_FLOAT:
				return PixelLayout{4, 32, 16, true};
			}
			return std::nullopt;
		}

		uint32_t ReadWord(const std::byte *bytes, uint8_t count) noexcept {
			uint32_t value = 0;
			for (uint8_t index = 0; index < count; index++)
				value |= uint32_t(std::to_integer<uint8_t>(bytes[index])) << (index * 8);
			return value;
		}

		void WriteWord(std::byte *bytes, uint32_t value, uint8_t count) noexcept {
			for (uint8_t index = 0; index < count; index++)
				bytes[index] = std::byte(value >> (index * 8));
		}

		double DisplayClamp(double value) noexcept {
			if (std::isnan(value)) return 0.0;
			return std::clamp(value, 0.0, 1.0);
		}
	} // namespace

	bool
	LoadTexturePixel(TextureFormat format, std::span<const std::byte> bytes, TexturePixel &pixel) noexcept {
		const auto layout = Layout(format);
		if (!layout || bytes.size() != layout->BytesPerPixel) return false;
		TexturePixel result{0.0, 0.0, 0.0, 1.0};
		if (layout->BitsPerChannel == 4) {
			const uint32_t packed = ReadWord(bytes.data(), 2);
			for (uint8_t channel = 0; channel < 4; channel++)
				result[channel] = double((packed >> (channel * 4)) & 15u) / 15.0;
		} else if (!layout->FloatingPoint && layout->BitsPerChannel == 8) {
			for (uint8_t channel = 0; channel < layout->Channels; channel++)
				result[channel] = double(std::to_integer<uint8_t>(bytes[channel])) / 255.0;
		} else if (layout->BitsPerChannel == 16) {
			for (uint8_t channel = 0; channel < layout->Channels; channel++)
				result[channel] = core::DecodeFloat16(uint16_t(ReadWord(bytes.data() + channel * 2, 2)));
		} else if (layout->BitsPerChannel == 32) {
			for (uint8_t channel = 0; channel < layout->Channels; channel++)
				result[channel] = std::bit_cast<float>(ReadWord(bytes.data() + channel * 4, 4));
		} else {
			return false;
		}
		pixel = result;
		return true;
	}

	bool
	StoreTexturePixel(TextureFormat format, const TexturePixel &pixel, std::span<std::byte> bytes) noexcept {
		const auto layout = Layout(format);
		if (!layout || bytes.size() != layout->BytesPerPixel) return false;
		for (uint8_t channel = 0; channel < layout->Channels; channel++) {
			if (!std::isfinite(pixel[channel])) return false;
			if (layout->FloatingPoint &&
				std::abs(pixel[channel]) >
					(layout->BitsPerChannel == 16 ? 65504.0 : double(std::numeric_limits<float>::max())))
				return false;
		}

		std::array<std::byte, 16> encoded{};
		if (layout->BitsPerChannel == 4) {
			uint16_t packed = 0;
			for (uint8_t channel = 0; channel < 4; channel++)
				packed |= uint16_t(std::lround(DisplayClamp(pixel[channel]) * 15.0)) << (channel * 4);
			WriteWord(encoded.data(), packed, 2);
		} else if (!layout->FloatingPoint) {
			for (uint8_t channel = 0; channel < layout->Channels; channel++)
				encoded[channel] = std::byte(uint8_t(std::lround(DisplayClamp(pixel[channel]) * 255.0)));
		} else if (layout->BitsPerChannel == 16) {
			for (uint8_t channel = 0; channel < layout->Channels; channel++)
				WriteWord(encoded.data() + channel * 2, core::EncodeFloat16(float(pixel[channel])), 2);
		} else if (layout->BitsPerChannel == 32) {
			for (uint8_t channel = 0; channel < layout->Channels; channel++)
				WriteWord(encoded.data() + channel * 4, std::bit_cast<uint32_t>(float(pixel[channel])), 4);
		} else {
			return false;
		}
		std::copy_n(encoded.data(), layout->BytesPerPixel, bytes.data());
		return true;
	}

	bool LoadTexturePixelForDisplay(
		TextureFormat format, std::span<const std::byte> bytes, std::array<float, 4> &rgba
	) noexcept {
		const auto layout = Layout(format);
		TexturePixel pixel{};
		if (!layout || !LoadTexturePixel(format, bytes, pixel)) return false;
		std::array<float, 4> result{};
		if (layout->Channels == 1) {
			const float gray = float(DisplayClamp(pixel[0]));
			result = {gray, gray, gray, 1.0f};
		} else {
			for (size_t channel = 0; channel < result.size(); channel++)
				result[channel] = float(DisplayClamp(pixel[channel]));
		}
		rgba = result;
		return true;
	}
} // namespace engine::assets
