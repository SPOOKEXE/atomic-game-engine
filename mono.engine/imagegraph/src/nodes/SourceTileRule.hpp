#pragma once
#include "Processor.hpp"

#include <array>
#include <cmath>
namespace engine::imagegraph::detail {
	struct PreparedTileRule {
		std::array<float, 64> Selection{};
		std::array<float, 64> UniqueSelection{};
		std::array<float, 640> Groups{};
		std::array<float, 256> Replacements{};
		uint32_t Width = 1, Height = 1, Range = 1, ScanWidth = 1, ScanHeight = 1;
		uint32_t UniqueCount = 0, ReplacementCount = 0;
		float Probability = 1;
	};
	template <class Rule, class Tileset>
	inline bool PrepareSourceTileRule(
		const Rule &rule, const Tileset &tileset, PreparedTileRule &prepared, bool &undefinedGroup
	) {
		undefinedGroup = false;
		if (rule.Size.X <= 0 || rule.Size.Y <= 0 || rule.Size.X > 64 || rule.Size.Y > 64 ||
			std::trunc(rule.Size.X) != rule.Size.X || std::trunc(rule.Size.Y) != rule.Size.Y ||
			rule.Range > 32)
			return false;
		prepared = {};
		prepared.Width = uint32_t(rule.Size.X);
		prepared.Height = uint32_t(rule.Size.Y);
		prepared.Range = rule.Range;
		const uint32_t sw = prepared.Width + rule.Range * 2, sh = prepared.Height + rule.Range * 2;
		if (uint64_t(sw) * sh > 64 || rule.Replacements.size() > 256 / (prepared.Width * prepared.Height))
			return false;
		std::array<uint32_t, 10> terrainIds{};
		uint32_t terrainCount = 0;
		uint32_t minX = 999, maxX = 0, minY = 999, maxY = 0;
		for (uint32_t i = 0; i < sw * sh; i++) {
			const bool has = i < rule.Selection.size();
			const bool terrain = has && rule.Selection[i].Terrain;
			const double index = has ? rule.Selection[i].Index : -1;
			if (!std::isfinite(index) || std::abs(index) > 65504) return false;
			const uint32_t x = i % sw, y = i / sw;
			if ((terrain || index != -1) && (y < rule.Range || y >= prepared.Height + rule.Range ||
											 x < rule.Range || x >= prepared.Width + rule.Range)) {
				minX = std::min(minX, x);
				maxX = std::max(maxX, x);
				minY = std::min(minY, y);
				maxY = std::max(maxY, y);
			}
			float selected = float(index);
			if (terrain) {
				if (index < 0 || index >= 10 || std::trunc(index) != index) return false;
				const uint32_t id = uint32_t(index);
				selected = float(10000 + id);
				bool known = false;
				for (uint32_t t = 0; t < terrainCount; t++)
					known |= terrainIds[t] == id;
				if (!known) terrainIds[terrainCount++] = id;
			}
			prepared.Selection[i] = selected;
			bool known = false;
			for (uint32_t t = 0; t < prepared.UniqueCount; t++)
				known |= prepared.UniqueSelection[t] == selected;
			if (!known) prepared.UniqueSelection[prepared.UniqueCount++] = selected;
		}
		prepared.ScanWidth = minX < maxX ? std::max(1u, maxX - minX + 1) : prepared.Width;
		prepared.ScanHeight = minX < maxX ? std::max(1u, maxY - minY + 1) : prepared.Height;
		for (uint32_t i = 0; i < terrainCount; i++) {
			const uint32_t id = terrainIds[i];
			if (id != i || id >= tileset.Terrains.size()) {
				undefinedGroup = true;
				return false;
			}
			const auto &indices = tileset.Terrains[id].Indices;
			if (indices.size() > 63) return false;
			// Source packs unique-order bands, while its selection shader addresses the
			// terrain ID.
			prepared.Groups[i * 64] = float(indices.size());
			for (size_t j = 0; j < indices.size(); j++)
				prepared.Groups[i * 64 + 1 + j] = float(indices[j]);
		}
		// Numeric selectors can also address terrain bands. A band the source
		// never uploaded depends on ambient GPU state and cannot be replayed here.
		for (uint32_t i = 0; i < prepared.UniqueCount; i++) {
			const float selection = prepared.UniqueSelection[i];
			if (selection < 10000) continue;
			// sh_tile_rule_select truncates the numeric selector before indexing a band.
			const uint32_t id = uint32_t(selection - 10000);
			if (id >= terrainCount) {
				undefinedGroup = true;
				return false;
			}
		}
		prepared.ReplacementCount = uint32_t(rule.Replacements.size());
		const uint32_t area = prepared.Width * prepared.Height;
		for (uint32_t i = 0; i < prepared.ReplacementCount; i++)
			for (uint32_t j = 0; j < area; j++) {
				const double replacement = j < rule.Replacements[i].size() ? rule.Replacements[i][j] : -1;
				if (!std::isfinite(replacement) || std::abs(replacement) > 65504) return false;
				prepared.Replacements[i * area + j] = float(replacement);
			}
		prepared.Probability = float(rule.Probability / 100);
		return std::isfinite(prepared.Probability);
	}
	inline float SourceTileRuleGroup(const PreparedTileRule &rule, float red) {
		const float base = red - 1;
		if (base == -1) return 0;
		for (uint32_t i = 0; i < rule.UniqueCount; i++) {
			const float selection = rule.UniqueSelection[i];
			if (selection < 10000) continue;
			const int32_t id = int32_t(selection - 10000);
			const int32_t length = int32_t(rule.Groups[size_t(id) * 64]);
			for (int32_t k = 0; k < length; k++) {
				const float tile = rule.Groups[size_t(id) * 64 + 1 + size_t(k)];
				if (tile != -1 && tile == base) return selection;
			}
		}
		return 0;
	}
	inline float SourceTileRuleRandom(float x, float y, float seed) {
		const float offset = (seed - std::floor(seed / 100000.f) * 100000.f) / 10.f;
		const float dot = (x + offset) * 1892.9898f + (y + offset) * 78.23453f;
		const float value = std::sin(dot) * 437.54123f;
		return value - std::floor(value);
	}
	inline bool SourceTileRuleMatch(
		const PreparedTileRule &rule, const Image &map, const Image &groups, float u, float v
	) {
		const uint32_t sw = rule.Width + 2 * rule.Range, sh = rule.Height + 2 * rule.Range;
		const float tx = 1.f / float(map.Width), ty = 1.f / float(map.Height);
		for (uint32_t y = 0; y < sh; y++)
			for (uint32_t x = 0; x < sw; x++) {
				float selected = rule.Selection[y * sw + x];
				if (selected == -1) continue;
				if (selected == -10000) selected = -1;
				const float sx = u + float(int32_t(x) - int32_t(rule.Range)) * tx;
				const float sy = v + float(int32_t(y) - int32_t(rule.Range)) * ty;
				if (selected >= 10000) {
					if (float(SampleNearest(groups, sx, sy)[0]) != selected) return false;
				} else if (float(SampleNearest(map, sx, sy)[0]) - 1 != selected)
					return false;
			}
		return true;
	}
	inline Rgba SourceTileRulePixel(
		const PreparedTileRule &rule,
		const Image &map,
		const Image &groups,
		uint32_t x,
		uint32_t y,
		float seed
	) {
		const float u = (float(x) + .5f) / float(map.Width), v = (float(y) + .5f) / float(map.Height);
		const float tx = 1.f / float(map.Width), ty = 1.f / float(map.Height);
		const Rgba base = ReadPixel(map, x, y);
		float originU = u, originV = v;
		int32_t shift = -1;
		const float px = std::floor(u * float(map.Width)), py = std::floor(v * float(map.Height));
		const float blockX = px - std::floor(px / float(rule.ScanWidth)) * float(rule.ScanWidth);
		const float blockY = py - std::floor(py / float(rule.ScanHeight)) * float(rule.ScanHeight);
		const uint32_t leftX = uint32_t(std::max(0, int32_t(rule.Width) - int32_t(rule.ScanWidth))) + 1;
		const uint32_t leftY = uint32_t(std::max(0, int32_t(rule.Height) - int32_t(rule.ScanHeight))) + 1;
		for (uint32_t i = 0; i < leftY; i++)
			for (uint32_t j = 0; j < leftX; j++) {
				const float sx = blockX + float(j), sy = blockY + float(i);
				if (sx >= float(rule.Width) || sy >= float(rule.Height)) continue;
				const float ou = u - sx * tx, ov = v - sy * ty;
				if (SourceTileRuleMatch(rule, map, groups, ou, ov)) {
					originU = ou;
					originV = ov;
					shift = int32_t(sy * float(rule.Width) + sx);
					break;
				}
			}
		if (shift == -1 || SourceTileRuleRandom(originU, originV, seed) > rule.Probability) return base;
		const uint32_t selected =
			uint32_t(SourceTileRuleRandom(originU, originV, seed + 100.f) * float(rule.ReplacementCount));
		if (selected >= rule.ReplacementCount) return base;
		return {
			double(rule.Replacements[selected * rule.Width * rule.Height + uint32_t(shift)] + 1.f), 0, 0, 1
		};
	}
} // namespace engine::imagegraph::detail
