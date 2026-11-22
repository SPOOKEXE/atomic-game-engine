#pragma once

// UDIM texture sets named by one texture slot.
//
// A `SurfaceAppearance` slot holds one content name. A UDIM set is still one
// name, so it replicates, saves and compares like any other slot value:
//
// - `udim://<prefix>` names tiles `<prefix>.1001` to `<prefix>.1020`.
// - `udim:1001=<content>|1002=<content>|...` names each tile explicitly, which
//   is what `MeshPart:SetTileTextures` writes.
//
// The renderer draws such a slot once per tile, and tile 1001 + u + 10 * v
// covers UV [u, u + 1) x [v, v + 1).
//
// @tier L7 · shared

#include <engine/core/Name.hpp>

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace engine::scene {
	// First tile number.
	inline constexpr uint16_t UDIM_FIRST_TILE = 1001;
	// Highest tile a prefix set probes; explicit sets may name tiles up to 1100.
	inline constexpr uint16_t UDIM_PREFIX_LAST_TILE = 1020;
	inline constexpr uint16_t UDIM_LAST_TILE = 1100;

	// One tile of a set.
	struct UdimTile {
		uint16_t Tile = UDIM_FIRST_TILE;
		core::Name Texture;
		bool operator==(const UdimTile &) const = default;
	};

	// Whether a slot value names a UDIM set rather than one texture.
	bool IsTileSet(std::string_view value) noexcept;

	// The tiles a set names, sorted by tile; empty for a plain name or a malformed set.
	std::vector<UdimTile> ExpandTileSet(std::string_view value);

	// Writes an explicit set. Refuses tiles outside 1001..1100, duplicates, empty
	// names and names holding `|`, returning an empty string.
	std::string ComposeTileSet(std::span<const UdimTile> tiles);
}
