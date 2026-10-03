#pragma once

#include "TextureFormatSupport.hpp"

#include <engine/render/ImageGraphTransform3D.hpp>

#include <SDL3/SDL_gpu.h>

namespace engine::render::imagegraph::detail {
	inline bool ValidTransformImage3DColorSpace(TransformImage3DColorSpace colorSpace) {
		return colorSpace == TransformImage3DColorSpace::Linear ||
			   colorSpace == TransformImage3DColorSpace::Display;
	}

	inline assets::TextureFormat
	ResolveTransformImage3DFormat(assets::TextureFormat format, TransformImage3DColorSpace colorSpace) {
		if (format == assets::TextureFormat::RGBA8)
			return colorSpace == TransformImage3DColorSpace::Display ? assets::TextureFormat::RGBA8
																	 : assets::TextureFormat::RGBA8_LINEAR;
		if (format == assets::TextureFormat::RGBA4_UNORM)
			return colorSpace == TransformImage3DColorSpace::Display ? assets::TextureFormat::RGBA4_SRGB
																	 : assets::TextureFormat::RGBA4_UNORM;
		if (format == assets::TextureFormat::RGBA4_SRGB)
			return colorSpace == TransformImage3DColorSpace::Display ? assets::TextureFormat::RGBA4_SRGB
																	 : assets::TextureFormat::RGBA4_UNORM;
		return format;
	}

	inline SDL_GPUTextureFormat TransformImage3DColourFormat(TransformImage3DColorSpace colorSpace) {
		return colorSpace == TransformImage3DColorSpace::Display ? SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM_SRGB
																 : SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
	}

	inline std::optional<SDL_GPUTextureFormat>
	TransformImage3DFormat(assets::TextureFormat format, TransformImage3DColorSpace colorSpace) {
		const assets::TextureFormat resolved = ResolveTransformImage3DFormat(format, colorSpace);
		switch (resolved) {
		case assets::TextureFormat::RGBA8:
		case assets::TextureFormat::RGBA4_SRGB:
			return SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM_SRGB;
		case assets::TextureFormat::RGBA8_LINEAR:
		case assets::TextureFormat::RGBA4_UNORM:
			return SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
		case assets::TextureFormat::R8:
			return SDL_GPU_TEXTUREFORMAT_R8_UNORM;
		case assets::TextureFormat::R16_FLOAT:
			return SDL_GPU_TEXTUREFORMAT_R16_FLOAT;
		case assets::TextureFormat::R32_FLOAT:
			return SDL_GPU_TEXTUREFORMAT_R32_FLOAT;
		case assets::TextureFormat::RGBA16_FLOAT:
			return SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT;
		case assets::TextureFormat::RGBA32_FLOAT:
			return SDL_GPU_TEXTUREFORMAT_R32G32B32A32_FLOAT;
		}
		return std::nullopt;
	}

	inline uint32_t
	TransformImage3DBytesPerPixel(assets::TextureFormat format, TransformImage3DColorSpace colorSpace) {
		const auto deviceFormat = TransformImage3DFormat(format, colorSpace);
		if (!deviceFormat) return 0;
		return format == assets::TextureFormat::RGBA4_UNORM || format == assets::TextureFormat::RGBA4_SRGB
				   ? 4u
				   : assets::BytesPerPixel(format);
	}

	inline bool SupportsTransformImage3DFormats(
		SDL_GPUDevice *device,
		assets::TextureFormat front,
		assets::TextureFormat back,
		bool hasBack,
		TransformImage3DColorSpace colorSpace,
		bool sourcePlane = false
	) {
		if (device == nullptr || !ValidTransformImage3DColorSpace(colorSpace)) return false;
		const auto frontFormat = TransformImage3DFormat(front, colorSpace);
		const auto backFormat = TransformImage3DFormat(hasBack ? back : front, colorSpace);
		if (!frontFormat || !backFormat) return false;
		const auto supports = [device](SDL_GPUTextureFormat format, SDL_GPUTextureUsageFlags usage) {
			return SDL_GPUTextureSupportsFormat(device, format, SDL_GPU_TEXTURETYPE_2D, usage);
		};
		return supports(*frontFormat, SDL_GPU_TEXTUREUSAGE_SAMPLER) &&
			   (!hasBack || supports(*backFormat, SDL_GPU_TEXTUREUSAGE_SAMPLER)) &&
			   supports(
				   sourcePlane ? SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM : *frontFormat,
				   SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER
			   ) &&
			   supports(SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM, SDL_GPU_TEXTUREUSAGE_COLOR_TARGET) &&
			   supports(SDL_GPU_TEXTUREFORMAT_D32_FLOAT, SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET);
	}

	inline bool
	SupportsTransformImage3DColorSpace(SDL_GPUDevice *device, TransformImage3DColorSpace colorSpace) {
		return SupportsTransformImage3DFormats(
			device, assets::TextureFormat::RGBA8, assets::TextureFormat::RGBA8, false, colorSpace
		);
	}
}
