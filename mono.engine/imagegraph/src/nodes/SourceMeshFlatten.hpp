#pragma once

#include "../MeshPayload.hpp"
#include "../SourceMeshTransform.hpp"

#include <array>
#include <type_traits>

namespace engine::imagegraph::detail {
	inline bool SourceMeshFlattenFitsFloat(double value) {
		return std::isfinite(value) && std::abs(value) <= std::numeric_limits<float>::max();
	}
	// Counts payload headers before the flatten pass scans vertex words or copies material surfaces.
	template <class T> bool SourceMeshFlattenWork(const T &value, uint64_t &work, size_t depth = 0) {
		constexpr uint64_t maximum = 64000000;
		auto add = [&](uint64_t amount) {
			if (amount > maximum - work) return false;
			work += amount;
			return true;
		};
		if (depth >= 128 || !add(1)) return false;
		if constexpr (std::is_same_v<T, MeshValue3D>) {
			if (!value.Data) return true;
			const auto &m = *value.Data;
			if (m.Parts.size() > Limits::MaximumArrayElements ||
				m.Materials.size() > Limits::MaximumArrayElements ||
				m.LocalTransforms.size() > MAXIMUM_MESH_TRANSFORMS)
				return false;
			if (!add(
					m.Instances.size() * 16 + m.ParticleRecords.size() * 16 + m.LocalTransforms.size() * 64 +
					64
				))
				return false;
			auto materialWork = [&](const MaterialValue3D &material) {
				const auto &d = material.Get();
				for (const auto *image : {&d.Surface, &d.Normal, &d.PropertiesMap})
					if (*image && !add((*image)->Pixels.size() * 2)) return false;
				return true;
			};
			for (const auto &material : m.Materials)
				if (!materialWork(material)) return false;
			for (const auto &part : m.Parts) {
				if (part.Vertices.size() > Limits::MaximumArrayElements ||
					!add(part.Vertices.size() * (128 + 256 * (depth + m.LocalTransforms.size())) + 1))
					return false;
				if (part.MaterialIndex >= m.Materials.size() ||
					!materialWork(m.Materials[part.MaterialIndex]))
					return false;
			}
			return true;
		} else if constexpr (std::is_same_v<T, SceneValue3D> ||
							 std::is_same_v<T, OwnedPayload3D<SceneData3D>>) {
			const auto *data = [&]() {
				if constexpr (std::is_same_v<T, SceneValue3D>)
					return value.Data ? &*value.Data : nullptr;
				else
					return value ? &*value : nullptr;
			}();
			if (!data) return true;
			if (!add(64)) return false;
			if (data->Objects.size() > Limits::MaximumArrayElements) return false;
			for (const auto &child : data->Objects)
				if (!std::visit(
						[&](const auto &v) { return SourceMeshFlattenWork(v, work, depth + 1); }, child.Data
					))
					return false;
			return true;
		} else
			return true;
	}

	struct SourceMeshFlatCost {
		uint64_t Bytes = sizeof(MeshData3D) + sizeof(MeshTransform3D);
		size_t Parts = 0, Vertices = 0;
	};
	template <class T>
	bool SourceMeshFlatten(
		const T &value,
		std::array<const MeshTransform3D *, 128> &chain,
		size_t depth,
		SourceMeshFlatCost &cost,
		MeshData3D *output
	) {
		if constexpr (std::is_same_v<T, MeshValue3D>) {
			if (!value.Data) return true;
			const auto &mesh = *value.Data;
			if (depth + mesh.LocalTransforms.size() > chain.size()) return false;
			for (const auto &transform : mesh.LocalTransforms)
				if (transform.Mirror) return true;
			size_t count = depth;
			for (size_t i = 0; i + 1 < mesh.LocalTransforms.size(); ++i)
				chain[count++] = &mesh.LocalTransforms[i];
			for (const auto &part : mesh.Parts) {
				cost.Parts++;
				cost.Vertices += part.Vertices.size();
				if (cost.Parts > Limits::MaximumArrayElements || cost.Vertices > Limits::MaximumArrayElements)
					return false;
				const auto &material = mesh.Materials[part.MaterialIndex];
				cost.Bytes = MeshAddBytes(
					cost.Bytes,
					sizeof(MeshPart3D) + sizeof(MaterialValue3D) +
						part.Vertices.size() * sizeof(MeshVertex3D) + MaterialStorageBytes<true>(material)
				);
				if (cost.Bytes > Limits::MaximumArrayBytes) return false;
				MeshPart3D *target = nullptr;
				if (output) {
					output->Materials.push_back(material);
					output->Parts.emplace_back();
					target = &output->Parts.back();
					target->MaterialIndex = uint32_t(output->Materials.size() - 1);
					target->Vertices.reserve(part.Vertices.size());
				}
				for (auto vertex : part.Vertices) {
					for (double component :
						 {vertex.Position.X,
						  vertex.Position.Y,
						  vertex.Position.Z,
						  vertex.Normal.X,
						  vertex.Normal.Y,
						  vertex.Normal.Z,
						  vertex.UV.X,
						  vertex.UV.Y})
						if (!SourceMeshFlattenFitsFloat(component)) return false;
					vertex.Position = {
						float(vertex.Position.X), float(vertex.Position.Y), float(vertex.Position.Z)
					};
					for (size_t i = count; i > 0; --i)
						vertex.Position = SourceMeshPoint(*chain[i - 1], vertex.Position);
					for (double component : {vertex.Position.X, vertex.Position.Y, vertex.Position.Z})
						if (!SourceMeshFlattenFitsFloat(component)) return false;
					vertex.Position = {
						float(vertex.Position.X), float(vertex.Position.Y), float(vertex.Position.Z)
					};
					vertex.Normal = {float(vertex.Normal.X), float(vertex.Normal.Y), float(vertex.Normal.Z)};
					vertex.UV = {float(vertex.UV.X), float(vertex.UV.Y)};
					if (target) target->Vertices.push_back(vertex);
				}
			}
			return true;
		} else if constexpr (std::is_same_v<T, SceneValue3D> ||
							 std::is_same_v<T, OwnedPayload3D<SceneData3D>>) {
			const auto *data = [&]() {
				if constexpr (std::is_same_v<T, SceneValue3D>)
					return value.Data ? &*value.Data : nullptr;
				else
					return value ? &*value : nullptr;
			}();
			if (!data) return true;
			if (depth >= chain.size()) return false;
			if (data->Transform.Mirror) return true;
			chain[depth] = &data->Transform;
			for (const auto &child : data->Objects)
				if (!std::visit(
						[&](const auto &item) {
							return SourceMeshFlatten(item, chain, depth + 1, cost, output);
						},
						child.Data
					))
					return false;
			return true;
		} else
			return true;
	}
}
