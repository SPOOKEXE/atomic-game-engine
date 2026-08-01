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
// camera, and a span of instances.
//
// @tier L12 · client

#include <engine/core/types/CFrame.hpp>
#include <engine/render/Overlay.hpp>

#include <glm/mat4x4.hpp>
#include <glm/vec4.hpp>

#include <cstdint>
#include <memory>
#include <span>
#include <string_view>

struct SDL_Window;

namespace engine::render {

	// One drawable thing. Deliberately flat and copyable: this is what crosses
	// from simulation to presentation, and the day a world is a process it has
	// to survive being memcpy'd.
	//
	// @client
	struct Instance {
		// Column-major transform from object space to world space.
		glm::mat4 Model { 1.0f };

		// Straight RGBA colour multiplier for the opaque mesh.
		glm::vec4 Colour { 1.0f, 1.0f, 1.0f, 1.0f };
	};

	// A right-handed, Y-up perspective camera that looks along local negative Z.
	//
	// SDL presents Y-up clip space on every backend. Depth is mapped to 0..1.
	//
	// @client
	struct Camera {
		// Camera transform in world space; the renderer uses its inverse as the view transform.
		core::CFrame Frame;

		// Vertical field of view, in radians.
		float FieldOfViewRadians = 1.22f;  // 70 degrees

		// Near clipping distance in world units.
		float NearPlane = 0.1f;

		// Far clipping distance in world units.
		float FarPlane = 500.0f;
	};

	// Work encoded by one Render call.
	//
	// A default result means no frame was presented, including while the window
	// is minimised or resizing and when the renderer is unavailable.
	//
	// @client
	struct FrameResult {
		// Whether a swapchain texture was acquired and the command buffer was
		// handed to SDL for submission.
		bool Presented = false;

		// Number of opaque and overlay draw calls submitted for this frame.
		uint32_t DrawCalls = 0;

		// Number of opaque mesh triangles submitted for this frame.
		uint64_t Triangles = 0;
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
		// error and not a reason to stop ticking.
		//
		// `overlay` is uploaded only when it has something in it.
		// The inputs are copied during the call and are not retained by the renderer.
		//
		// @param camera    World-space camera and perspective clipping settings.
		// @param instances Object-to-world transforms and colours to draw as cubes.
		// @param overlay   CPU premultiplied RGBA8 overlay; uploaded only when dirty
		//                   and non-empty.
		// @return Submitted draw counts and whether the frame was presented.
		FrameResult Render(
			const Camera &camera,
			std::span<const Instance> instances,
			const OverlayImage &overlay
		);

	  private:
		struct Impl;
		std::unique_ptr<Impl> State;
	};
}
