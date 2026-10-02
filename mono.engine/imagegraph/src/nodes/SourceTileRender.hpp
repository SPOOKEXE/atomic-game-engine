#pragma once
#include "Processor.hpp"
namespace engine::imagegraph::detail {
	template <class Tileset>
	inline bool RenderSourceTilePixel(
		const Tileset &tileset,
		const Image &map,
		uint32_t x,
		uint32_t y,
		uint32_t width,
		uint32_t height,
		float frame,
		Rgba &pixel
	) {
		const float sx = float(tileset.TileSize.X), sy = float(tileset.TileSize.Y);
		const float px = ((float(x) + .5f) / float(width)) * (sx * float(map.Width));
		const float py = ((float(y) + .5f) / float(height)) * (sy * float(map.Height));
		const auto sample = SampleNearest(
			map, std::floor(px / sx) / float(map.Width - 1), std::floor(py / sy) / float(map.Height - 1)
		);
		const float red = float(sample[0]);
		if (!std::isfinite(red) || double(red) < double(INT32_MIN) || double(red) > double(INT32_MAX))
			return false;
		const int32_t tile = int32_t(red + .1f * (red < 0 ? -1.f : red > 0 ? 1.f : 0.f));
		pixel = {};
		if (!tile) return true;
		int32_t index = tile > 0 ? tile - 1 : 0;
		if (tile < 0) {
			const int64_t animation = -int64_t(tile) - 1;
			if (animation < 0 || animation >= int64_t(tileset.Animations.size())) return false;
			const auto &a = tileset.Animations[size_t(animation)];
			if (!a.Length || !std::isfinite(frame) || double(frame) < double(INT32_MIN) ||
				double(frame) > double(INT32_MAX))
				return false;
			int64_t flattened = int32_t(frame) % int32_t(a.Length);
			for (size_t i = 0; i < size_t(animation); i++)
				flattened += int64_t(tileset.Animations[i].Indices.size());
			if (flattened < 0) return false;
			bool found = false;
			for (const auto &row : tileset.Animations) {
				if (flattened < int64_t(row.Indices.size())) {
					index = row.Indices[size_t(flattened)];
					found = true;
					break;
				}
				flattened -= int64_t(row.Indices.size());
			}
			if (!found) return false;
		}
		const int32_t columns = int32_t(float(tileset.Texture.Width) / sx);
		if (columns <= 0) return false;
		const int32_t tx = index % columns, ty = index / columns;
		float ux = (px - std::floor(px / sx) * sx) / sx, uy = (py - std::floor(py / sy) * sy) / sy;
		const float variant = float(sample[1]) + .1f;
		if (!std::isfinite(variant) || double(variant) < double(INT32_MIN) ||
			double(variant) > double(INT32_MAX))
			return false;
		const int32_t flags = int32_t(variant), rotation = flags % 4;
		if ((flags / 4) % 2 == 1) ux = 1 - ux;
		if ((flags / 8) % 2 == 1) uy = 1 - uy;
		if (rotation == 1) {
			const float previous = ux;
			ux = uy;
			uy = 1 - previous;
		}
		if (rotation == 2) {
			ux = 1 - ux;
			uy = 1 - uy;
		}
		if (rotation == 3) {
			const float previous = ux;
			ux = 1 - uy;
			uy = previous;
		}
		pixel = SampleNearest(
			tileset.Texture,
			(float(tx) * sx + ux * sx) / float(tileset.Texture.Width),
			(float(ty) * sy + uy * sy) / float(tileset.Texture.Height)
		);
		return true;
	}
} // namespace engine::imagegraph::detail
