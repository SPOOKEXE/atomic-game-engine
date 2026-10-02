#pragma once

#include <engine/assets/Texture.hpp>
#include <engine/imagegraph/Document.hpp>

#include <optional>

namespace client::detail {
	inline std::optional<engine::assets::TextureFormat>
	TextureFormatForSurface(engine::imagegraph::SurfaceFormat format) {
		using engine::assets::TextureFormat;
		using engine::imagegraph::SurfaceFormat;
		switch (format) {
		case SurfaceFormat::RGBA8Unorm:
			return TextureFormat::RGBA8;
		case SurfaceFormat::RGBA4Unorm:
			return TextureFormat::RGBA4_UNORM;
		case SurfaceFormat::RGBA16Float:
			return TextureFormat::RGBA16_FLOAT;
		case SurfaceFormat::RGBA32Float:
			return TextureFormat::RGBA32_FLOAT;
		case SurfaceFormat::R8Unorm:
			return TextureFormat::R8;
		case SurfaceFormat::R16Float:
			return TextureFormat::R16_FLOAT;
		case SurfaceFormat::R32Float:
			return TextureFormat::R32_FLOAT;
		}
		return std::nullopt;
	}

	inline std::optional<engine::imagegraph::SurfaceFormat>
	SurfaceFormatForTexture(engine::assets::TextureFormat format) {
		using engine::assets::TextureFormat;
		using engine::imagegraph::SurfaceFormat;
		switch (format) {
		case TextureFormat::RGBA8:
		case TextureFormat::RGBA8_LINEAR:
			return SurfaceFormat::RGBA8Unorm;
		case TextureFormat::RGBA4_UNORM:
		case TextureFormat::RGBA4_SRGB:
			return SurfaceFormat::RGBA4Unorm;
		case TextureFormat::RGBA16_FLOAT:
			return SurfaceFormat::RGBA16Float;
		case TextureFormat::RGBA32_FLOAT:
			return SurfaceFormat::RGBA32Float;
		case TextureFormat::R8:
			return SurfaceFormat::R8Unorm;
		case TextureFormat::R16_FLOAT:
			return SurfaceFormat::R16Float;
		case TextureFormat::R32_FLOAT:
			return SurfaceFormat::R32Float;
		}
		return std::nullopt;
	}

	inline uint32_t TextureUploadBytesPerPixel(engine::imagegraph::SurfaceFormat format) {
		using engine::imagegraph::SurfaceFormat;
		if (format == SurfaceFormat::RGBA4Unorm || format == SurfaceFormat::R8Unorm) return 4;
		const auto info = engine::imagegraph::DescribeSurfaceFormat(format);
		return info ? info->BytesPerPixel : 0;
	}
}
