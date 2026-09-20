#pragma once

#include <engine/core/types/AABB.hpp>
#include <engine/render/PortalShadowImage.hpp>

#include <optional>

namespace engine::render {
	// Part index that requests the shadow manifest rather than a tile.
	inline constexpr uint8_t PORTAL_SHADOW_MANIFEST_PART = 0;
	// Part index that cancels the parent shadow route.
	inline constexpr uint8_t PORTAL_SHADOW_CANCEL_PART = PORTAL_SHADOW_TILE_COUNT + 1;

	// ParentEye selects an already delivered route. Only the authenticated presentation
	// envelope identifies its requester; no address in these values grants authority.
	struct PortalShadowPull {
		// Previously delivered parent eye whose route this pull follows.
		PortalExchangeKey ParentEye;
		// Producer selected at the target capture-tree endpoint.
		PortalCaptureTreeEndpoint TargetProducer;
		// Eye key whose source shadow is requested.
		PortalExchangeKey TargetEye;
		// The authenticated requester may supply its copied body bounds so the source
		// fits a map that covers both worlds. Empty keeps the source-only map.
		std::optional<core::AABB> BodyBounds;
		// Zero requests the manifest, 1..16 request tiles, 17 cancels the parent route.
		uint8_t Part = PORTAL_SHADOW_MANIFEST_PART;
		// Compares route identity, body bounds and requested part.
		bool operator==(const PortalShadowPull &) const = default;
	};
	// Reply to one manifest, tile or cancellation pull.
	struct PortalShadowPacket {
		// Pull whose authenticated route this packet answers.
		PortalShadowPull Pull;
		// Producer outcome for the requested part.
		PortalImageStatus Status = PortalImageStatus::Unavailable;
		// Successful manifest/tile payloads use SHDW verbatim. Cancellation and failures are empty.
		std::vector<std::byte> Payload;
		// Compares the answered pull, status and owned bytes.
		bool operator==(const PortalShadowPacket &) const = default;
	};

	// Encodes a bounded shadow pull for the presentation bus.
	bool EncodePortalShadowPull(const PortalShadowPull &, std::vector<std::byte> &, std::string &error);
	// Decodes a bounded shadow pull, leaving output unchanged on failure.
	bool DecodePortalShadowPull(std::span<const std::byte>, PortalShadowPull &, std::string &error);
	// Encodes a shadow status and optional SHDW payload.
	bool EncodePortalShadowPacket(const PortalShadowPacket &, std::vector<std::byte> &, std::string &error);
	// Decodes a shadow packet before route and envelope matching.
	bool DecodePortalShadowPacket(std::span<const std::byte>, PortalShadowPacket &, std::string &error);
	// Header discriminator only. Decode and exact pending-pull/envelope matching remain mandatory.
	bool IsPortalShadowPacket(std::span<const std::byte>);
}
