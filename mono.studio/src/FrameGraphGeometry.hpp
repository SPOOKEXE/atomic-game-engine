#pragma once

// Retain only flame primitives. ImGui still owns this frame's items, layout and input.
#include <array>
#include <cstdint>
#include <cstring>
#include <imgui.h>
#include <limits>
#include <vector>

namespace studio::frame_graph_detail {

	struct FlameGeometryKey {
		uint64_t Revision = 0;
		std::array<float, 2> Origin{};
		std::array<float, 4> Clip{};
		std::array<float, 2> Mouse{};
		std::array<float, 2> WhiteUv{};
		float Width = 0;
		float RowHeight = 0;
		float Scale = 0;
		float LabelMinimumWidth = 0;
		float FontSize = 0;
		float Density = 0;
		bool Hovered = false;
		int AtlasWidth = 0;
		int AtlasHeight = 0;
		unsigned int Accent = 0;
		unsigned int Bright = 0;
		ImFont *Font = nullptr;
		ImTextureData *AtlasTexture = nullptr;
		ImTextureID Texture = ImTextureID_Invalid;
		ImDrawListFlags Flags = 0;
		ImGuiID BakedFont = 0;
		int AtlasGeneration = 0;
		bool operator==(const FlameGeometryKey &) const = default;
	};

	struct FlameGeometry {
		FlameGeometryKey Key;
		std::vector<ImDrawVert> Vertices;
		std::vector<ImDrawIdx> Indices;
		uint32_t Hovered = UINT32_MAX;
		uint64_t Hits = 0;
		uint64_t Writes = 0;
		bool Ready = false;
	};

	inline FlameGeometryKey GeometryKey(
		ImDrawList &draw,
		uint64_t revision,
		ImVec2 origin,
		float width,
		float rowHeight,
		float scale,
		float labelMinimumWidth,
		bool hovered,
		unsigned int accent,
		unsigned int bright
	) {
		const ImVec2 minimum = draw.GetClipRectMin();
		const ImVec2 maximum = draw.GetClipRectMax();
		const ImVec2 white = ImGui::GetFontTexUvWhitePixel();
		const ImFontBaked *baked = ImGui::GetFontBaked();
		const ImVec2 mouse = hovered ? ImGui::GetMousePos() : ImVec2(-1, -1);
		return {
			.Revision = revision,
			.Origin = {origin.x, origin.y},
			.Clip = {minimum.x, minimum.y, maximum.x, maximum.y},
			.Mouse = {mouse.x, mouse.y},
			.WhiteUv = {white.x, white.y},
			.Width = width,
			.RowHeight = rowHeight,
			.Scale = scale,
			.LabelMinimumWidth = labelMinimumWidth,
			.FontSize = ImGui::GetFontSize(),
			.Density = baked->RasterizerDensity,
			.Hovered = hovered,
			.AtlasWidth = draw._CmdHeader.TexRef._TexData ? draw._CmdHeader.TexRef._TexData->Width : 0,
			.AtlasHeight = draw._CmdHeader.TexRef._TexData ? draw._CmdHeader.TexRef._TexData->Height : 0,
			.Accent = accent,
			.Bright = bright,
			.Font = ImGui::GetFont(),
			.AtlasTexture = draw._CmdHeader.TexRef._TexData,
			.Texture = draw._CmdHeader.TexRef._TexID,
			.Flags = draw.Flags,
			.BakedFont = baked->BakedId,
			.AtlasGeneration = ImGui::GetFont()->OwnerAtlas->TexNextUniqueID,
		};
	}

	// Indices are relative to this segment, never to another frame's draw list.
	// A command or 16-bit vertex boundary during capture declines reuse safely.
	template <class Paint>
	void PaintGeometry(
		FlameGeometry &cache, const FlameGeometryKey &key, ImDrawList &draw, Paint &&paint, bool retain = true
	) {
		if (!retain) {
			cache.Ready = false;
			cache.Hovered = UINT32_MAX;
			paint();
			return;
		}
		if (cache.Ready && cache.Key == key &&
			(draw._VtxCurrentIdx + cache.Vertices.size() < std::numeric_limits<ImDrawIdx>::max() ||
			 (draw.Flags & ImDrawListFlags_AllowVtxOffset) != 0)) {
			const int vertices = static_cast<int>(cache.Vertices.size());
			const int indices = static_cast<int>(cache.Indices.size());
			if (vertices > 0) {
				draw.PrimReserve(indices, vertices);
				std::memcpy(
					draw._VtxWritePtr, cache.Vertices.data(), cache.Vertices.size() * sizeof(ImDrawVert)
				);
				for (const ImDrawIdx index : cache.Indices)
					*draw._IdxWritePtr++ = static_cast<ImDrawIdx>(draw._VtxCurrentIdx + index);
				draw._VtxWritePtr += vertices;
				draw._VtxCurrentIdx += static_cast<unsigned int>(vertices);
			}
			cache.Hits++;
			return;
		}
		const int firstVertex = draw.VtxBuffer.Size;
		const int firstIndex = draw.IdxBuffer.Size;
		const int firstCommand = draw.CmdBuffer.Size;
		const unsigned int firstLocalVertex = draw._VtxCurrentIdx;
		const unsigned int firstOffset = draw._CmdHeader.VtxOffset;
		cache.Hovered = UINT32_MAX;
		paint();
		cache.Writes++;
		cache.Ready = draw.CmdBuffer.Size == firstCommand && draw._CmdHeader.VtxOffset == firstOffset &&
					  draw.VtxBuffer.Size - firstVertex < std::numeric_limits<ImDrawIdx>::max();
		if (!cache.Ready) return;
		cache.Vertices.assign(draw.VtxBuffer.begin() + firstVertex, draw.VtxBuffer.end());
		cache.Indices.resize(static_cast<size_t>(draw.IdxBuffer.Size - firstIndex));
		for (size_t index = 0; index < cache.Indices.size(); index++)
			cache.Indices[index] =
				static_cast<ImDrawIdx>(draw.IdxBuffer[firstIndex + index] - firstLocalVertex);
		cache.Key = key;
	}
}
