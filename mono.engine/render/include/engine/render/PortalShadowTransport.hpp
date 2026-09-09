#pragma once

#include <engine/core/types/AABB.hpp>
#include <engine/render/PortalShadowImage.hpp>

#include <optional>

namespace engine::render {
	inline constexpr uint8_t PORTAL_SHADOW_MANIFEST_PART = 0;
	inline constexpr uint8_t PORTAL_SHADOW_CANCEL_PART = PORTAL_SHADOW_TILE_COUNT + 1;

	// ParentEye selects an already delivered route. Only the authenticated presentation
	// envelope identifies its requester; no address in these values grants authority.
	struct PortalShadowPull {
		PortalExchangeKey ParentEye;
		PortalCaptureTreeEndpoint TargetProducer;
		PortalExchangeKey TargetEye;
		// The authenticated requester may supply its copied body bounds so the source
		// fits a map that covers both worlds. Empty keeps the source-only map.
		std::optional<core::AABB> BodyBounds;
		// Zero requests the manifest, 1..16 request tiles, 17 cancels the parent route.
		uint8_t Part = PORTAL_SHADOW_MANIFEST_PART;
		bool operator==(const PortalShadowPull &) const = default;
	};
	struct PortalShadowPacket {
		PortalShadowPull Pull;
		PortalImageStatus Status = PortalImageStatus::Unavailable;
		// Successful manifest/tile payloads use SHDW verbatim. Cancellation and failures are empty.
		std::vector<std::byte> Payload;
		bool operator==(const PortalShadowPacket &) const = default;
	};

	bool EncodePortalShadowPull(const PortalShadowPull &, std::vector<std::byte> &, std::string &error);
	bool DecodePortalShadowPull(std::span<const std::byte>, PortalShadowPull &, std::string &error);
	bool EncodePortalShadowPacket(const PortalShadowPacket &, std::vector<std::byte> &, std::string &error);
	bool DecodePortalShadowPacket(std::span<const std::byte>, PortalShadowPacket &, std::string &error);
	// Header discriminator only. Decode and exact pending-pull/envelope matching remain mandatory.
	bool IsPortalShadowPacket(std::span<const std::byte>);
}
