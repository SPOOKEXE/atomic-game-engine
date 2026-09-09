#pragma once

#include <engine/render/PortalCaptureTree.hpp>
#include <engine/render/PortalImageImport.hpp>

namespace engine::render {
	inline constexpr size_t MAX_IMPORTED_PORTAL_CAPTURE_TREES = 16;
	inline constexpr size_t MAX_IMPORTED_PORTAL_TREE_METADATA_BYTES = MAX_PORTAL_EXCHANGE_BYTES;
	// Two prepared trees can coexist with their source-preview pins while a newer
	// candidate uploads, so the renderer owns four independent lease handles.
	inline constexpr size_t MAX_PORTAL_CAPTURE_TREE_LEASES = 4;

	// Renderer-local residency accompanies authenticated value records. These image
	// and program handles are leases inside one renderer, never wire identifiers.
	struct ImportedPortalCaptureTreeNode {
		PortalCaptureTreeEndpoint Producer;
		std::string RetainedBodyPlayer;
		PortalCaptureTreeCamera Camera;
		PortalImageBinding Binding;
		PortalCaptureLighting Lighting;
		PortalCaptureLenses Lenses;
		uint32_t Width = 0, Height = 0;
		std::array<uint64_t, MAX_PORTAL_TRANSPARENT_LAYERS + 2> Images{};
		uint64_t LensPrograms = 0;
	};
	struct ImportedPortalCaptureTree {
		uint64_t Token = 0;
		bool Owner = true;
		// Revocation makes a tree unavailable immediately, while a submitted GPU job
		// retains its source textures through its fence.
		bool Revoked = false;
		std::array<uint64_t, MAX_PORTAL_CAPTURE_TREE_LEASES> Leases{};
		std::vector<ImportedPortalCaptureTreeNode> Nodes;
		std::vector<PortalCaptureTreeEdge> Edges;
		size_t MetadataBytes = 0;
	};
}
