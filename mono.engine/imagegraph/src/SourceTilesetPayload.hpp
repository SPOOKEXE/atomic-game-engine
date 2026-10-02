#pragma once
#include "MeshPayload.hpp"

#include <engine/imagegraph/Document.hpp>
namespace engine::imagegraph::detail {
	template <bool Retained> uint64_t TilesetDataBytes(const TilesetData &data) {
		uint64_t bytes = MeshAddBytes(
			sizeof(data), Retained ? data.Texture.Pixels.capacity() : data.Texture.Pixels.size()
		);
		bytes = MeshAddBytes(bytes, Retained ? data.DisplayName.capacity() : data.DisplayName.size());
		bytes = MeshAddBytes(bytes, MeshVectorBytes<Retained>(data.Animations));
		bytes = MeshAddBytes(bytes, MeshVectorBytes<Retained>(data.Terrains));
		bytes = MeshAddBytes(bytes, MeshVectorBytes<Retained>(data.Rules));
		for (const auto &animation : data.Animations) {
			bytes = MeshAddBytes(bytes, Retained ? animation.Name.capacity() : animation.Name.size());
			bytes = MeshAddBytes(bytes, MeshVectorBytes<Retained>(animation.Indices));
		}
		for (const auto &terrain : data.Terrains) {
			bytes = MeshAddBytes(bytes, Retained ? terrain.Name.capacity() : terrain.Name.size());
			bytes = MeshAddBytes(bytes, MeshVectorBytes<Retained>(terrain.Indices));
		}
		for (const auto &rule : data.Rules) {
			bytes = MeshAddBytes(bytes, Retained ? rule.Name.capacity() : rule.Name.size());
			bytes = MeshAddBytes(bytes, MeshVectorBytes<Retained>(rule.Selection));
			bytes = MeshAddBytes(bytes, MeshVectorBytes<Retained>(rule.Replacements));
			for (const auto &replacement : rule.Replacements)
				bytes = MeshAddBytes(bytes, MeshVectorBytes<Retained>(replacement));
		}
		return bytes;
	}
	template <bool Retained> uint64_t TilesetStorageBytes(const TilesetValue &value) {
		return value.Data ? TilesetDataBytes<Retained>(*value.Data) : 0;
	}
	inline bool ValidTilesetPayload(const TilesetValue &value) {
		if (!value.Data) return true;
		const auto &data = *value.Data;
		if (!ValidSurfaceLayout(data.Texture, Limits::MaximumDimension, Limits::MaximumOutputBytes) ||
			!FiniteSurfaceSamples(data.Texture) || !MeshFinite(data.TileSize) || data.TileSize.X <= 0 ||
			data.TileSize.Y <= 0 || data.TileSize.X > Limits::MaximumDimension ||
			data.TileSize.Y > Limits::MaximumDimension ||
			data.DisplayName.size() > Limits::MaximumTextBytes || data.Animations.size() > 128 ||
			data.Terrains.size() > Limits::MaximumArrayElements ||
			data.Rules.size() > Limits::MaximumArrayElements ||
			TilesetDataBytes<true>(data) > Limits::MaximumEvaluationBytes)
			return false;
		size_t frames = 0, totalElements = 0;
		for (const auto &animation : data.Animations) {
			if (animation.Name.size() > Limits::MaximumTextBytes || animation.Indices.size() > 256 - frames ||
				animation.Length > 256)
				return false;
			frames += animation.Indices.size();
		}
		for (const auto &terrain : data.Terrains) {
			if (terrain.Name.size() > Limits::MaximumTextBytes || terrain.Type < -1 || terrain.Type > 4 ||
				terrain.Indices.size() > 63)
				return false;
			totalElements += terrain.Indices.size();
			if (totalElements > Limits::MaximumArrayElements) return false;
		}
		for (const auto &rule : data.Rules) {
			if (rule.Name.size() > Limits::MaximumTextBytes || !MeshFinite(rule.Size) ||
				!std::isfinite(rule.Probability) || rule.Range > 32 || rule.Size.X <= 0 || rule.Size.Y <= 0 ||
				rule.Size.X > 64 || rule.Size.Y > 64 || std::trunc(rule.Size.X) != rule.Size.X ||
				std::trunc(rule.Size.Y) != rule.Size.Y || rule.Selection.size() > 64 ||
				rule.Replacements.size() > 256)
				return false;
			for (const auto &selected : rule.Selection)
				if (!std::isfinite(selected.Index)) return false;
			totalElements += rule.Selection.size();
			if (totalElements > Limits::MaximumArrayElements) return false;
			for (const auto &replacement : rule.Replacements) {
				if (replacement.size() > 256) return false;
				totalElements += replacement.size();
				if (totalElements > Limits::MaximumArrayElements) return false;
				for (double index : replacement)
					if (!std::isfinite(index)) return false;
			}
		}
		return true;
	}
}
