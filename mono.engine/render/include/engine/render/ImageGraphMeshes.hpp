#pragma once

// From `scene::EditableMesh` attributes to resident imagegraph data sources.
//
// The GPU evaluator samples textures, so a host rasterizes each `image.mesh_attribute`
// node on the CPU with `imagegraph::RasteriseMeshAttribute`, uploads the tile and
// rewrites the node as a data `image.source` before `Renderer::SetImageGraph`. The
// uploaded name hashes the node's properties, the mesh entity and its revision, so an
// unchanged mesh is rasterized once and a changed one gets a new name.
//
// @tier L12 · client

#include <engine/core/Name.hpp>
#include <engine/imagegraph/Document.hpp>

#include <cstdint>
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

	// Uploads and names the rasterized tiles per graph, dropping tiles a graph stops naming.
	class ImageGraphMeshUploader {
	  public:
		// Inside the owning world. Rewrites every mesh attribute node of a resolved graph
		// as a data source in owner's namespace. Refusal leaves resolved partly rewritten
		// and keeps the graph's previous uploads.
		bool Lower(
			const ecs::Store &,
			Renderer &,
			core::Name owner,
			uint64_t graph,
			imagegraph::Document &resolved,
			imagegraph::Diagnostic &
		);
		// Drops every tile uploaded for a graph that is no longer evaluated.
		void Forget(Renderer &, uint64_t graph);

	  private:
		struct Upload {
			core::Name Owner;
			core::Name Texture;
		};
		std::unordered_map<uint64_t, std::vector<Upload>> Uploads;
	};

	// Whether any node of a document is a mesh attribute.
	bool HasMeshAttributes(const imagegraph::Document &);
}
