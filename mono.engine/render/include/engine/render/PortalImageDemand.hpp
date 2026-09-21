#pragma once

#include <engine/render/PortalImageImport.hpp>
#include <engine/render/Renderer.hpp>

namespace engine::render {
	// Pixel and recursion limits used while planning portal captures.
	struct PortalImageDemandSettings {
		// Requested capture width in pixels.
		uint32_t Width = 0;
		// Requested capture height in pixels.
		uint32_t Height = 0;
		// Maximum allowed width or height after demand planning.
		uint32_t MaximumExtent = MAX_PORTAL_IMAGE_EXTENT;
		// Maximum number of nested portal crossings to request.
		uint32_t RecursionDepth = 2;
		// Pixel ceiling shared by the root and recursive captures.
		uint32_t PixelBudget = MAX_PORTAL_IMAGE_PIXELS;
		// Whether to combine the player's retained body with the capture.
		bool ComposePlayerBody = false;
		// These admitted destinations render on this device at the requested camera.
		// Capture their complete world and body together instead of delayed body layers.
		std::span<const core::Name> ResidentDestinations{};
	};

	// One destination capture and its binding to a visible portal.
	struct PortalImageDemand {
		// Stable name of the receiving world.
		core::Name DestinationWorld;
		// Local portal view that will consume the result.
		PortalView Portal;
		// Copied request sent to the destination producer.
		PortalImageRequest Request;
		// Local renderer binding for the accepted image.
		PortalImageBinding Binding;
		// A radiance-only probe paired with a normal cross-world portal image.
		// It is submitted and imported independently, but never displayed as a pane.
		bool SeamRadiance = false;
	};

	// Planner outcomes for a candidate portal capture.
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

	// Counts of portal candidates by planning outcome.
	struct PortalImageDemandCounts {
		// Candidates admitted as capture requests.
		size_t Ready = 0;
		// Candidates not visible to this view.
		size_t Hidden = 0;
		// Candidates rejected for invalid input or geometry.
		size_t Invalid = 0;
		// Candidates using a profile the producer cannot capture.
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
