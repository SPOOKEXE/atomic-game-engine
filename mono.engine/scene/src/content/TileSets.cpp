#include <engine/scene/content/TileSets.hpp>

#include <algorithm>
#include <charconv>

namespace engine::scene {
	namespace {
		constexpr std::string_view PREFIX = "udim://";
		constexpr std::string_view EXPLICIT = "udim:";
	}

	bool IsTileSet(std::string_view value) noexcept {
		return value.starts_with(EXPLICIT);
	}

	std::vector<UdimTile> ExpandTileSet(std::string_view value) {
		std::vector<UdimTile> tiles;
		if (value.starts_with(PREFIX)) {
			const std::string prefix(value.substr(PREFIX.size()));
			if (prefix.empty()) return {};
			for (uint16_t tile = UDIM_FIRST_TILE; tile <= UDIM_PREFIX_LAST_TILE; ++tile)
				tiles.push_back({tile, core::Name(prefix + "." + std::to_string(tile))});
			return tiles;
		}
		if (!value.starts_with(EXPLICIT)) return {};
		value.remove_prefix(EXPLICIT.size());
		while (!value.empty()) {
			const size_t end = std::min(value.find('|'), value.size());
			const std::string_view entry = value.substr(0, end);
			const size_t equals = entry.find('=');
			uint16_t tile = 0;
			if (equals == std::string_view::npos || equals + 1 >= entry.size() ||
				std::from_chars(entry.data(), entry.data() + equals, tile).ptr != entry.data() + equals ||
				tile < UDIM_FIRST_TILE || tile > UDIM_LAST_TILE)
				return {};
			tiles.push_back({tile, core::Name(entry.substr(equals + 1))});
			value.remove_prefix(std::min(end + 1, value.size()));
		}
		std::sort(tiles.begin(), tiles.end(), [](const UdimTile &a, const UdimTile &b) {
			return a.Tile < b.Tile;
		});
		if (std::adjacent_find(tiles.begin(), tiles.end(), [](const UdimTile &a, const UdimTile &b) {
				return a.Tile == b.Tile;
			}) != tiles.end())
			return {};
		return tiles;
	}

	std::string ComposeTileSet(std::span<const UdimTile> tiles) {
		std::vector<UdimTile> sorted(tiles.begin(), tiles.end());
		std::sort(sorted.begin(), sorted.end(), [](const UdimTile &a, const UdimTile &b) {
			return a.Tile < b.Tile;
		});
		std::string out(EXPLICIT);
		for (size_t index = 0; index < sorted.size(); ++index) {
			const auto &tile = sorted[index];
			const std::string_view text = tile.Texture.Text();
			if (tile.Tile < UDIM_FIRST_TILE || tile.Tile > UDIM_LAST_TILE || text.empty() ||
				text.find('|') != std::string_view::npos ||
				(index > 0 && sorted[index - 1].Tile == tile.Tile))
				return {};
			if (index > 0) out += '|';
			out += std::to_string(tile.Tile) + "=" + std::string(text);
		}
		return sorted.empty() ? std::string() : out;
	}

	namespace {
		constexpr std::string_view LAYERS = "layers:";
	}

	bool IsLayerSet(std::string_view value) noexcept {
		return value.starts_with(LAYERS);
	}

	std::vector<core::Name> ExpandLayerSet(std::string_view value) {
		std::vector<core::Name> layers;
		if (!value.starts_with(LAYERS)) return layers;
		value.remove_prefix(LAYERS.size());
		while (!value.empty()) {
			const size_t bar = std::min(value.find('|'), value.size());
			const std::string_view name = value.substr(0, bar);
			if (name.empty() || layers.size() >= MAX_TEXTURE_LAYERS) return {};
			layers.emplace_back(name);
			value = bar < value.size() ? value.substr(bar + 1) : std::string_view{};
		}
		return layers;
	}

	std::string ComposeLayerSet(std::span<const core::Name> layers) {
		if (layers.empty() || layers.size() > MAX_TEXTURE_LAYERS) return {};
		std::string out(LAYERS);
		for (size_t index = 0; index < layers.size(); ++index) {
			const std::string_view text = layers[index].Text();
			if (text.empty() || text.find('|') != std::string_view::npos) return {};
			if (index > 0) out += '|';
			out += text;
		}
		return out;
	}
}
