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
	// `PassThrough` and `Reflection` remain reserved until a probe can name a
	// specific event. Portal seam radiance is a real transport field, but is not
	// attributable to one local light. The local-light builder emits only
	// `EmptySpace` and `Termination`.
	enum class LightProbeEvent : uint8_t {
		EmptySpace,
		PassThrough,
		Reflection,
		Termination,
	};

	// One overlay line and the event that chose its colour.
	struct LightProbeSegment {
		AdornmentLine Line;
		LightProbeEvent Event = LightProbeEvent::EmptySpace;
	};

	// Bounded sample paths from resolved local lights through render bounds.
	//
	// Build reuses its storage. A probe reports the nearest draw-instance AABB
	// it meets, which is intentionally conservative and can differ from a mesh
	// silhouette or the shadow map.
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
