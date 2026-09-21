#pragma once

#include <engine/render/PortalCaptureTree.hpp>
#include <engine/render/PortalImageImport.hpp>

namespace engine::render {
	// Maximum capture trees resident in one renderer.
	inline constexpr size_t MAX_IMPORTED_PORTAL_CAPTURE_TREES = 16;
	// Maximum CPU metadata bytes across one imported tree payload.
	inline constexpr size_t MAX_IMPORTED_PORTAL_TREE_METADATA_BYTES = MAX_PORTAL_EXCHANGE_BYTES;
	// Two prepared trees can coexist with their source-preview pins while a newer
	// candidate uploads, so the renderer owns four independent lease handles.
	inline constexpr size_t MAX_PORTAL_CAPTURE_TREE_LEASES = 4;

	// Renderer-local residency accompanies authenticated value records. These image
	// and program handles are leases inside one renderer, never wire identifiers.
	// Renderer-resident capture node with leased images and programs.
	struct ImportedPortalCaptureTreeNode {
		// Authenticated endpoint that produced this capture node.
		PortalCaptureTreeEndpoint Producer;
		// Player body retained by the capture tree, if any.
		std::string RetainedBodyPlayer;
		// Capture camera expressed in the producer world.
		PortalCaptureTreeCamera Camera;
		// Imported image group bound to this capture node.
		PortalImageBinding Binding;
		// Lighting snapshot used to shade the capture.
		PortalCaptureLighting Lighting;
		// Lens stack applied while composing the capture.
		PortalCaptureLenses Lenses;
		// Allocated image width in pixels.
		uint32_t Width = 0;
		// Allocated image height in pixels.
		uint32_t Height = 0;
		// Renderer-local image leases in base, transparent-layer, and overlay order.
		std::array<uint64_t, MAX_PORTAL_TRANSPARENT_LAYERS + 2> Images{};
		// Renderer-local packed lens-program lease.
		uint64_t LensPrograms = 0;
	};
	// Imported tree residency owned by one renderer token.
	struct ImportedPortalCaptureTree {
		// Renderer-local token used to release this imported tree.
		uint64_t Token = 0;
		// Whether this renderer owns the leases rather than borrowing them.
		bool Owner = true;
		// Revocation makes a tree unavailable immediately, while a submitted GPU job
		// retains its source textures through its fence.
		bool Revoked = false;
		// Renderer-local leases pinning source-preview and replacement trees.
		std::array<uint64_t, MAX_PORTAL_CAPTURE_TREE_LEASES> Leases{};
		// Imported capture nodes in authenticated tree order.
		std::vector<ImportedPortalCaptureTreeNode> Nodes;
		// Authenticated edges connecting Nodes by index.
		std::vector<PortalCaptureTreeEdge> Edges;
		// CPU metadata footprint, excluding image payload bytes.
		size_t MetadataBytes = 0;
	};
}
