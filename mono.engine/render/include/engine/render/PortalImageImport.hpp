#pragma once

#include <engine/core/Name.hpp>
#include <engine/render/PortalExchange.hpp>

#include <glm/mat4x4.hpp>

namespace engine::render {
	inline constexpr size_t MAX_IMPORTED_PORTAL_IMAGES = 16;
	inline constexpr size_t MAX_IMPORTED_PORTAL_TEXTURE_BYTES = 32 * 1024 * 1024;
	inline constexpr size_t MAX_IMPORTED_PORTAL_CPU_BYTES = 32 * 1024 * 1024;
	inline constexpr size_t MAX_IMPORTED_PORTAL_STAGING_BYTES = MAX_PORTAL_IMAGE_PIXELS * 12;

	// Local ownership accompanies an already authenticated reply. No world or bus
	// pointer enters the renderer. Expected is the complete outstanding wire key.
	struct PortalImageBinding {
		uint64_t World = 0;
		core::Name WorldName;
		size_t ViewSlot = 0;
		core::Name Portal;
		int16_t Index = 0;
		PortalExchangeKey Expected;
		PortalImageScope ExpectedScope = PortalImageScope::CompleteWorld;
		PortalImageProjection ExpectedProjection = PortalImageProjection::Seam;
		// Seam: source-world positions to capture clip space through the seam mapping.
		// Eye: destination-world positions to capture clip space. In both projections,
		// clip w is camera-forward distance in destination units, matching paired depth.
		glm::mat4 Sampling{1};
		// Local role within one portal: zero is the base image, one and two are
		// ordered transparent layers. This renderer ordinal never crosses the wire.
		uint8_t Layer = 0;
	};

	// Logical payload residency, including vector capacity and the upload staging
	// allocation. Upload totals count bytes actually recorded at the graph boundary.
	struct PortalImageImportUsage {
		size_t Images = 0;
		size_t PendingCpuBytes = 0;
		size_t TextureBytes = 0;
		// Replaced resident exports retained for reuse, separate from live images.
		size_t CachedTextureBytes = 0;
		size_t StagingBytes = 0;
		uint64_t Uploads = 0;
		uint64_t UploadedBytes = 0;
		uint64_t Reuses = 0;
	};
}
