#pragma once

#include <engine/render/PortalImageImport.hpp>
#include <engine/render/Renderer.hpp>

namespace engine::render {
	struct PortalImageDemandSettings {
		uint32_t Width = 0;
		uint32_t Height = 0;
		uint32_t MaximumExtent = MAX_PORTAL_IMAGE_EXTENT;
		uint32_t RecursionDepth = 2;
		uint32_t PixelBudget = MAX_PORTAL_IMAGE_PIXELS;
		bool ComposePlayerBody = false;
	};

	struct PortalImageDemand {
		core::Name DestinationWorld;
		PortalView Portal;
		PortalImageRequest Request;
		PortalImageBinding Binding;
	};

	enum class PortalDemandStatus { Ready, Hidden, Invalid, Unsupported };

	// The pose is already in the eye's destination world. Builds an owned camera
	// demand without source geometry or a seam plane. Sampling maps destination-world
	// positions to the capture clip space. Unchanged output on refusal.
	PortalDemandStatus BuildPortalEyeDemand(
		core::Name cameraKey,
		const View &eye,
		const PortalImageDemandSettings &settings,
		PortalImageDemand &demand
	);

	// Maps source-owned ordinary rows crossing directly into destination. Camera
	// culling is deliberately absent: an eye beyond the mouth can see its far half.
	// An eye in the source's authored world carries the live or held local body
	// in source coordinates so its producer can forward that pose into child views.
	// Foreign rows and local synthetic copies are omitted. Output is transactional;
	// no crossing geometry produces an empty payload, not an encoded empty picture.
	bool CollectPortalEyeGeometry(
		ecs::Store &store,
		core::Name destination,
		std::span<const scene::DrawInstance> instances,
		std::span<const core::CFrame> joints,
		std::vector<std::byte> &out,
		std::string &error
	);

	struct PortalImageDemandCounts {
		size_t Ready = 0;
		size_t Hidden = 0;
		size_t Invalid = 0;
		size_t Unsupported = 0;
	};

	// Gather authored mouths without aiming or changing cameras. Appends cross-world
	// claims to portals so CollectSurfaceViews cannot substitute a local reflection.
	// Replaces demands. Explicit slots must match those applied to the view's rows.
	// Duplicate full paths are refused because a wire name cannot distinguish them.
	// Viewer rows must belong to store. Carries clipped crossing rows and their
	// referenced skin palettes; foreign-world rows must be appended afterward.
	// Invalid or over-budget geometry refuses the demand rather than omitting limbs.
	PortalImageDemandCounts CollectPortalImageDemands(
		ecs::Store &store,
		const View &viewer,
		const PortalImageDemandSettings &settings,
		std::vector<PortalImageDemand> &demands,
		std::vector<PortalView> &portals,
		std::span<const scene::SurfaceSlot> slots = {}
	);

	// Converts one gathered cross-world seam for this exact viewer. No store or
	// camera is changed. The destination receives values; local entity IDs stay here.
	// Output is unchanged unless Ready. Unsupported profiles must remain visible
	// as missing images, never fall back to rendering the source world in the mouth.
	PortalDemandStatus BuildPortalImageDemand(
		const scene::PortalSeam &seam,
		core::Name portalKey,
		const View &viewer,
		size_t viewSlot,
		const PortalImageDemandSettings &settings,
		PortalImageDemand &demand
	);
}
