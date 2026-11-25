#include <engine/assets/Texture.hpp>
#include <engine/core/Float16.hpp>
#include <engine/core/Profiling.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/imagegraph/Reference.hpp>
#include <engine/render/Renderer.hpp>
#include <engine/render/compute/ImageGraphMeshes.hpp>
#include <engine/render/resources/TextureTable.hpp>
#include <engine/scene/editable/EditableMesh.hpp>

#include <algorithm>
#include <charconv>
#include <chrono>
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
		uint64_t FoldMeshWord(uint64_t signature, uint64_t word) {
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
		TilePixels(const imagegraph::MeshAttribute &node, const std::vector<float> &rgba, bool wide) {
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

	namespace {
		// Identifies a tile's pixels: node properties, mesh entity and revision.
		core::Name TileName(
			const imagegraph::MeshAttribute &node,
			const scene::EditableMesh &mesh,
			core::Name owner,
			bool wide
		) {
			uint64_t key = FoldMeshWord(
				FoldMeshWord(imagegraph::Fingerprint(imagegraph::Operation(node)), mesh.Revision), owner.Id()
			);
			key = FoldMeshWord(FoldMeshWord(key, std::hash<std::string>{}(node.Mesh)), wide);
			return core::Name("imagegraph-mesh-attribute/" + Hex(key));
		}
		uint64_t UploadKey(core::Name owner, core::Name tile) {
			return (uint64_t(owner.Id()) << 32) | tile.Id();
		}
	}

	bool ImageGraphMeshUploader::Lower(
		const ecs::Store &store,
		core::Name owner,
		imagegraph::Document &resolved,
		std::vector<ImageGraphSourceBinding> &bindings,
		imagegraph::Diagnostic &diagnostic
	) {
		ENGINE_PROFILE("imagegraph mesh attribute lowering");
		const bool wide = resolved.Storage == imagegraph::Precision::Wide;
		// Names are content keys, so forgetting old ones costs a re-lower at worst.
		if (Named.size() > 4096) Named.clear();
		for (auto &node : resolved.Nodes) {
			const auto *attribute = std::get_if<imagegraph::MeshAttribute>(&node.Value);
			if (attribute == nullptr) continue;
			const auto *mesh = FindMesh(store, attribute->Mesh);
			if (mesh == nullptr) {
				diagnostic = {node.Id, "mesh attribute names no editable mesh in this world"};
				return false;
			}
			if (attribute->Attribute >= scene::EDITABLE_ATTRIBUTE_CHANNELS ||
				mesh->Attributes[attribute->Attribute].empty()) {
				diagnostic = {node.Id, "editable mesh has no values in the selected attribute channel"};
				return false;
			}
			const core::Name tile = TileName(*attribute, *mesh, owner, wide);
			Named.insert_or_assign(UploadKey(owner, tile), Tile{*attribute, wide});
			const std::string path(tile.Text());
			if (std::none_of(bindings.begin(), bindings.end(), [&](const auto &binding) {
					return binding.Path == path;
				}))
				bindings.push_back(
					{.Path = path,
					 .Texture = tile,
					 .Owner = owner,
					 .Interpretation = imagegraph::SourceInterpretation::Data,
					 .ContentAddressed = true,
					 .Width = attribute->Width,
					 .Height = attribute->Height,
					 .Format =
						 wide ? assets::TextureFormat::RGBA16_FLOAT : assets::TextureFormat::RGBA8_LINEAR}
				);
			node.Value = imagegraph::Source{path, imagegraph::SourceInterpretation::Data};
		}
		diagnostic = {};
		return true;
	}

	bool ImageGraphMeshUploader::Advance(Renderer &renderer, imagegraph::Diagnostic &diagnostic) {
		diagnostic = {};
		auto &job = *Uploading;
		if (!job.Result.Diagnostic.Message.empty()) {
			diagnostic = std::move(job.Result.Diagnostic);
			Uploading.reset();
			return false;
		}
		// A tile its world dropped leaves a stale row behind.
		std::erase_if(Uploads, [&](const Upload &upload) {
			return upload.Owner == job.Owner && upload.Texture == job.Texture;
		});
		const size_t bytes = job.Result.Texture.Pixels.size();
		size_t resident = bytes;
		for (const auto &upload : Uploads)
			resident += upload.Bytes;
		while (resident > BUDGET && !Uploads.empty()) {
			resident -= Uploads.front().Bytes;
			renderer.DropTexture(Uploads.front().Texture, Uploads.front().Owner);
			Uploads.erase(Uploads.begin());
		}
		const auto status = renderer.AddTextureBand(job.Texture, job.Result.Texture, BAND_BYTES, job.Owner);
		if (status == TextureBandStatus::Partial) return true;
		const bool added = status == TextureBandStatus::Added;
		if (added)
			Uploads.push_back({job.Owner, job.Texture, bytes});
		else if (renderer.TextureHandle(job.Texture, job.Owner) == nullptr)
			diagnostic = {std::string(job.Texture.Text()), "mesh attribute tile upload refused"};
		Disposal = std::async(std::launch::async, [pixels = std::move(job.Result)] {});
		Uploading.reset();
		return diagnostic.Message.empty();
	}

	bool ImageGraphMeshUploader::Provide(
		const ecs::Store &store,
		Renderer &renderer,
		core::Name owner,
		core::Name tile,
		imagegraph::Diagnostic &diagnostic
	) {
		if (renderer.TextureHandle(tile, owner) != nullptr) return true;
		diagnostic = {};
		// Whoever asked for it, so graphs taking turns never discard each other's work.
		if (Uploading) return Advance(renderer, diagnostic);
		if (Running) {
			if (Running->Result.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return true;
			Uploading.emplace(Finished{Running->Owner, Running->Texture, Running->Result.get()});
			Running.reset();
			return Advance(renderer, diagnostic);
		}
		const auto named = Named.find(UploadKey(owner, tile));
		// The mesh was edited since this graph was lowered; its next lowering renames it.
		if (named == Named.end()) return true;
		const Tile tileNode = named->second;
		const auto *mesh = FindMesh(store, tileNode.Node.Mesh);
		if (mesh == nullptr || TileName(tileNode.Node, *mesh, owner, tileNode.Wide) != tile) return true;
		imagegraph::MeshAttributeData data;
		if (!CopyMeshAttribute(*mesh, tileNode.Node.Attribute, data)) {
			diagnostic = {
				std::string(tile.Text()), "editable mesh has no values in the selected attribute channel"
			};
			return false;
		}
		auto rasterise = [tileNode, data = std::move(data)] {
			RasterResult result;
			try {
				std::vector<float> rgba;
				if (imagegraph::RasteriseMeshAttribute(tileNode.Node, data, rgba, result.Diagnostic))
					result.Texture = TilePixels(tileNode.Node, rgba, tileNode.Wide);
			} catch (const std::bad_alloc &) {
				result.Diagnostic = {{}, "mesh attribute raster allocation refused"};
			}
			return result;
		};
		if (size_t(tileNode.Node.Width) * tileNode.Node.Height <= INLINE_PIXELS) {
			Uploading.emplace(Finished{owner, tile, rasterise()});
			return Advance(renderer, diagnostic);
		}
		Running.emplace(Raster{owner, tile, std::async(std::launch::async, std::move(rasterise))});
		diagnostic = {};
		return true;
	}

	void ImageGraphMeshUploader::Forget(Renderer &renderer, core::Name owner) {
		std::erase_if(Uploads, [&](const Upload &upload) {
			if (upload.Owner != owner) return false;
			renderer.DropTexture(upload.Texture, upload.Owner);
			return true;
		});
	}
}
