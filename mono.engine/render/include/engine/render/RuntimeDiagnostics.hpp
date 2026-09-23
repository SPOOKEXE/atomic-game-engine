#pragma once

// Headless geometry for renderer diagnostics.
//
// These paths sample resolved local-light finite volumes. Portal seam radiance
// is carried by a separate bounded field and does not identify one local-light
// trajectory, so it is not represented here. The probes are deterministic
// diagnostics of authored influence volumes, not a record of renderer rays.
//
// @tier L12 · client

#include <engine/render/AdornmentGeometry.hpp>
#include <engine/render/Renderer.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace engine::render {

	// What one light-influence probe segment represents.
	//
	// `PassThrough` marks sampled bounds that the shadow pass does not draw:
	// transparent or refractive rows and rows with CastShadow disabled. It is
	// a conservative bounds diagnostic, not a per-pixel transmission result.
	// `Reflection` remains reserved: mirror cameras do not bounce local light.
	enum class LightProbeEvent : uint8_t {
		EmptySpace,
		PassThrough,
		Reflection,
		Termination,
	};

	// One overlay line and the event that chose its colour.
	struct LightProbeSegment {
		AdornmentLine Line;									 // Visible sampled path segment.
		LightProbeEvent Event = LightProbeEvent::EmptySpace; // Event at this segment.
	};

	// Bounded sample paths from resolved local lights through render bounds.
	//
	// Build reuses its storage. Each probe traverses at most 16 nearest
	// pass-through bounds, then stops at the first opaque shadow-casting AABB or
	// the light range. Alpha-masked opaque rows remain conservative blockers.
	// These sampled bounds can differ from mesh silhouettes and shadow maps.
	class LightPathGeometry {
	  public:
		// Rebuilds probe segments for the supplied resolved lights and draw rows.
		void Build(std::span<const SceneLight> lights, std::span<const scene::DrawInstance> instances);

		// The lines valid until the next Build call.
		std::span<const LightProbeSegment> Segments() const {
			return Paths;
		}

	  private:
		std::vector<LightProbeSegment> Paths;
	};

	// Culls draw rows against one diagnostic camera pose.
	//
	// The result preserves source order. Hosts use it to submit a frozen visible
	// set while projecting the surviving rows through a separate inspection
	// camera.
	size_t CullForCamera(
		std::span<const scene::DrawInstance> instances,
		const core::CFrame &cameraFrame,
		const scene::Camera &camera,
		float aspectRatio,
		std::vector<uint32_t> &visible
	);

	// Appends a wire camera marker to the existing overlay line path.
	//
	// `distance` is deliberately supplied by the host: a full far plane makes a
	// useful marker disappear in large worlds.
	void AppendCameraLockAdornment(
		std::vector<AdornmentLine> &lines,
		const core::CFrame &frame,
		const scene::Camera &camera,
		float aspectRatio,
		float distance
	);
}
