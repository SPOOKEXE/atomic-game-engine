#pragma once
#include <engine/assets/Texture.hpp>

#include <optional>
#include <string>
namespace engine::bake {
	struct GameMakerResource {
		// Exact durable path from a YY reference. Bytes are explicitly supplied by the host.
		std::string Key;
		std::span<const std::byte> Bytes;
	};
	struct GameMakerTileOverride {
		std::string LayerName;
		std::vector<uint32_t> Data;
		std::string TilesetKey;
		std::optional<assets::TextureData> Preview;
	};
	// Reads a room preview and its explicitly supplied YY/PNG dependencies without opening paths.
	// Instance, asset, background, nested and tile layers follow the pinned source thumbnail renderer.
	bool ReadGameMakerRoom(
		std::span<const std::byte> room,
		std::span<const GameMakerResource> resources,
		assets::TextureData &out,
		std::string &failure,
		uint64_t maximumBytes = 64ull * 1024 * 1024,
		std::span<const GameMakerTileOverride> overrides = {}
	);
}
