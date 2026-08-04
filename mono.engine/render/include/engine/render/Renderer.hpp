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
#include <filesystem>
#include <memory>
#include <span>
#include <string_view>
#include <thread>

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

	// An offscreen colour target the world is drawn into instead of the window.
	//
	// **The editor's requirement, and the transparent-window trick it replaced
	// could not work.** A studio wants the world inside a dockable panel, and
	// the obvious cheap answer — draw the world to the swapchain and put a
	// background-less imgui window over it — fails the moment that window is
	// actually docked. `imgui.cpp`'s `central_node_hole` is only punched while
	// the central node is *empty*, so docking a panel into it makes the
	// dockspace fill its whole rectangle with `ImGuiCol_WindowBg` and paint over
	// the frame. The hole is a hole, not a panel.
	//
	// So the world goes into a texture and the panel shows it. That is what
	// every editor does, and it buys more than the fix: the viewport becomes an
	// ordinary window that docks, floats, resizes and closes like every other,
	// and several views become a matter of several targets rather than a second
	// arrangement of the window.
	//
	// The texture is the swapchain's format, because the pipelines that draw
	// into it were built against that format, and it is sampleable, because
	// something has to show it.
	//
	// @since v0.7
	struct SceneTarget {
		// How wide the texture is, in pixels. Zero draws to the window instead.
		uint32_t Width = 0;

		// How tall the texture is, in pixels. Zero draws to the window instead.
		uint32_t Height = 0;

		// Reports whether this asks for an offscreen target at all.
		//
		// @return `true` when both dimensions are positive.
		bool IsValid() const {
			return Width > 0 && Height > 0;
		}
	};

	// How much of a slot's texture the world was actually drawn into.
	//
	// **The texture is bigger than the panel on purpose, and this is how a
	// caller finds the part that is the picture.** A target allocated to the
	// panel's exact size has to be destroyed and created again on every frame
	// of a drag — and worse than the allocation, the *new* texture is not the
	// one the interface already recorded a bind of, so the panel spends the
	// whole drag showing the previous frame's image stretched to a rectangle it
	// was never drawn for. Rounding the allocation up to a block keeps one
	// texture alive across the whole drag, which means the image the panel
	// samples is the one this frame drew.
	//
	// What that costs is a border of pixels nothing draws into, and what it
	// needs is for whoever shows the texture to sample only the corner that is
	// the world. These are those coordinates: `(0, 0)` to `(U, V)`.
	//
	// @since v0.7
	struct SceneExtent {
		// The right edge of the drawn region, as a fraction of the texture.
		float U = 1.0f;

		// The bottom edge of the drawn region, as a fraction of the texture.
		float V = 1.0f;
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
		// **A null window is headless, and is not an error.** There is then no
		// swapchain, nothing is presented, and the overlay and interface passes
		// do not run — but the world is still drawn, into the `SceneTarget` a
		// caller passes to `Render`. That is what makes a build server, a golden
		// image comparison and a scripted editor possible without a display,
		// and a headless `Render` with no scene target draws nothing rather than
		// pretending to.
		//
		// @param window SDL window to claim, or null for headless.
		// @return True when the device, pipelines and geometry are ready.
		bool Initialise(SDL_Window *window);

		// Whether this renderer has a window to present to.
		//
		// @return `true` when `Initialise` was given none.
		bool IsHeadless() const;

		// Waits for GPU work, releases the window claim and resources, and becomes uninitialised.
		//
		// Calling this on an uninitialised renderer has no effect.
		void Shutdown();

		// Waits for the display and claims this frame's image, before the caller
		// has read a single event.
		//
		// **Optional, and the reason to call it is latency rather than
		// throughput.** `Render` waits for the swapchain itself when this was not
		// called, so an existing loop is correct without it and measures the same.
		// What it cannot be is *responsive*: the wait is the better part of a
		// frame with vertical sync on, and a loop that pumps events and then
		// waits has already read its input by the time it starts waiting — so
		// every frame is drawn from input one frame old, no matter how fast the
		// code between them is. Calling this first moves the dead time to before
		// the input is read.
		//
		// The frame is held until the next `Render` consumes it. Calling this
		// twice in one frame is safe and acquires once; not calling `Render`
		// afterwards submits an empty frame at `Shutdown` rather than leaking it.
		//
		// **This is why the split exists at all rather than `Render` simply
		// waiting later**: a swapchain image cannot be acquired without a command
		// buffer, and the wait is the acquisition. There is no ordering inside
		// one call that puts it after the caller's input.
		//
		// @return `false` when there was nothing to acquire — minimised or
		//         mid-resize, which is not an error and not a reason to stop
		//         ticking. Headless always succeeds; it waits for nothing.
		bool WaitForFrame();

		// Reports whether the caller is the thread that owns this renderer.
		//
		// **Recording is single-threaded by contract, and this is the contract
		// rather than a note about it.** v0.7 decided that a studio with several
		// viewports draws them one after another — the passes share one command
		// buffer and one device, so parallel recording would serialise at submit
		// and buy nothing, and it would cost the property that makes a viewport
		// correct: the world pass writes a scene target before the interface pass
		// samples it *in the same buffer*. That decision was written down and
		// nothing enforced it, which is the state this engine has twice found a
		// stale claim in.
		//
		// The owner is the thread that called `Initialise`, or the constructing
		// thread before that — a renderer is often built by whoever owns the
		// object and initialised by whoever owns the window, and it is the
		// device rather than the C++ object that this is about. There is
		// deliberately no `BindToCallingThread` beside `ecs::Store`'s: a store
		// is picked up by a different worker every tick and a device is not, so
		// a public rebind would be a seam for exactly what the contract forbids.
		//
		// @return `true` when the current thread may record and submit.
		// @threadsafe
		bool IsOnOwningThread() const;

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
		// @param sceneTarget Draw the world into an offscreen texture of this
		//                    size instead of into the window, or null for the
		//                    window. Decides the aspect ratio and the cull
		//                    frustum as well as the target — see `SceneTarget`.
		// @return Submitted draw counts and whether the frame was presented.
		FrameResult Render(
			const core::CFrame &cameraFrame,
			const scene::Camera &camera,
			std::span<const scene::DrawInstance> instances,
			OverlayImage &overlay,
			const SurfaceView *surface = nullptr,
			FrameOverlayHook *interfaceHook = nullptr,
			const SceneTarget *sceneTarget = nullptr,
			size_t targetSlot = 0
		);

		// The texture the most recent `Render` drew that slot's world into.
		//
		// **Slots exist because an editor has more than one viewport.** A game
		// draws one view of one world and only ever uses slot 0. A studio
		// showing a server's world beside a client's keeps a target per panel —
		// and they must be *separate* targets, because the panels are different
		// sizes and a single shared one would be reallocated twice a frame as
		// each panel asked for its own dimensions. That reallocation is
		// measurable: it is a colour and a depth texture destroyed and created
		// per frame.
		//
		// **Valid until the next `Render` into that slot with a different
		// size**, which is when the target is reallocated. An interface layer hands it straight to
		// whatever draws it — for Dear ImGui's SDL_GPU backend that is an
		// `ImTextureID`, which is an `SDL_GPUTexture *` and therefore this
		// pointer unchanged.
		//
		// @param slot Which offscreen target to ask about. A program with one
		//             viewport uses 0 and never passes this.
		// @return The texture, or `nullptr` when nothing has been drawn into
		//         that slot.
		void *SceneTexture(size_t slot = 0) const;

		// Which corner of that slot's texture the world is in.
		//
		// **It describes the texture `SceneTexture` returns right now**, which
		// is the one an interface recording a bind right now will sample — so
		// the two are read together and stay a matched pair through a resize.
		// On the frame a target is reallocated the interface binds the outgoing
		// texture, and this reports the outgoing texture's rectangle with it.
		//
		// While a panel is merely being dragged the allocation does not change,
		// so this trails the rectangle the frame is about to draw by one frame's
		// worth of drag — under a pixel of scale, against a whole stale frame
		// stretched to the wrong shape before targets were allocated in blocks.
		// See `SceneExtent`.
		//
		// @param slot Which offscreen target to ask about.
		// @return The drawn fraction, or the whole texture when that slot has
		//         never been drawn into.
		SceneExtent SceneTextureExtent(size_t slot = 0) const;

		// Writes the next offscreen frame's world to a file, once.
		//
		// **What makes an editor checkable without a screen.** Driving a window
		// and photographing a display is a test that depends on nobody else
		// using the machine and on the compositor cooperating; this reads the
		// texture the frame was actually drawn into. It costs a fence wait, so
		// it happens on the frames a caller asks for and no others.
		//
		// Only the world, and deliberately: the chrome is drawn onto the window
		// and the window is the swapchain, which SDL does not promise is
		// readable. What this answers is "did the scene render", which is the
		// question a renderer is asked.
		//
		// @param path Where to write a BMP. Empty cancels a pending request.
		void RequestSceneCapture(std::filesystem::path path);

		// The device and swapchain format a `FrameOverlayHook` builds against.
		//
		// @return The handles, both empty before Initialise.
		BackendHandles Backend() const;

	  private:
		// Aborts when the caller is not the owning thread. See `IsOnOwningThread`.
		//
		// @param what The call being refused, for the message.
		void RequireOwningThread(const char *what) const;

		struct Impl;
		std::unique_ptr<Impl> State;

		// The thread that called `Initialise`, and the only one that may record.
		//
		// Not atomic, unlike `ecs::Store::Owner`, and the difference is the
		// point: a store's owner is written every tick by whichever worker
		// picked the world up, so that write races other threads' reads. This is
		// written by the constructor and again by `Initialise`, then only read —
		// so any thread that could observe a torn value is a thread already
		// violating the contract this exists to state.
		std::thread::id Owner;
	};
}
