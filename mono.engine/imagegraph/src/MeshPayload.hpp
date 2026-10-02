#pragma once

#include <engine/imagegraph/Document.hpp>

#include <algorithm>
#include <cmath>
#include <limits>

namespace engine::imagegraph::detail {
	inline constexpr size_t MAXIMUM_MESH_TRANSFORMS = 64;

	inline uint64_t MeshAddBytes(uint64_t left, uint64_t right) {
		return right > std::numeric_limits<uint64_t>::max() - left ? std::numeric_limits<uint64_t>::max()
																   : left + right;
	}
	template <bool retained, class T> uint64_t MeshVectorBytes(const std::vector<T> &items) {
		const uint64_t count = retained ? items.capacity() : items.size();
		return count > std::numeric_limits<uint64_t>::max() / sizeof(T) ? std::numeric_limits<uint64_t>::max()
																		: count * sizeof(T);
	}
	template <bool retained> uint64_t MaterialStorageBytes(const MaterialValue3D &material) {
		uint64_t bytes = material.Data ? sizeof(MaterialData3D) : 0;
		const auto &data = material.Get();
		for (const auto *surface : {&data.Surface, &data.Normal, &data.PropertiesMap})
			if (*surface) bytes = MeshAddBytes(bytes, MeshVectorBytes<retained>((*surface)->Pixels));
		return bytes;
	}
	template <bool retained> uint64_t MeshStorageBytes(const MeshValue3D &mesh) {
		if (!mesh.Data) return 0;
		const auto &data = *mesh.Data;
		uint64_t bytes = MeshAddBytes(sizeof(MeshData3D), MeshVectorBytes<retained>(data.Parts));
		bytes = MeshAddBytes(bytes, MeshVectorBytes<retained>(data.Edges));
		bytes = MeshAddBytes(bytes, MeshVectorBytes<retained>(data.Materials));
		bytes = MeshAddBytes(bytes, MeshVectorBytes<retained>(data.LocalTransforms));
		bytes = MeshAddBytes(bytes, MeshVectorBytes<retained>(data.Instances));
		for (const auto &part : data.Parts)
			bytes = MeshAddBytes(bytes, MeshVectorBytes<retained>(part.Vertices));
		for (const auto &material : data.Materials)
			bytes = MeshAddBytes(bytes, MaterialStorageBytes<retained>(material));
		return bytes;
	}
	inline bool MeshFinite(Vector2 value) {
		return std::isfinite(value.X) && std::isfinite(value.Y);
	}
	inline bool MeshFinite(Vector3 value) {
		return std::isfinite(value.X) && std::isfinite(value.Y) && std::isfinite(value.Z);
	}
	inline bool MeshFinite(Quaternion value) {
		return std::isfinite(value.X) && std::isfinite(value.Y) && std::isfinite(value.Z) &&
			   std::isfinite(value.W);
	}
	inline bool ValidMaterialSurface(const MaterialSurface3D &surface) {
		return ValidSurfaceLayout(surface, Limits::MaximumDimension, Limits::MaximumArrayBytes) &&
			   FiniteSurfaceSamples(surface);
	}
	inline bool ValidMaterialPayload(const MaterialValue3D &material) {
		const auto &data = material.Get();
		if (!MeshFinite(data.TextureScale) || !MeshFinite(data.TextureShift) ||
			!MeshFinite(data.MetallicRange) || !MeshFinite(data.RoughnessRange))
			return false;
		for (double number :
			 {data.TextureFilter,
			  data.Diffuse,
			  data.Specular,
			  data.Shininess,
			  data.Reflectance,
			  data.NormalStrength})
			if (!std::isfinite(number)) return false;
		for (const auto *surface : {&data.Surface, &data.Normal, &data.PropertiesMap})
			if (*surface && !ValidMaterialSurface(**surface)) return false;
		return MaterialStorageBytes<false>(material) <= Limits::MaximumArrayBytes;
	}
	inline bool ValidMeshPayload(const MeshValue3D &mesh) {
		if (!mesh.Data) return true;
		const auto &data = *mesh.Data;
		if (data.LocalTransforms.empty() || data.LocalTransforms.size() > MAXIMUM_MESH_TRANSFORMS ||
			data.Parts.size() > Limits::MaximumArrayElements ||
			data.Materials.size() > Limits::MaximumArrayElements ||
			data.Edges.size() > Limits::MaximumArrayElements)
			return false;
		for (const auto &transform : data.LocalTransforms)
			if (!MeshFinite(transform.Position) || !MeshFinite(transform.Anchor) ||
				!MeshFinite(transform.Rotation) || !MeshFinite(transform.Scale))
				return false;
		if (data.Instances.size() > Limits::MaximumArrayElements ||
			(!data.Instanced && !data.Instances.empty()) ||
			!MeshFinite(data.InstanceObjectTransform.Position) ||
			!MeshFinite(data.InstanceObjectTransform.Anchor) ||
			!MeshFinite(data.InstanceObjectTransform.Rotation) ||
			!MeshFinite(data.InstanceObjectTransform.Scale))
			return false;
		for (const auto &instance : data.Instances)
			for (float value : instance.Fields)
				if (!std::isfinite(value)) return false;
		size_t vertices = 0;
		for (const auto &part : data.Parts) {
			if (part.LocalMatrix &&
				std::any_of(part.LocalMatrix->begin(), part.LocalMatrix->end(), [](double value) {
					return !std::isfinite(value);
				}))
				return false;
			if (part.MaterialIndex >= data.Materials.size() || part.Vertices.size() % 3 != 0 ||
				part.Vertices.size() > Limits::MaximumArrayElements - vertices)
				return false;
			vertices += part.Vertices.size();
			for (const auto &vertex : part.Vertices)
				if (!MeshFinite(vertex.Position) || !MeshFinite(vertex.Normal) || !MeshFinite(vertex.UV))
					return false;
		}
		for (const auto &edge : data.Edges)
			if (!MeshFinite(edge.From) || !MeshFinite(edge.To)) return false;
		for (const auto &material : data.Materials)
			if (!ValidMaterialPayload(material)) return false;
		return MeshStorageBytes<false>(mesh) <= Limits::MaximumArrayBytes;
	}
}
