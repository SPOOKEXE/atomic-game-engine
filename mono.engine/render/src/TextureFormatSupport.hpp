#pragma once

#include <engine/assets/Texture.hpp>

#include <SDL3/SDL_gpu.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <optional>
#include <span>

namespace engine::render::detail {
	struct TextureFormatSupport {
		SDL_GPUTextureFormat DeviceFormat;
		uint32_t UploadBytesPerPixel;
		bool ExpandRedToRgba;
		bool ExpandRgba4ToRgba8;
	};

	inline std::optional<TextureFormatSupport> TextureFormatForUpload(assets::TextureFormat format) {
		switch (format) {
		case assets::TextureFormat::RGBA8:
			return TextureFormatSupport{SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM_SRGB, 4, false, false};
		case assets::TextureFormat::RGBA8_LINEAR:
			return TextureFormatSupport{SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM, 4, false, false};
		case assets::TextureFormat::R8:
			return TextureFormatSupport{SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM, 4, true, false};
		case assets::TextureFormat::RGBA4_UNORM:
			return TextureFormatSupport{SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM, 4, false, true};
		case assets::TextureFormat::RGBA4_SRGB:
			return TextureFormatSupport{SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM_SRGB, 4, false, true};
		case assets::TextureFormat::RGBA16_FLOAT:
			return TextureFormatSupport{SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT, 8, false, false};
		case assets::TextureFormat::RGBA32_FLOAT:
			return TextureFormatSupport{SDL_GPU_TEXTUREFORMAT_R32G32B32A32_FLOAT, 16, false, false};
		case assets::TextureFormat::R16_FLOAT:
			return TextureFormatSupport{SDL_GPU_TEXTUREFORMAT_R16_FLOAT, 2, false, false};
		case assets::TextureFormat::R32_FLOAT:
			return TextureFormatSupport{SDL_GPU_TEXTUREFORMAT_R32_FLOAT, 4, false, false};
		}
		return std::nullopt;
	}

	inline bool SupportsTextureFormat(SDL_GPUDevice *device, assets::TextureFormat format) {
		const auto support = TextureFormatForUpload(format);
		return device != nullptr && support.has_value() &&
			   SDL_GPUTextureSupportsFormat(
				   device, support->DeviceFormat, SDL_GPU_TEXTURETYPE_2D, SDL_GPU_TEXTUREUSAGE_SAMPLER
			   );
	}

	inline bool CopyPixelsForUpload(
		assets::TextureFormat format, std::span<const std::byte> source, std::span<std::byte> destination
	) {
		const auto support = TextureFormatForUpload(format);
		const uint32_t sourceBytesPerPixel = assets::BytesPerPixel(format);
		const size_t pixels = sourceBytesPerPixel == 0 ? 0 : source.size() / sourceBytesPerPixel;
		if (!support || sourceBytesPerPixel == 0 || source.size() % sourceBytesPerPixel != 0 ||
			pixels > std::numeric_limits<size_t>::max() / support->UploadBytesPerPixel ||
			destination.size() != pixels * support->UploadBytesPerPixel)
			return false;
		if (source.empty()) return true;
		if (support->ExpandRedToRgba) {
			for (size_t index = 0; index < source.size(); index++) {
				destination[index * 4] = source[index];
				destination[index * 4 + 1] = source[index];
				destination[index * 4 + 2] = source[index];
				destination[index * 4 + 3] = std::byte{255};
			}
			return true;
		}
		if (support->ExpandRgba4ToRgba8) {
			for (size_t pixel = 0; pixel < source.size() / 2; pixel++) {
				const uint16_t packed = std::to_integer<uint8_t>(source[pixel * 2]) |
										(uint16_t(std::to_integer<uint8_t>(source[pixel * 2 + 1])) << 8);
				for (uint32_t channel = 0; channel < 4; channel++) {
					const uint8_t nibble = static_cast<uint8_t>((packed >> (channel * 4)) & 15u);
					destination[pixel * 4 + channel] = std::byte{static_cast<uint8_t>(nibble * 17u)};
				}
			}
			return true;
		}
		if (source.size() != destination.size()) return false;
		std::memcpy(destination.data(), source.data(), source.size());
		return true;
	}

	inline bool CopyRgba8ToRgba4(std::span<const std::byte> source, std::span<std::byte> destination) {
		if (source.size() % 4 != 0 || destination.size() != source.size() / 2) return false;
		for (size_t pixel = 0; pixel < source.size() / 4; pixel++) {
			uint16_t packed = 0;
			for (uint32_t channel = 0; channel < 4; channel++) {
				const uint32_t value = std::to_integer<uint8_t>(source[pixel * 4 + channel]);
				const uint32_t nibble = (value * 15u + 127u) / 255u;
				packed |= static_cast<uint16_t>(nibble << (channel * 4));
			}
			destination[pixel * 2] = std::byte{static_cast<uint8_t>(packed & 255u)};
			destination[pixel * 2 + 1] = std::byte{static_cast<uint8_t>(packed >> 8)};
		}
		return true;
	}
}
