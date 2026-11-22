#include <engine/assets/Texture.hpp>
#include <engine/core/Float16.hpp>
#include <engine/core/Profiling.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/imagegraph/Reference.hpp>
#include <engine/render/ImageGraphMeshes.hpp>
#include <engine/render/Renderer.hpp>
#include <engine/scene/EditableMesh.hpp>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstring>

namespace engine::render {
	namespace {
		const scene::EditableMesh *FindMesh(const ecs::Store &store, std::string_view reference) {
			if (!imagegraph::IsEditableMeshReference(reference)) return nullptr;
			reference.remove_prefix(std::string_view("editable-mesh://").size());
			uint64_t id = 0;
			std::from_chars(reference.data(), reference.data() + reference.size(), id);
			return store.Get<scene::EditableMesh>(ecs::Entity(id));
		}
		uint64_t Fold(uint64_t signature, uint64_t word) {
			return (signature ^ word) * 1099511628211ull;
		}
		std::string Hex(uint64_t value) {
			constexpr char DIGITS[] = "0123456789abcdef";
			std::string text(16, '0');
			for (size_t index = 0; index < 16; ++index)
				text[15 - index] = DIGITS[(value >> (index * 4)) & 15];
			return text;
		}
		// Byte graphs upload linear RGBA8; wide graphs keep 16-bit detail as RGBA16F.
		assets::TextureData
		Tile(const imagegraph::MeshAttribute &node, const std::vector<float> &rgba, bool wide) {
			assets::TextureData texture;
			texture.Width = node.Width;
			texture.Height = node.Height;
			texture.Format = wide ? assets::TextureFormat::RGBA16_FLOAT : assets::TextureFormat::RGBA8_LINEAR;
			texture.Pixels.resize(rgba.size() * (wide ? 2 : 1));
			for (size_t index = 0; index < rgba.size(); ++index) {
				if (wide) {
					const uint16_t half = core::EncodeFloat16(rgba[index]);
					std::memcpy(&texture.Pixels[index * 2], &half, 2);
				} else
					texture.Pixels[index] =
						std::byte(static_cast<uint8_t>(std::lround(rgba[index] * 255.0f)));
			}
			return texture;
		}
	}

	bool
	CopyMeshAttribute(const scene::EditableMesh &mesh, uint32_t channel, imagegraph::MeshAttributeData &out) {
		if (channel >= scene::EDITABLE_ATTRIBUTE_CHANNELS || mesh.Attributes[channel].empty()) return false;
		imagegraph::MeshAttributeData data;
		data.UVs.reserve(mesh.UVs.size());
		for (const auto &uv : mesh.UVs)
			data.UVs.push_back({uv.X, uv.Y});
		data.Values.assign(mesh.Attributes[channel].begin(), mesh.Attributes[channel].end());
		data.Indices = mesh.Indices;
		out = std::move(data);
		return true;
	}

	imagegraph::MeshResolver EditableMeshResolver(const ecs::Store &store) {
		return [&store](
				   const imagegraph::MeshAttribute &node,
				   imagegraph::MeshAttributeData &out,
				   std::string &failure
			   ) {
			const auto *mesh = FindMesh(store, node.Mesh);
			if (mesh == nullptr) {
				failure = "mesh attribute names no editable mesh in this world";
				return false;
			}
			if (!CopyMeshAttribute(*mesh, node.Attribute, out)) {
				failure = "editable mesh has no values in the selected attribute channel";
				return false;
			}
			return true;
		};
	}

	bool HasMeshAttributes(const imagegraph::Document &document) {
		return std::any_of(document.Nodes.begin(), document.Nodes.end(), [](const imagegraph::Node &node) {
			return std::holds_alternative<imagegraph::MeshAttribute>(node.Value);
		});
	}

	bool ImageGraphMeshUploader::Lower(
		const ecs::Store &store,
		Renderer &renderer,
		core::Name owner,
		uint64_t graph,
		imagegraph::Document &resolved,
		imagegraph::Diagnostic &diagnostic
	) {
		ENGINE_PROFILE("imagegraph mesh attribute lowering");
		const bool wide = resolved.Storage == imagegraph::Precision::Wide;
		std::vector<Upload> named;
		for (auto &node : resolved.Nodes) {
			const auto *attribute = std::get_if<imagegraph::MeshAttribute>(&node.Value);
			if (attribute == nullptr) continue;
			const auto *mesh = FindMesh(store, attribute->Mesh);
			if (mesh == nullptr) {
				diagnostic = {node.Id, "mesh attribute names no editable mesh in this world"};
				return false;
			}
			uint64_t key = Fold(Fold(imagegraph::Fingerprint(node.Value), mesh->Revision), owner.Id());
			key = Fold(Fold(key, std::hash<std::string>{}(attribute->Mesh)), wide);
			const core::Name texture("imagegraph-mesh-attribute/" + Hex(key));
			if (renderer.TextureHandle(texture, owner) == nullptr) {
				imagegraph::MeshAttributeData data;
				if (!CopyMeshAttribute(*mesh, attribute->Attribute, data)) {
					diagnostic = {node.Id, "editable mesh has no values in the selected attribute channel"};
					return false;
				}
				std::vector<float> rgba;
				if (!imagegraph::RasteriseMeshAttribute(*attribute, data, rgba, diagnostic)) {
					diagnostic.Node = node.Id;
					return false;
				}
				if (!renderer.AddTexture(texture, Tile(*attribute, rgba, wide), owner)) {
					diagnostic = {node.Id, "mesh attribute tile upload refused"};
					return false;
				}
			}
			named.push_back({owner, texture});
			node.Value =
				imagegraph::Source{std::string(texture.Text()), imagegraph::SourceInterpretation::Data};
		}
		auto &previous = Uploads[graph];
		for (const auto &upload : previous)
			if (std::none_of(named.begin(), named.end(), [&](const Upload &kept) {
					return kept.Texture == upload.Texture && kept.Owner == upload.Owner;
				}))
				renderer.DropTexture(upload.Texture, upload.Owner);
		previous = std::move(named);
		if (previous.empty()) Uploads.erase(graph);
		diagnostic = {};
		return true;
	}

	void ImageGraphMeshUploader::Forget(Renderer &renderer, uint64_t graph) {
		const auto found = Uploads.find(graph);
		if (found == Uploads.end()) return;
		for (const auto &upload : found->second)
			renderer.DropTexture(upload.Texture, upload.Owner);
		Uploads.erase(found);
	}
}
