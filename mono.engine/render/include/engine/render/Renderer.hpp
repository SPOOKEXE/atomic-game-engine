#pragma once

// The RHI, such as it is at v0.6.
//
// Five passes — shadow, surface, opaque, transparent, overlay — over a
// frustum-culled draw list. That is enough to prove the staged-shader path, the
// depth buffer, an offscreen target and the swapchain, and it is where the
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

	// The stages `Render` submits, in submission order.
	//
	// **This exists so that the two descriptions of a frame can be compared.**
	// `graph::StandardPipeline` is the same five stages as data and
	// `Pipeline::Validate` checks them for a stage reading what nothing wrote —
	// but until v0.6 nothing tied that list to the function that actually draws,
	// so keeping them in step was a sentence in `AGENTS.md`. A sentence is
	// documentation; `tests/Passes.cpp` compares this enum's names against that
	// pipeline's, in order, and fails the build when they disagree.
	//
	// **What it does not do is make the frame run from data.** That is the
	// render-node system, and this is deliberately not a small version of it —
	// two competing executors would be worse than the one hand-rolled list. What
	// is checked is the *description*: a sixth stage added to the pipeline and
	// not to this enum, or the reverse, stops the build. Adding a pass without
	// entering it through `PassRecorder` is still possible and is the hole this
	// leaves open, named here rather than implied. See `D00016`.
	//
	// @since v0.6
	enum class Pass : uint8_t {
		// Depth from the light, over the whole scene rather than the culled set.
		Shadow,

		// The surface camera's view, into a texture a mirror samples.
		Surface,

		// The screen, with depth written.
		Opaque,

		// Blended, depth-tested and not depth-written. **Its own stage even
		// though it shares a render pass with `Opaque`** — a separate pass would
		// have to reload the depth buffer — because what the list describes is
		// what is drawn and in what order, not how many times a target is bound.
		Transparent,

		// The overlay texture, loaded over the frame rather than clearing it.
		Overlay,

		// Not a pass. The count, for the bitmask below.
		Count,
	};

	// The names of those stages, in the same order.
	//
	// `core::Name` rather than `const char *` for rule 4's reason and for the
	// comparison's: `graph::Stage::Name` is a `core::Name`, and comparing two of
	// them is an integer compare rather than a string one.
	//
	// @return The five names, valid for the life of the program.
	std::span<const core::Name> PassOrder();

	// Work encoded by one Render call.
	//
	// A default result means no frame was presented, including while the window
	// is minimised or resizing and when the renderer is unavailable.
	//
	// @client
	// A second view, rendered into a texture instead of the swapchain.
	//
	// **`world::ViewChannel`'s shape, with a texture on the far end.** v0.2
	// built the seam for a hosted world publishing a view to a client; this is
	// the same idea with the consumer inside the process — and the one-frame
	// staleness that design already assumed is what makes a mirror cheap: the
	// surface shows the frame *before* the one being drawn, so there is no
	// dependency cycle between a mirror and what it reflects.
	//
	// One per frame. Several would be several passes and a texture array, which
	// is the render-node system's job rather than this pipeline's.
	//
	// @since v0.6
	struct SurfaceView {
		// Where the surface camera is, in world space.
		core::CFrame Frame;

		// Its field of view and clipping distances.
		scene::Camera Lens;

		// How big the texture is. Square is not required; a wide mirror wants a
		// wide target, and giving it a square one wastes half the texels.
		uint32_t Width = 1024;
		uint32_t Height = 1024;
	};

	struct FrameResult {
		// Whether SDL accepted a command buffer for presentation.
		bool Presented = false;

		// Number of opaque and overlay draw calls submitted for this frame.
		uint32_t DrawCalls = 0;

		// Number of opaque mesh triangles submitted for this frame.
		uint64_t Triangles = 0;

		// How many instances showed a surface texture.
		uint32_t SurfaceInstances = 0;

		// How many instances the frustum rejected.
		//
		// **Reported rather than inferred**, because the interesting number is
		// the ratio and the denominator is the caller's draw list — which the
		// caller has and this does not need to repeat. A camera framing its own
		// scene culls almost nothing and a camera inside a large world culls
		// almost everything; a reading that never moves means the frustum is
		// wrong, not that the scene is small.
		uint32_t Culled = 0;

		// One bit per `Pass` that submitted work this frame.
		//
		// **Every pass is skippable and most of them usually are**, so "the
		// shadow pass exists" and "the shadow pass ran" are different questions,
		// and only the second one explains a frame with no shadows in it. The
		// draw-call count cannot answer it — the shadow pass and the overlay
		// pass are one draw each and look identical from there.
		//
		// @since v0.6
		uint8_t Passes = 0;

		// Whether one pass submitted work this frame.
		//
		// @param pass Which one.
		// @return True when it ran.
		bool Ran(Pass pass) const {
			return (Passes & static_cast<uint8_t>(1u << static_cast<uint8_t>(pass))) != 0;
		}
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
			OverlayImage &overlay,
			const SurfaceView *surface = nullptr
		);

	  private:
		struct Impl;
		std::unique_ptr<Impl> State;
	};
}
