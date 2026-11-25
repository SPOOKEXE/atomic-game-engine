#pragma once

// From `scene::EditableMesh` attributes to resident imagegraph data sources.
//
// The GPU evaluator samples textures, so a host names each `image.mesh_attribute`
// node as a data `image.source` before `Renderer::SetImageGraph` and provides the
// tile when an evaluation needs it. The name hashes the node's properties, the mesh
// entity and its revision, so it identifies the pixels: an unchanged mesh is not
// rasterized while its outputs stand, and a tile may be dropped once it has been read.
//
// @tier L12 · client

#include <engine/assets/Texture.hpp>
#include <engine/core/Name.hpp>
#include <engine/imagegraph/Document.hpp>
#include <engine/render/ImageGraphBinding.hpp>

#include <cstdint>
#include <future>
#include <optional>
#include <unordered_map>
#include <vector>

namespace engine::ecs {
	class Store;
}

namespace engine::scene {
	struct EditableMesh;
}

namespace engine::render {
	class Renderer;

	// Copies one free attribute channel and its triangles; false when the channel is empty.
	bool CopyMeshAttribute(const scene::EditableMesh &, uint32_t channel, imagegraph::MeshAttributeData &out);

	// Resolves `editable-mesh://` references in one world for CPU evaluation.
	imagegraph::MeshResolver EditableMeshResolver(const ecs::Store &);

	// Names, rasterizes and uploads mesh attribute tiles for graph evaluations. Large
	// tiles rasterize on a worker; resident tiles past BUDGET leave least recent first.
	class ImageGraphMeshUploader {
	  public:
		// Resident tile bytes kept for graphs that may be edited and evaluated again.
		static constexpr size_t BUDGET = size_t(1024) * 1024 * 1024;
		// Largest tile rasterized inside Provide rather than on a worker.
		static constexpr size_t INLINE_PIXELS = size_t(512) * 512;

		// Inside the owning world. Rewrites every mesh attribute node of a resolved graph
		// as a data source in owner's namespace and appends its content-addressed binding.
		// Refusal leaves resolved partly rewritten.
		bool Lower(
			const ecs::Store &,
			core::Name owner,
			imagegraph::Document &resolved,
			std::vector<ImageGraphSourceBinding> &bindings,
			imagegraph::Diagnostic &
		);
		// Inside the owning world. Works toward the tile `Renderer::ImageGraphNeededSource`
		// named for this graph; call again each frame until the evaluation stops asking.
		// One tile is made at a time. False with a diagnostic when it cannot be made.
		bool
		Provide(const ecs::Store &, Renderer &, core::Name owner, core::Name tile, imagegraph::Diagnostic &);
		// Drops every tile in a namespace that is no longer evaluated.
		void Forget(Renderer &, core::Name owner);

	  private:
		struct RasterResult {
			assets::TextureData Texture;
			imagegraph::Diagnostic Diagnostic;
		};
		struct Tile {
			imagegraph::MeshAttribute Node;
			bool Wide = false;
		};
		struct Upload {
			core::Name Owner;
			core::Name Texture;
			size_t Bytes = 0;
		};
		struct Raster {
			core::Name Owner;
			core::Name Texture;
			std::future<RasterResult> Result;
		};
		bool Add(Renderer &, core::Name owner, core::Name tile, RasterResult, imagegraph::Diagnostic &);
		// Least recently uploaded first.
		std::vector<Upload> Uploads;
		std::optional<Raster> Running;
		// The node each name Lower produced rasterizes.
		std::unordered_map<uint64_t, Tile> Named;
	};

	// Whether any node of a document is a mesh attribute.
	bool HasMeshAttributes(const imagegraph::Document &);
}
