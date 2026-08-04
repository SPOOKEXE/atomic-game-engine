#pragma once

// Dear ImGui, and the only place in the repository that knows it exists.
//
// **This module is here so that the engine is not.** An editor needs a real
// widget toolkit — a dockable tree, a property grid, a text field that handles
// selection and an IME — and writing one is a year nobody has. Dear ImGui is
// vendored, has an SDL3 platform backend and an SDL_GPU renderer backend, and
// both were built by `mono.build/MonoVendor.cmake` from v0.0 onward with
// nothing linking them. This is the version that links them.
//
// **It is not in `render`, and the reason is the link line.** A shipped game
// draws no panels. Putting imgui into the renderer would put its atlas, its
// pipelines and its shaders into every client binary to be initialised and
// never used, and would make "the renderer owns the device and not the
// decisions" false the moment a widget wanted a font. So `render` declares
// `render::FrameOverlayHook` — two virtual calls and an opaque handle — and
// this module is the only implementation of it.
//
// **The hand-rolled overlay is not replaced and must not be.**
// `render::OverlayImage` draws the F3/F4/F5 panels into a CPU buffer with a
// 3x5 bitmap font, and `Overlay.hpp` says why: they have to work when the
// renderer is the thing being debugged. An imgui panel reporting a frame is
// a panel drawn *by* the pipeline it is reporting on, which is exactly the
// arrangement in which a broken pipeline shows you nothing and you conclude
// the engine hung. Two overlays is the right number here — one for the
// program that is being written, one for the program that is being blamed.
//
// **The interface is not simulation state and never touches a store.** Which
// node is expanded, where a splitter sits, what is selected — none of it is
// world state, none of it is replicated, none of it survives a snapshot. Rule
// 2 says the ECS owns the storage for data another module also reads; nobody
// else reads a scroll position.
//
// @tier L12 · client

#include <engine/render/Renderer.hpp>

#include <memory>

struct SDL_Window;
union SDL_Event;

namespace engine::ui {

	// How an interface is set up.
	//
	// @since v0.7
	struct InterfaceSettings {
		// The scale every font and every padding is multiplied by.
		//
		// One knob rather than a font size and a spacing multiplier, because a
		// UI scaled in one dimension and not the other is worse than one that
		// is small — text that outgrows the row it sits in overlaps the row
		// below and reads as a corrupt font.
		float Scale = 1.0f;

		// Whether panels may be docked against each other and against the
		// window's edges.
		//
		// The editor's whole layout is this, so it defaults on. It is a setting
		// rather than an assumption because a tool that shows one panel does
		// not want the dockspace's invisible full-window host swallowing its
		// mouse clicks.
		bool Docking = true;

		// Where the layout, the window sizes and the open panels are kept
		// between runs, or empty to keep nothing.
		//
		// **Empty by default, and that is deliberate.** imgui writes this file
		// itself, on its own schedule, next to wherever the process happened to
		// start. A program that wants it says where.
		std::string LayoutPath;
	};

	// The imgui context, both backends, and the frame bracket around them.
	//
	// Bracket every frame's widget code with `Begin` and `End`, then hand this
	// to `render::Renderer::Render` as its hook. Outside that bracket no
	// `ImGui::` call is legal, which is imgui's rule rather than this class's.
	//
	// @since v0.7
	// @client
	class Interface final : public render::FrameOverlayHook {
	  public:
		Interface();

		// Shuts both backends down and destroys the context.
		~Interface() override;

		Interface(const Interface &) = delete;
		Interface &operator=(const Interface &) = delete;

		// Creates the context and both backends against a live renderer.
		//
		// The renderer must already be initialised: the SDL_GPU backend builds
		// its pipeline against the device and the swapchain's colour format,
		// and neither exists before `Renderer::Initialise` has run.
		//
		// @param renderer The renderer whose frames this will record into.
		// @param window   The window the renderer claimed.
		// @param settings Scale, docking and where the layout is kept.
		// @return `false` when the renderer has no device or a backend refused.
		bool
		Initialise(render::Renderer &renderer, SDL_Window *window, const InterfaceSettings &settings = {});

		// Tears both backends down and destroys the context. Idempotent.
		void Shutdown();

		// Reports whether `Initialise` succeeded.
		//
		// @return `true` when `Begin` may be called.
		bool IsInitialised() const;

		// Offers one SDL event to the interface.
		//
		// **Call this for every event, and check `WantsMouse`/`WantsKeyboard`
		// before acting on it yourself.** imgui decides whether it wanted an
		// event *after* being told about it, so a program that skipped the ones
		// it thought were its own would have a text field that never received
		// the letter W because the camera was listening for it.
		//
		// @param event The event, as SDL delivered it.
		void ProcessEvent(const SDL_Event &event);

		// Opens a frame. Every `ImGui::` call belongs between this and `End`.
		//
		// @param frameSeconds Wall seconds since the previous frame. Passed in
		//                     rather than read, so a headless or fixed-step
		//                     driver produces the same frames every run.
		void Begin(float frameSeconds);

		// Closes the frame and builds this frame's draw lists.
		//
		// Nothing is submitted here — the renderer does that, from inside its
		// own command buffer, through the two hook calls below.
		void End();

		// Whether the interface consumed the mouse this frame.
		//
		// @return `true` when a click belongs to a panel rather than the world.
		bool WantsMouse() const;

		// Whether the interface consumed the keyboard this frame.
		//
		// @return `true` when a key belongs to a text field rather than a
		//         camera.
		bool WantsKeyboard() const;

		// --- render::FrameOverlayHook ---------------------------------------

		// Uploads this frame's vertices and indices. See `FrameOverlayHook`.
		//
		// @param commandBuffer The frame's `SDL_GPUCommandBuffer *`.
		// @return `false` when there is nothing to draw.
		bool Prepare(void *commandBuffer) override;

		// Records this frame's draw lists. See `FrameOverlayHook`.
		//
		// @param commandBuffer The frame's `SDL_GPUCommandBuffer *`.
		// @param renderPass    The open `SDL_GPURenderPass *`.
		void Record(void *commandBuffer, void *renderPass) override;

	  private:
		struct Impl;
		std::unique_ptr<Impl> State;
	};
}
