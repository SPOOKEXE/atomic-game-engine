#pragma once

#include <engine/core/Name.hpp>
#include <engine/render/PortalCaptureTree.hpp>

#include <glm/mat4x4.hpp>

namespace engine::render {
	// Keep old and replacement trees, the widest bottom-up composition frontier,
	// and the preceding composed root. Image bytes retain their separate budget.
	inline constexpr size_t MAX_IMPORTED_PORTAL_IMAGES =
		2 * MAX_PORTAL_CAPTURE_TREE_NODES * (MAX_PORTAL_TRANSPARENT_LAYERS + 2) +
		MAX_PORTAL_CAPTURE_TREE_NODES + 1;
	// Maximum live GPU payload bytes held by imported portal images.
	inline constexpr size_t MAX_IMPORTED_PORTAL_TEXTURE_BYTES = 32 * 1024 * 1024;
	// Maximum CPU bytes retained by decoded and incomplete image imports.
	inline constexpr size_t MAX_IMPORTED_PORTAL_CPU_BYTES = 32 * 1024 * 1024;
	// Maximum temporary upload bytes held while importing one portal image group.
	inline constexpr size_t MAX_IMPORTED_PORTAL_STAGING_BYTES = MAX_PORTAL_IMAGE_PIXELS * 64;

	// Local ownership accompanies an already authenticated reply. No world or bus
	// pointer enters the renderer. Expected is the complete outstanding wire key.
	struct PortalImageBinding {
		// Process-local world handle that owns the pending portal request.
		uint64_t World = 0;
		// Stable world identity used to authenticate the imported reply.
		core::Name WorldName;
		// Renderer view slot that requested the image group.
		size_t ViewSlot = 0;
		// Stable portal identity within WorldName.
		core::Name Portal;
		// Signed-eye ordinal within the portal request.
		int16_t Index = 0;
		// Complete wire key that the reply must match exactly.
		PortalExchangeKey Expected;
		// Expected complete-world or partial image scope.
		PortalImageScope ExpectedScope = PortalImageScope::CompleteWorld;
		// Expected seam or eye coordinate projection.
		PortalImageProjection ExpectedProjection = PortalImageProjection::Seam;
		// Seam: source-world positions to capture clip space through the seam mapping.
		// Eye: destination-world positions to capture clip space. In both projections,
		// clip w is camera-forward distance in destination units, matching paired depth.
		glm::mat4 Sampling{1};
		// Local role within one portal: zero is the base image, one and two are
		// ordered transparent layers, and three is spatial overlay. This renderer
		// ordinal never crosses the wire; overlay imports require a complete group.
		// Renderer-local image-layer role within the imported portal group.
		uint8_t Layer = 0;
	};

	// Logical payload residency, including vector capacity and the upload staging
	// allocation. Upload totals count bytes actually recorded at the graph boundary.
	struct PortalImageImportUsage {
		// Number of resident imported image groups.
		size_t Images = 0;
		// CPU bytes reserved by incomplete image imports.
		size_t PendingCpuBytes = 0;
		// Live GPU import payload, including packed shadow storage buffers.
		size_t TextureBytes = 0;
		// Replaced resident exports retained for reuse, separate from live images.
		size_t CachedTextureBytes = 0;
		// CPU upload-buffer bytes currently retained by imports.
		size_t StagingBytes = 0;
		// Number of GPU upload operations recorded for imports.
		uint64_t Uploads = 0;
		// GPU payload bytes recorded by those upload operations.
		uint64_t UploadedBytes = 0;
		// Number of imports satisfied by a resident image reuse.
		uint64_t Reuses = 0;
	};
}
