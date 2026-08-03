#pragma once

// The RHI, such as it is at v0.1.
//
// One instanced opaque pass and one overlay pass. That is enough to prove the
// staged-shader path, the depth buffer and the swapchain, and it is where the
// render graph at L9 will attach — the passes below become nodes, and this
// class becomes the backend they compile to.
//
// SDL's GPU API is the backend rather than Vulkan directly. The API can target
// Vulkan, Metal and D3D12, but v0.1 supplies SPIR-V and therefore requests the
// Vulkan path; the other backends need their platform shader formats first.
//
// No SDL GPU type appears here. The public surface is a window pointer, a
// camera, and a span of `scene::DrawInstance`.
//
// **This module does not describe a frame; it consumes one.** What crosses from
// simulation to presentation is `scene::DrawInstance` at L7, because a
// `server`-tier host writes it and a `client`-tier consumer reads it and a type
// only one of those tiers can name cannot be the thing they hand between them.
// There used to be a `render::Instance` and a `render::Camera` here, and the
// client converted into them; the conversion into a GPU layout now happens once,
// below, at the point of upload.
//
// @tier L12 · client

#include <engine/core/types/CFrame.hpp>
#include <engine/graph/Frustum.hpp>
#include <engine/render/Overlay.hpp>
#include <engine/scene/Components.hpp>
#include <engine/scene/DrawInstance.hpp>

#include <cstdint>
#include <memory>
#include <span>
#include <string_view>

struct SDL_Window;

namespace engine::render {

	// Work encoded by one Render call.
	//
	// A default result means no frame was presented, including while the window
	// is minimised or resizing and when the renderer is unavailable.
	//
	// @client
	struct FrameResult {
		// Whether SDL accepted a command buffer for presentation.
		bool Presented = false;

		// Number of opaque and overlay draw calls submitted for this frame.
		uint32_t DrawCalls = 0;

		// Number of opaque mesh triangles submitted for this frame.
		uint64_t Triangles = 0;

		// How many instances the frustum rejected.
		//
		// **Reported rather than inferred**, because the interesting number is
		// the ratio and the denominator is the caller's draw list — which the
		// caller has and this does not need to repeat. A camera framing its own
		// scene culls almost nothing and a camera inside a large world culls
		// almost everything; a reading that never moves means the frustum is
		// wrong, not that the scene is small.
		uint32_t Culled = 0;
	};

	// Owns the client GPU device, window claim, pipelines, and per-frame upload resources.
	//
	// @client
	class Renderer {
	  public:
		// Creates an uninitialised renderer with no GPU resources.
		Renderer();

		// Shuts down the renderer and releases all GPU resources.
		~Renderer();

		// Renderers cannot share ownership of a device or claimed window.
		Renderer(const Renderer &) = delete;

		// Renderers cannot share ownership of a device or claimed window.
		Renderer &operator=(const Renderer &) = delete;

		// Creates the device and claims the window. Returns false and logs the
		// reason; the caller decides whether that is fatal, because a headless
		// test run legitimately has no GPU.
		//
		// The renderer does not own `window`; it must remain alive until Shutdown
		// or destruction releases the GPU claim.
		//
		// @param window SDL window to claim for the GPU device; must not be null.
		// @return True when the device, pipelines, geometry, and window claim are ready.
		bool Initialise(SDL_Window *window);

		// Waits for GPU work, releases the window claim and resources, and becomes uninitialised.
		//
		// Calling this on an uninitialised renderer has no effect.
		void Shutdown();

		// Reports whether Initialise completed and a GPU device is available.
		bool IsInitialised() const;

		// "vulkan", "metal", "direct3d12". Shown in the F3 panel, because the
		// first question about a performance report is which backend produced
		// it.
		//
		// The returned view belongs to the renderer and is invalidated by Shutdown
		// or destruction. It is empty before successful initialisation.
		std::string_view BackendName() const;

		// Off presents without waiting for vblank, which is what makes a frame
		// time measure the engine rather than the display. Returns false, and
		// stays as it was, when the backend has no unsynchronised mode.
		//
		// This is on the renderer rather than something the caller does to the
		// window because the swapchain belongs to the GPU device, and the
		// device is behind the pimpl.
		//
		// @param enabled True to wait for vertical blank; false to request immediate presentation.
		// @return True when the requested mode was supported and applied.
		bool SetVerticalSync(bool enabled);

		// Draws one frame and presents it. Returns false in Presented when the
		// swapchain had no texture — minimised, or resizing — which is not an
		// error and not a reason to stop ticking. It is also false if SDL rejects
		// command submission.
		//
		// `overlay` is uploaded only when it has something pending, and drawn
		// whenever it has content — the texture holds the last thing sent to it,
		// so a caller may redraw the overlay far less often than it presents.
		// The renderer marks the overlay uploaded once it has recorded the copy,
		// which is the only reason it is not a const reference; nothing else
		// about it is retained past the call.
		//
		// The camera and instances are copied during the call and not retained.
		//
		// The aspect ratio comes from the swapchain texture this call acquired
		// rather than from a caller, so a frame taken during a resize is
		// projected for the image it is actually drawn into. `scene::Camera`
		// therefore holds no aspect ratio and `scene::ResolveCamera` takes one.
		//
		// @param cameraFrame World-space placement of the camera.
		// @param camera      Field of view and clipping distances.
		// @param instances   What to draw, as the world described it. Each is
		//                    turned into a model matrix and a colour here.
		// @param overlay     CPU premultiplied RGBA8 overlay. Uploaded only when
		//                    it has a pending region, drawn whenever it has
		//                    content, and marked uploaded on the way out.
		// @return Submitted draw counts and whether the frame was presented.
		FrameResult Render(
			const core::CFrame &cameraFrame,
			const scene::Camera &camera,
			std::span<const scene::DrawInstance> instances,
			OverlayImage &overlay
		);

	  private:
		struct Impl;
		std::unique_ptr<Impl> State;
	};
}
