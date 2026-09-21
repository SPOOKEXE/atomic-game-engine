#pragma once
#include <engine/render/PortalExchange.hpp>
namespace engine::render {
	struct PortalLayerMeasure {
		uint32_t Width = 0, Height = 0;
		uint64_t RequestId = 0, CameraRevision = 0, SeamRevision = 0;
		std::string_view PortalKey;
		PortalImageReplyMatch Match;
		size_t ProgramBytes = 0, ProgramCount = 0;
		std::array<assets::ContentHash, MAX_PORTAL_CAPTURE_LENSES> ProgramHashes{};
	};
	// Borrows the layer envelope without decoding images or allocating owned metadata.
	bool PreflightPortalLayers(std::span<const std::byte> bytes, PortalLayerMeasure &out);
}
