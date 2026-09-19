#include <engine/assets/Builtin.hpp>
#include <engine/core/Name.hpp>
#include <engine/ecs/Attributes.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/scene/Components.hpp>
#include <engine/scene/EditableMesh.hpp>
#include <engine/script/GltfSceneExport.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace engine::script {
	namespace {
		constexpr std::string_view STABLE_ID_ATTRIBUTE = "DataFactoryId";

		bool StableId(const ecs::Store &store, ecs::Entity entity, std::string &out) {
			ecs::AttributeValue value;
			if (!ecs::GetAttribute(store, entity, core::Name(STABLE_ID_ATTRIBUTE), value) ||
				value.Type != ecs::PropertyType::String || value.String.empty())
				return false;
			out = value.String;
			return true;
		}

		bool Finite(const core::Vector3 &value) {
			return std::isfinite(value.X) && std::isfinite(value.Y) && std::isfinite(value.Z);
		}

		bool Finite(const core::Color3 &value) {
			return std::isfinite(value.R) && std::isfinite(value.G) && std::isfinite(value.B);
		}

		bool NormalisedFrame(const core::CFrame &source, core::CFrame &out) {
			if (!Finite(source.Position) || !std::isfinite(source.QuaternionX) ||
				!std::isfinite(source.QuaternionY) || !std::isfinite(source.QuaternionZ) ||
				!std::isfinite(source.QuaternionW))
				return false;
			const double length = std::hypot(
				std::hypot(static_cast<double>(source.QuaternionX), static_cast<double>(source.QuaternionY)),
				std::hypot(static_cast<double>(source.QuaternionZ), static_cast<double>(source.QuaternionW))
			);
			if (!std::isfinite(length) || length <= 0.0) return false;
			out = source;
			out.QuaternionX = static_cast<float>(static_cast<double>(source.QuaternionX) / length);
			out.QuaternionY = static_cast<float>(static_cast<double>(source.QuaternionY) / length);
			out.QuaternionZ = static_cast<float>(static_cast<double>(source.QuaternionZ) / length);
			out.QuaternionW = static_cast<float>(static_cast<double>(source.QuaternionW) / length);
			return true;
		}

		bool HasTextureFacts(const scene::SurfaceAppearance &surface) {
			return surface.ColourMap.IsValid() || surface.NormalMap.IsValid() ||
				   surface.RoughnessMap.IsValid() || surface.OcclusionMap.IsValid() ||
				   surface.HeightMap.IsValid() || surface.MetalnessMap.IsValid() ||
				   surface.EmissiveMap.IsValid();
		}

		bool MeshFromEditable(const scene::EditableMesh &editable, assets::MeshData &out) {
			if (editable.Positions.empty() || editable.Positions.size() > MAX_GLTF_EXPORT_VERTICES ||
				editable.Indices.empty() || editable.Indices.size() > MAX_GLTF_EXPORT_INDICES ||
				editable.Indices.size() % 3 != 0 || editable.Normals.size() != editable.Positions.size() ||
				editable.UVs.size() != editable.Positions.size())
				return false;
			out.Vertices.reserve(editable.Positions.size());
			for (size_t index = 0; index < editable.Positions.size(); ++index) {
				if (!Finite(editable.Positions[index]) || !Finite(editable.Normals[index]) ||
					!std::isfinite(editable.UVs[index].X) || !std::isfinite(editable.UVs[index].Y))
					return false;
				assets::MeshVertex vertex{};
				vertex.Position[0] = editable.Positions[index].X;
				vertex.Position[1] = editable.Positions[index].Y;
				vertex.Position[2] = editable.Positions[index].Z;
				vertex.Normal[0] = editable.Normals[index].X;
				vertex.Normal[1] = editable.Normals[index].Y;
				vertex.Normal[2] = editable.Normals[index].Z;
				vertex.TexCoord[0] = editable.UVs[index].X;
				vertex.TexCoord[1] = editable.UVs[index].Y;
				out.Vertices.push_back(vertex);
			}
			out.Indices = editable.Indices;
			out.ComputeBounds();
			return out.IsValid();
		}

		bool
		MeshFromName(ecs::Store &store, const core::Name &name, assets::MeshData &out, std::string &reason) {
			if (!name.IsValid()) {
				out = assets::MakeBuiltin(assets::BuiltinMesh::Cube);
				return true;
			}
			assets::BuiltinMesh builtin;
			if (assets::BuiltinFromName(name.Text(), builtin)) {
				out = assets::MakeBuiltin(builtin);
				return true;
			}
			bool matchedEditable = false;
			store.Each<const scene::EditableMesh>([&](ecs::Entity entity,
													  const scene::EditableMesh &editable) {
				if (matchedEditable || scene::EditableMeshContentName(store, entity) != name) return;
				matchedEditable = true;
				if (!MeshFromEditable(editable, out)) reason = "editable_mesh_geometry_invalid_or_over_limit";
			});
			if (matchedEditable) {
				if (reason.empty()) return true;
				return false;
			}
			reason = "source_geometry_unavailable";
			return false;
		}

		core::Vector3 ScaleFor(const assets::MeshData &mesh, const scene::Bounds &bounds) {
			const core::Vector3 extent{
				(mesh.Maximum.X - mesh.Minimum.X) * 0.5f,
				(mesh.Maximum.Y - mesh.Minimum.Y) * 0.5f,
				(mesh.Maximum.Z - mesh.Minimum.Z) * 0.5f,
			};
			return {
				extent.X > 0.0f ? bounds.HalfExtent.X / extent.X : 1.0f,
				extent.Y > 0.0f ? bounds.HalfExtent.Y / extent.Y : 1.0f,
				extent.Z > 0.0f ? bounds.HalfExtent.Z / extent.Z : 1.0f
			};
		}
	}

	bool CaptureGltfSceneExport(ecs::Store &store, GltfSceneExport &out, std::string &failure) {
		GltfSceneExport captured;
		captured.Tick = store.Time().Tick;
		std::vector<std::pair<ecs::Entity, std::string>> selected;
		std::unordered_set<std::string> ids;
		store.Each<const ecs::InstanceName>([&](ecs::Entity entity, const ecs::InstanceName &) {
			std::string id;
			if (!StableId(store, entity, id)) return;
			if (id.size() > 256 || !ids.emplace(id).second) {
				failure = "gltf export requires unique stable ids of at most 256 bytes";
				return;
			}
			selected.emplace_back(entity, std::move(id));
		});
		if (!failure.empty()) return false;
		std::sort(selected.begin(), selected.end(), [](const auto &left, const auto &right) {
			return left.second < right.second;
		});

		std::unordered_map<std::string, size_t> meshIndexes;
		for (const auto &[entity, id] : selected) {
			if (const auto *camera = store.Get<scene::Camera>(entity); camera != nullptr) {
				const auto *transform = store.Get<scene::Transform>(entity);
				core::CFrame frame;
				if (transform == nullptr || !NormalisedFrame(transform->Frame, frame) ||
					!std::isfinite(camera->NearPlane) || !std::isfinite(camera->FarPlane) ||
					!std::isfinite(camera->FieldOfViewRadians) || !(camera->NearPlane > 0.0f) ||
					!(camera->FarPlane > camera->NearPlane) || !(camera->FieldOfViewRadians > 0.0f) ||
					!(camera->FieldOfViewRadians < std::numbers::pi_v<float>)) {
					captured.Unavailable.push_back({id, "camera", "camera_transform_or_projection_invalid"});
				} else {
					captured.Cameras.push_back(
						{id,
						 std::string(store.InstanceNameOf(entity).Text()),
						 frame,
						 camera->FieldOfViewRadians,
						 camera->NearPlane,
						 camera->FarPlane}
					);
				}
			}

			const auto *visual = store.Get<scene::Visual>(entity);
			const auto *transform = store.Get<scene::Transform>(entity);
			const auto *bounds = store.Get<scene::Bounds>(entity);
			if (visual == nullptr || transform == nullptr || bounds == nullptr) continue;
			if (!visual->Visible) {
				captured.Unavailable.push_back({id, "visibility", "visual_hidden_not_representable"});
				continue;
			}
			if (!std::isfinite(visual->Transparency) || visual->Transparency < 0.0f ||
				visual->Transparency > 1.0f) {
				captured.Unavailable.push_back({id, "appearance", "visual_transparency_invalid"});
				continue;
			}
			const auto *surface = store.Get<scene::SurfaceAppearance>(entity);
			if (!Finite(visual->Tint) || (surface != nullptr && !Finite(surface->Colour))) {
				captured.Unavailable.push_back({id, "appearance", "material_colour_invalid"});
				continue;
			}
			if (surface != nullptr && surface->Mode != scene::AlphaMode::Opaque &&
				surface->ColourMap.IsValid()) {
				captured.Unavailable.push_back({id, "appearance", "surface_alpha_mode_texture_dependent"});
				continue;
			}
			if (surface != nullptr && HasTextureFacts(*surface)) {
				captured.Unavailable.push_back({id, "appearance", "surface_appearance_texture_unavailable"});
				continue;
			}
			if (captured.Nodes.size() == MAX_GLTF_EXPORT_NODES) {
				failure = "gltf export node limit exceeded";
				return false;
			}
			assets::MeshData mesh;
			std::string reason;
			if (!MeshFromName(store, visual->Mesh, mesh, reason)) {
				captured.Unavailable.push_back({id, "geometry", std::move(reason)});
				continue;
			}
			const core::Vector3 scale = ScaleFor(mesh, *bounds);
			core::CFrame frame;
			if (!NormalisedFrame(transform->Frame, frame) || !Finite(scale) || !(scale.X > 0.0f) ||
				!(scale.Y > 0.0f) || !(scale.Z > 0.0f)) {
				captured.Unavailable.push_back({id, "geometry", "transform_or_mesh_extent_invalid"});
				continue;
			}
			const std::string meshName(visual->Mesh.IsValid() ? visual->Mesh.Text() : "builtin/cube");
			auto [found, inserted] = meshIndexes.emplace(meshName, captured.Meshes.size());
			if (inserted) captured.Meshes.push_back({meshName, std::move(mesh)});
			GltfExportMaterial material;
			material.BaseColour = visual->Tint;
			material.Alpha = 1.0f - visual->Transparency;
			material.AlphaMode =
				visual->Transparency > 0.0f ? GltfExportAlphaMode::Blend : GltfExportAlphaMode::Opaque;
			if (surface != nullptr) {
				material.BaseColour.R *= surface->Colour.R;
				material.BaseColour.G *= surface->Colour.G;
				material.BaseColour.B *= surface->Colour.B;
			}
			captured.Nodes.push_back(
				{id, std::string(store.InstanceNameOf(entity).Text()), frame, scale, found->second, material}
			);
		}
		out = std::move(captured);
		return true;
	}
}
