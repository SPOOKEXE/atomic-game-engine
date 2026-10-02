#pragma once

#include "MeshPayload.hpp"

namespace engine::imagegraph::detail {
	inline bool ValidLightPayload(const LightValue3D &light) {
		if (!light.Data) return true;
		const auto &data = *light.Data;
		return (data.Kind == LightKind3D::Point || data.Kind == LightKind3D::Directional) &&
			   MeshFinite(data.Transform.Position) && MeshFinite(data.Transform.Anchor) &&
			   MeshFinite(data.Transform.Scale) && MeshFinite(data.Transform.Rotation) &&
			   std::isfinite(data.Intensity) && std::isfinite(data.Radius) && std::isfinite(data.ShadowBias);
	}
	template <bool retained> uint64_t SceneStorageBytes(const SceneData3D &scene, size_t depth = 0) {
		if (depth > Limits::MaximumArrayDepth) return UINT64_MAX;
		uint64_t bytes = MeshAddBytes(sizeof(SceneData3D), MeshVectorBytes<retained>(scene.Objects));
		for (const auto &object : scene.Objects) {
			const uint64_t child = std::visit(
				[&](const auto &value) -> uint64_t {
					using T = std::decay_t<decltype(value)>;
					if constexpr (std::is_same_v<T, MeshValue3D>)
						return MeshStorageBytes<retained>(value);
					else if constexpr (std::is_same_v<T, LightValue3D>)
						return value.Data ? sizeof(LightData3D) : 0;
					else
						return value ? SceneStorageBytes<retained>(*value, depth + 1) : 0;
				},
				object.Data
			);
			bytes = MeshAddBytes(bytes, child);
		}
		return bytes;
	}
	template <bool retained> uint64_t SceneStorageBytes(const SceneValue3D &scene) {
		return scene.Data ? SceneStorageBytes<retained>(*scene.Data) : 0;
	}
	inline bool ValidSceneData(const SceneData3D &scene, size_t &count, size_t depth = 0) {
		if (depth > Limits::MaximumArrayDepth || scene.Objects.size() > Limits::MaximumArrayElements - count)
			return false;
		count += scene.Objects.size();
		if (!MeshFinite(scene.Transform.Position) || !MeshFinite(scene.Transform.Anchor) ||
			!MeshFinite(scene.Transform.Scale) || !MeshFinite(scene.Transform.Rotation))
			return false;
		for (const auto &object : scene.Objects) {
			if (!std::visit(
					[&](const auto &value) {
						using T = std::decay_t<decltype(value)>;
						if constexpr (std::is_same_v<T, MeshValue3D>)
							return ValidMeshPayload(value);
						else if constexpr (std::is_same_v<T, LightValue3D>)
							return ValidLightPayload(value);
						else
							return !value || ValidSceneData(*value, count, depth + 1);
					},
					object.Data
				))
				return false;
		}
		return true;
	}
	inline bool ValidScenePayload(const SceneValue3D &scene) {
		if (!scene.Data) return true;
		size_t count = 0;
		return ValidSceneData(*scene.Data, count) &&
			   SceneStorageBytes<false>(scene) <= Limits::MaximumArrayBytes;
	}
}
