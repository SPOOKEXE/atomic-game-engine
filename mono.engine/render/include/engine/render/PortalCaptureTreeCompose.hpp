#pragma once

#include <engine/core/types/AABB.hpp>
#include <engine/render/PortalShadowImage.hpp>

namespace engine::render {
	// Outcomes while preparing or composing a captured portal tree.
	enum class PortalTreeCompositionStatus : uint8_t { Pending, BudgetExceeded, Invalid, Complete };

	// The host authenticates the source response against this accepted eye identity.
	struct PortalTreeShadowRequest {
		// Renderer-local composition job awaiting a source shadow.
		uint64_t Job = 0;
		// Capture-tree node that needs the shadow.
		uint32_t Node = 0;
		// Authenticated producer expected to answer this node.
		PortalCaptureTreeEndpoint Producer;
		// Accepted eye capture paired with this shadow request.
		PortalExchangeKey Eye;
		// World tick of the accepted eye capture.
		uint64_t CaptureTick = 0;
		// Content revision of the accepted eye capture.
		uint64_t ContentRevision = 0;
		// Lighting revision of the accepted eye capture.
		uint64_t LightingRevision = 0;
		// Digest of the accepted eye pixels used for exact matching.
		assets::ContentHash EyePixelHash;
		// Player identity excluded from the source shadow, when authorized.
		std::string ExcludedPlayer;
		// Empty when this node has neither body nor aperture rows.
		std::optional<core::AABB> BodyBounds;
		// From this node's accepted eye lighting, never from the current world revision.
		core::Vector3 LightDirection{};
	};
	// Matches decoded metadata only. The host must separately authenticate the envelope
	// and ensure this is still the pending job/node before beginning tile assembly.
	bool MatchesPortalTreeShadowRequest(const PortalTreeShadowRequest &, const PortalShadowSnapshot &);

	// Progress transfer for a pending tree composition or preparation.
	struct PortalTreeCompositionProgress {
		// Current job outcome.
		PortalTreeCompositionStatus Status = PortalTreeCompositionStatus::Invalid;
		// Next source shadow to fetch, when the job is waiting.
		std::optional<PortalTreeShadowRequest> Request = {};
		// Complete transfers this renderer-local image to the caller exactly once.
		uint64_t Image = 0;
	};
}
