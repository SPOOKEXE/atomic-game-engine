#pragma once

// The RHI, such as it is at v0.7.
//
// Six passes — shadow, surface, opaque, transparent, overlay, interface — over
// a frustum-culled draw list. That is enough to prove the staged-shader path,
// the depth buffer, an offscreen target and the swapchain, and it is where the
// render graph at L9 will attach — the passes below become nodes, and this
// class becomes the backend they compile to.
//
// **The sixth draws nothing this module owns.** `interface` is a
// `FrameOverlayHook`, which is what lets `mono.engine/ui` record an editor's
// chrome into this frame without Dear ImGui appearing anywhere in the engine.
// A game runs five.
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
	// `graph::StandardPipeline` is the same six stages as data and
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

		// Whatever a `FrameOverlayHook` records — the editor's chrome, and
		// nothing a game ships with.
		//
		// **Last, and it has to be.** Everything above it draws the world; this
		// draws the thing you look at the world *through*. A pass that put
		// panels underneath geometry would be a window you cannot read.
		//
		// @since v0.7
		Interface,

		// Not a pass. The count, for the bitmask below.
		Count,
	};

	// The names of those stages, in the same order.
	//
	// `core::Name` rather than `const char *` for rule 4's reason and for the
	// comparison's: `graph::Stage::Name` is a `core::Name`, and comparing two of
	// them is an integer compare rather than a string one.
	//
	// @return The six names, valid for the life of the program.
	std::span<const core::Name> PassOrder();

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

		// How wide the texture is. Square is not required; a wide mirror wants a
		// wide target, and giving it a square one wastes half the texels.
		uint32_t Width = 1024;

		// How tall the texture is.
		uint32_t Height = 1024;
	};

	// Where in the window the world is drawn.
	//
	// **The editor's requirement, and it is a projection decision rather than a
	// crop.** A studio puts panels down the sides and along the bottom, so the
	// world occupies a rectangle that is neither the window's size nor its
	// shape. Drawing it full-window and letting the panels cover the edges
	// would project for an aspect ratio nothing is displayed at — a scene
	// stretched by however wide the explorer happens to be, changing every time
	// somebody drags a splitter.
	//
	// So the rectangle is passed in, the viewport and scissor are set from it,
	// and **the aspect ratio and the cull frustum both come from it** rather
	// than from the swapchain. Those two travelling together is the whole
	// point: `Renderer::Render`'s existing contract is that the frustum and the
	// projection are resolved once from one aspect, because two copies is two
	// chances to disagree and the symptom is geometry popping at the screen
	// edge on one machine and not another.
	//
	// Pixels from the top-left of the swapchain, matching every other
	// coordinate in this module. A default-constructed one is empty and means
	// the whole swapchain — which is what a game passes, by passing nothing.
	//
	// @since v0.7
	struct Viewport {
		// Left edge in pixels.
		int X = 0;

		// Top edge in pixels.
		int Y = 0;

		// Width in pixels. Zero means the whole swapchain.
		int Width = 0;

		// Height in pixels. Zero means the whole swapchain.
		int Height = 0;

		// Reports whether this names a rectangle at all.
		//
		// @return `true` when both dimensions are positive.
		bool IsValid() const {
			return Width > 0 && Height > 0;
		}
	};

	// The backend handles a hook needs to build its own pipelines.
	//
	// **Opaque on purpose.** `Device` is an `SDL_GPUDevice *` and `ColourFormat`
	// is an `SDL_GPUTextureFormat`, and neither name appears here — this header
	// says it holds no SDL GPU type and that sentence is load-bearing. A hook
	// casts them back, which is a cast a caller writes once in a file that
	// already includes SDL, rather than SDL appearing on the include line of
	// everything that draws.
	//
	// @since v0.7
	struct BackendHandles {
		// The GPU device, as an `SDL_GPUDevice *`. Null before Initialise.
		void *Device = nullptr;

		// The swapchain's colour format, as an `SDL_GPUTextureFormat`.
		//
		// A pipeline built against the wrong one is a validation error at
		// creation on some drivers and a corrupt image on others, which is why
		// it is handed over rather than guessed.
		uint32_t ColourFormat = 0;
	};

	// A layer that records into this renderer's frame.
	//
	// **This exists so that Dear ImGui is not in the engine.** An editor needs
	// its chrome inside the same command buffer as the world — SDL's GPU API
	// acquires a swapchain texture once per command buffer, so a second pass in
	// a second buffer is not an option — and the two ways to arrange that are
	// both worse than this one. Putting imgui in `render` puts it on every
	// shipping client's link line to draw nothing. Exposing `SDL_GPUCommandBuffer`
	// on `Render` breaks the rule at the top of this file.
	//
	// So the renderer hands over its command buffer and render pass as `void *`,
	// and `mono.engine/ui` is the only module in the repository that knows what
	// Dear ImGui is. The lost type safety is real and is the price; it is paid
	// at exactly two call sites, both inside one file.
	//
	// **`Prepare` is outside every render pass and `Record` is inside the last
	// one.** That split is not stylistic: uploading vertices is a copy pass, and
	// a copy pass cannot be started while a render pass is open. A hook that
	// uploaded from `Record` would work until the first frame with enough
	// widgets to grow its buffer.
	//
	// @since v0.7
	// @client
	class FrameOverlayHook {
	  public:
		virtual ~FrameOverlayHook() = default;

		// Uploads whatever this frame needs, before any render pass is open.
		//
		// @param commandBuffer The frame's `SDL_GPUCommandBuffer *`.
		// @return `false` to skip `Record` — nothing to draw, which is not an
		//         error and not a reason to fail the frame.
		virtual bool Prepare(void *commandBuffer) = 0;

		// Records draw commands into the swapchain.
		//
		// @param commandBuffer The frame's `SDL_GPUCommandBuffer *`.
		// @param renderPass    An open `SDL_GPURenderPass *` bound to the
		//                      swapchain with no depth attachment.
		virtual void Record(void *commandBuffer, void *renderPass) = 0;
	};

	// What one `Render` call submitted, and whether it reached the display.
	//
	// A default result means no frame was presented, including while the window
	// is minimised or resizing and when the renderer is unavailable.
	//
	// @since v0.1
	// @client
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
		// @param surface     The offscreen view to render first, or null.
		// @param interfaceHook An editor's chrome, drawn last, or null. See
		//                    `FrameOverlayHook` for why this is not imgui.
		// @param viewport    Where in the window the world is drawn, or null for
		//                    all of it. Decides the aspect ratio and the cull
		//                    frustum as well as the scissor — see `Viewport`.
		// @return Submitted draw counts and whether the frame was presented.
		FrameResult Render(
			const core::CFrame &cameraFrame,
			const scene::Camera &camera,
			std::span<const scene::DrawInstance> instances,
			OverlayImage &overlay,
			const SurfaceView *surface = nullptr,
			FrameOverlayHook *interfaceHook = nullptr,
			const Viewport *viewport = nullptr
		);

		// The device and swapchain format a `FrameOverlayHook` builds against.
		//
		// @return The handles, both empty before Initialise.
		BackendHandles Backend() const;

	  private:
		struct Impl;
		std::unique_ptr<Impl> State;
	};
}
