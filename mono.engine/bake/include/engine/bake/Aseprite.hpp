#pragma once

#include <engine/assets/Texture.hpp>

#include <array>
#include <string>

namespace engine::bake {
	struct AsepriteLayer {
		std::string Name;
		uint16_t Type = 0, Depth = 0, Blend = 0;
		uint8_t Opacity = 255;
		bool Visible = true;
		uint32_t Tileset = 0;
	};
	struct AsepriteCel {
		uint16_t Layer = 0;
		int16_t X = 0, Y = 0, Z = 0;
		uint8_t Opacity = 255;
		assets::TextureData Pixels;
	};
	struct AsepriteFrame {
		uint16_t Duration = 0;
		std::vector<AsepriteCel> Cels;
	};
	struct AsepriteTag {
		std::string Name;
		uint16_t First = 0, Last = 0, Repeat = 0;
		uint8_t Direction = 0;
	};
	struct AsepriteTileset {
		std::string Name;
		uint32_t Id = 0, Flags = 0, Count = 0;
		uint16_t Width = 0, Height = 0;
		int16_t Base = 0;
		assets::TextureData Pixels;
	};
	struct AsepriteDocument {
		uint16_t Width = 0, Height = 0;
		std::vector<AsepriteLayer> Layers;
		std::vector<AsepriteFrame> Frames;
		std::vector<AsepriteTag> Tags;
		std::vector<AsepriteTileset> Tilesets;
		std::vector<std::array<uint8_t, 4>> Palette;
		// Source reader field names, including Frames/Chunks; binary buffers use an internal hex tag.
		std::string InspectionJson;
	};
	// Parses bytes only. Counts, inflated pixels, linked references and total retained bytes are bounded.
	// External tilesets require a separate host capability and are refused by this byte-only parser.
	bool ReadAseprite(
		std::span<const std::byte> bytes,
		AsepriteDocument &out,
		std::string &failure,
		uint64_t maximumBytes = 64ull * 1024 * 1024
	);
	// Renders one frame, optionally one named layer subtree; crop uses the cel's own bounds.
	bool RenderAseprite(
		const AsepriteDocument &document,
		size_t frame,
		std::string_view layer,
		bool crop,
		bool opacity,
		assets::TextureData &out,
		std::string &failure,
		uint64_t maximumBytes = 64ull * 1024 * 1024
	);
}
