#pragma once

// Backend handles and the frame overlay hook a host uses to record its own passes.

#include <engine/core/types/CFrame.hpp>
#include <engine/core/types/Color3.hpp>

#include <glm/mat4x4.hpp>

#include <cstdint>
#include <functional>

namespace engine::render {
	// The backend handles a hook needs to build its own pipelines.
	//
	// **Opaque on purpose.** `Device` is an `SDL_GPUDevice *` and `ColourFormat`
	// is an `SDL_GPUTextureFormat`, and neither name appears here - this header
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

	// Device-resource callbacks attached to a custom native node handler.
	//
	// Reinstall runs after each device creation, including a renderer that was
	// shut down and initialised again. Release runs before that device is
	// destroyed and before a replacement handler takes ownership of the kind.
	// Both may be empty for a handler that owns no device resources.
	//
	// @since v0.20
	struct NodeHandlerLifecycle {
		// Recreates resources against the new device. False refuses Initialise or
		// leaves an existing handler installed during replacement.
		std::function<bool(BackendHandles)> Reinstall;

		// Releases resources while the device is still valid.
		std::function<void(BackendHandles)> Release;
	};

	// The attachment contract for world-space UI. Display uses Backend().ColourFormat;
	// Hdr uses RGBA16F and preserves radiance until the portal's display conversion.
	enum class WorldColourTarget : uint8_t { Display, Hdr };

	// One prepared spatial batch rendered into caller-owned RGBA16F/D32F scratch
	// attachments. Device handles stay local to the frame-owning renderer.
	struct WorldInterfaceCapture {
		// Opaque handle to the active GPU command buffer.
		void *Command = nullptr;
		// Opaque handle to the open render pass.
		void *Pass = nullptr;
		// Transform from world space into this attachment's clip space.
		glm::mat4 ViewProjection{1};
		// Camera pose used for world-space interface placement.
		core::CFrame Camera;
		// Ambient light colour applied to interface geometry.
		core::Color3 Ambient;
		// Direction of sunlight used by interface shading.
		core::Vector3 Sun;
		// Scratch attachment width in pixels.
		uint32_t Width = 0;
		// Scratch attachment height in pixels.
		uint32_t Height = 0;
	};

	// Whether an overlay can attest to a complete recorded layer.
	enum class OverlayRecordStatus : uint8_t {
		// The hook cannot attest to its output.
		Unknown,
		// A required recording resource or operation failed.
		Failed,
		// All eligible content was recorded, including an empty layer.
		Complete,
	};

	// Which part of the staged GUI an overlay hook can record.
	enum class OverlayLayer : uint8_t {
		// GUI in screen coordinates.
		Screen,
		// GUI on world-space collectors.
		Spatial,
	};

	// What a prepared hook can attest about a layer's submitted commands.
	enum class OverlayLayerContent : uint8_t {
		// The hook cannot attest to its commands.
		Unknown,
		// No commands belong to the layer.
		Empty,
		// At least one command belongs to the layer.
		Present,
		// Command and collector classifications disagree.
		Invalid,
	};

	// A layer that records into this renderer's frame.
	//
	// **This exists so that Dear ImGui is not in the engine.** An editor needs
	// its chrome inside the same command buffer as the world - SDL's GPU API
	// acquires a swapchain texture once per command buffer, so a second pass in
	// a second buffer is not an option - and the two ways to arrange that are
	// both worse than this one. Putting imgui in `render` puts it on every
	// shipping client's link line to draw nothing. Exposing `SDL_GPUCommandBuffer`
	// on `Render` breaks the rule at the top of this file.
	//
	// So the renderer hands over its command buffer and render pass as `void *`,
	// and `mono.engine/ui` is the only module in the repository that knows what
	// Dear ImGui is. The lost type safety is real and is the price; it is paid
	// at exactly two call sites, both inside one file.
	//
	// **`Prepare` is outside every render pass and `Record` is inside a render
	// pass.** That split is not stylistic: uploading vertices is a copy pass, and
	// a copy pass cannot be started while a render pass is open. A hook that
	// uploaded from `Record` would work until the first frame with enough
	// widgets to grow its buffer.
	//
	// A game interface hook records into the graph's `interface` image. A host
	// overlay hook records after `output-image`, directly onto the swapchain.
	// Keeping those roles separate prevents Studio chrome from becoming part of
	// a universe's rendering profile or its node previews.
	//
	// @since v0.7
	// @client
	class FrameOverlayHook {
	  public:
		virtual ~FrameOverlayHook() = default;

		// Complete means this hook can record the requested layer, Failed means it
		// is absent, and Unknown leaves legacy hooks on the fallback input path.
		virtual OverlayRecordStatus LayerAvailability(OverlayLayer) const {
			return OverlayRecordStatus::Unknown;
		}
		// Reports the submitted command content, or Unknown when a hook cannot attest it.
		virtual OverlayLayerContent LayerContent(OverlayLayer) const {
			return OverlayLayerContent::Unknown;
		}

		// The result of the most recent Prepare call, when the hook can verify it.
		virtual OverlayRecordStatus PreparationStatus() const {
			return OverlayRecordStatus::Unknown;
		}
		// True when Prepare succeeded but produced no drawable geometry.
		virtual bool PreparedEmpty() const {
			return false;
		}

		// Uploads whatever this frame needs, before any render pass is open.
		//
		// @param commandBuffer The frame's `SDL_GPUCommandBuffer *`.
		// @return `false` to skip `Record` - nothing to draw, which is not an
		//         error and not a reason to fail the frame.
		virtual bool Prepare(void *commandBuffer) = 0;

		// Called after the frame command buffer has been accepted or rejected.
		// Hooks that retain device output use this to advance a pixel baseline only
		// after the bytes that produced it are owned by SDL.
		virtual void CompleteFrame(bool) {}

		// Batch indices remain stable between Prepare calls. Scratch capture writes
		// fragment depth as well as colour, for composition with other world layers.
		virtual bool SupportsWorldLayers() const {
			return !AffectsScene();
		}
		// Reports whether a prepared world-space overlay must be recorded.
		virtual bool HasWorldOverlay() const {
			return false;
		}
		// Returns the number of prepared world-space batches.
		virtual size_t WorldBatchCount() const {
			return 0;
		}
		// Records one world-space batch and returns its submitted draw count.
		virtual uint32_t RecordWorldBatch(const WorldInterfaceCapture &, size_t) {
			return 0;
		}

		// Records world-space interface collectors into an open scene pass.
		//
		// The default keeps editor hooks screen-only. A game interface backend
		// overrides it for `SurfaceGui` and `BillboardGui`, using the same upload
		// `Prepare` made for the final screen pass.
		virtual uint32_t RecordWorld(
			void *,
			void *,
			const glm::mat4 &,
			const core::CFrame &,
			const core::Color3 &,
			const core::Vector3 &,
			uint32_t,
			uint32_t,
			bool,
			WorldColourTarget = WorldColourTarget::Display
		) {
			return 0;
		}

		// Records a world pass and reports whether every eligible batch was recorded.
		// The default preserves existing hooks without attesting to their output.
		virtual OverlayRecordStatus RecordWorldChecked(
			const WorldInterfaceCapture &capture, bool alwaysOnTop, WorldColourTarget target, uint32_t &draws
		) {
			draws = RecordWorld(
				capture.Command,
				capture.Pass,
				capture.ViewProjection,
				capture.Camera,
				capture.Ambient,
				capture.Sun,
				capture.Width,
				capture.Height,
				alwaysOnTop,
				target
			);
			return OverlayRecordStatus::Unknown;
		}

		// Records the screen layer and reports whether its output was complete.
		// Legacy hooks draw normally but cannot attest to a complete result.
		virtual OverlayRecordStatus RecordChecked(void *commandBuffer, void *renderPass) {
			Record(commandBuffer, renderPass);
			return OverlayRecordStatus::Unknown;
		}

		// Whether a game-interface change also changes world-space pixels. Screen
		// UI returns false; SurfaceGui and BillboardGui implementations return true.
		virtual bool AffectsScene() const {
			return false;
		}

		// Records draw commands into the supplied image target.
		//
		// @param commandBuffer The frame's `SDL_GPUCommandBuffer *`.
		// @param renderPass    An open `SDL_GPURenderPass *` with no depth
		//                      attachment.
		virtual void Record(void *commandBuffer, void *renderPass) = 0;
	};
}
