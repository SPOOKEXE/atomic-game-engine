#include "Decoders.hpp"

#include <engine/imagecodec/Image.hpp>

namespace engine::bake {
	namespace {
		bool DecodeShared(
			std::span<const std::byte> bytes,
			imagecodec::Format format,
			assets::TextureData &out,
			std::string &failure
		) {
			// grug keep legacy bake texture bounds. runtime uses smaller defaults.
			const imagecodec::Limits limits{
				assets::Texture::MAXIMUM_DIMENSION,
				assets::Texture::MAXIMUM_DIMENSION,
				128 * 1024 * 1024,
				static_cast<size_t>(assets::Texture::MAXIMUM_DIMENSION) * assets::Texture::MAXIMUM_DIMENSION *
					4
			};
			imagecodec::Image decoded;
			if (!imagecodec::Decode(bytes, format, decoded, failure, limits)) return false;
			assets::TextureData texture;
			texture.Width = decoded.Width;
			texture.Height = decoded.Height;
			texture.Format = assets::TextureFormat::RGBA8;
			texture.Pixels = std::move(decoded.Pixels);
			out = std::move(texture);
			return true;
		}
	}
	bool ReadPng(std::span<const std::byte> bytes, assets::TextureData &out, std::string &failure) {
		return DecodeShared(bytes, imagecodec::Format::Png, out, failure);
	}
	bool ReadJpeg(std::span<const std::byte> bytes, assets::TextureData &out, std::string &failure) {
		return DecodeShared(bytes, imagecodec::Format::Jpeg, out, failure);
	}
}
