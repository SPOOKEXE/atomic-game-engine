#pragma once

#include <engine/core/types/AABB.hpp>
#include <engine/render/PortalShadowImage.hpp>

namespace engine::render {
	enum class PortalTreeCompositionStatus : uint8_t { Pending, BudgetExceeded, Invalid, Complete };

	// The host authenticates the source response against this accepted eye identity.
	struct PortalTreeShadowRequest {
		uint64_t Job = 0;
		uint32_t Node = 0;
		PortalCaptureTreeEndpoint Producer;
		PortalExchangeKey Eye;
		uint64_t CaptureTick = 0, ContentRevision = 0, LightingRevision = 0;
		assets::ContentHash EyePixelHash;
		std::string ExcludedPlayer;
		// Empty when this node has neither body nor aperture rows.
		std::optional<core::AABB> BodyBounds;
		// From this node's accepted eye lighting, never from the current world revision.
		core::Vector3 LightDirection{};
	};
	// Matches decoded metadata only. The host must separately authenticate the envelope
	// and ensure this is still the pending job/node before beginning tile assembly.
	bool MatchesPortalTreeShadowRequest(const PortalTreeShadowRequest &, const PortalShadowSnapshot &);

	struct PortalTreeCompositionProgress {
		PortalTreeCompositionStatus Status = PortalTreeCompositionStatus::Invalid;
		std::optional<PortalTreeShadowRequest> Request = {};
		// Complete transfers this renderer-local image to the caller exactly once.
		uint64_t Image = 0;
	};
}
