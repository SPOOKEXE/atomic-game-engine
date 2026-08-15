#include <engine/assets/Resample.hpp>
#include <engine/render/DefaultTexture.hpp>

#include <cstdint>

namespace engine::render {

	namespace {
		// How wide the compiled-in tile is. Square.
		constexpr uint32_t TILE_SIDE = 64;

		// One channel of ambientCG Plastic 013 A's colour map, box-filtered from
		// 1024 to 64. See `DefaultTexture.hpp` for the provenance and for why it
		// is one channel.
		constexpr uint8_t TILE[] = {
#include "DefaultTexture.inl"
		};

		static_assert(sizeof(TILE) == TILE_SIDE * TILE_SIDE, "the tile is not 64x64");
	}

	const assets::TextureData &DefaultTexture() {
		static const assets::TextureData image = [] {
			assets::TextureData built;
			built.Width = TILE_SIDE;
			built.Height = TILE_SIDE;
			built.Format = assets::TextureFormat::R8;
			built.Pixels.resize(sizeof(TILE));
			for (size_t texel = 0; texel < sizeof(TILE); texel++) {
				built.Pixels[texel] = static_cast<std::byte>(TILE[texel]);
			}

			// **Filtered here rather than shipped as a chain in the `.inl`.** The
			// tile is compiled in because it has to exist before any content has
			// streamed, and six more levels of it would be a third more source
			// bytes for something derivable in microseconds once, at first use.
			// `assets::BuildMipChain` is one tier below this module; it was
			// `bake`'s until v0.15, and `bake` is what a shipped game may not link.
			assets::BuildMipChain(built);
			return built;
		}();
		return image;
	}
}
