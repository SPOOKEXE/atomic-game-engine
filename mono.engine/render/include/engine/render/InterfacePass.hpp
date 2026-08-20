#pragma once

// Drawing a compiled interface, on the device.
//
// **The half a shipped client needs, and the last piece of the seam.**
// `mono.client` does not link `Engine::ui` and must not, so a game draws no
// `ScreenGui` until something other than imgui consumes a `gui::DrawList`. This
// is that something: it implements `FrameOverlayHook`, which is the same slot
// the editor's chrome occupies, so a client hands one of these where the studio
// hands its imgui recorder and the renderer knows the difference not at all.
//
// **The second backend is a new consumer, not a second compile.** Everything
// above this is shared with `ui::PaintGui`: one `gui::Compiled`, one
// `gui::DrawList`, one `render::InterfaceMesh`. What differs is only what the
// triangles are handed to. That is what stops the two halves drifting about
// where an element is - the failure a second layout pass would guarantee.
//
// **A class rather than a branch inside `Renderer`.** The renderer already
// takes a hook for exactly this, and adding an interface path to its own
// recording would put a `gui` concern inside three thousand lines of pipeline
// state - where the tier check would not have caught the edge and a reviewer
// would have had to.
//
// @tier L12 · client

#include <engine/core/Name.hpp>
#include <engine/gui/Components.hpp>
#include <engine/gui/DrawList.hpp>
#include <engine/render/Flipbook.hpp>
#include <engine/render/GlyphAtlas.hpp>
#include <engine/render/InterfaceMesh.hpp>
#include <engine/render/Renderer.hpp>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <unordered_map>
#include <vector>

namespace engine::ecs {
	class Store;
}

namespace engine::render {

	// One interface image resolved for the current frame.
	//
	// The dimensions travel with the handle because every scale mode except
	// stretch needs the source aspect or source-pixel insets. `Cell` identifies
	// the current frame of an animated sheet.
	struct InterfaceImage {
		void *Texture = nullptr;
		FlipbookCell Cell;
		core::Vector2 UVMax{1.0f, 1.0f};
		uint32_t Width = 0;
		uint32_t Height = 0;
	};

	// Draws a compiled `gui::DrawList` into the render target selected by the
	// interface node.
	//
	// @since v0.8
	class InterfacePass : public FrameOverlayHook {
	  public:
		InterfacePass() = default;
		~InterfacePass() override;

		InterfacePass(const InterfacePass &) = delete;
		InterfacePass &operator=(const InterfacePass &) = delete;
		InterfacePass(InterfacePass &&) = delete;
		InterfacePass &operator=(InterfacePass &&) = delete;

		// Creates the pipeline, the sampler and the atlas texture.
		//
		// **The atlas is uploaded once here rather than per frame.** It changes
		// only when the size it was baked at does, which is a settings change
		// and not a frame event - re-uploading a megabyte every frame to save a
		// dirty flag would be the whole cost of the design.
		//
		// @param device         The `SDL_GPUDevice *` the renderer opened.
		// @param swapchainFormat The `SDL_GPUTextureFormat` of the window.
		// @param pixelSize      The em size to bake glyphs at.
		// @return `false` when a shader, the pipeline or the atlas failed. The
		//         caller draws no interface rather than failing the frame.
		bool Initialise(void *device, uint32_t swapchainFormat, float pixelSize = 16.0f);

		// Releases everything, in the order the device wants it.
		void Shutdown();

		// Compiles a fragment shader an `ImageLabel` or `ImageButton` may
		// select, and builds the pipeline that draws with it.
		//
		// **Written against `interface.frag`'s own slots and not
		// `opaque.frag`'s** - one sampler and no bound uniform buffer -
		// because the two passes are different pipelines with different
		// bindings. A `ShaderScript` meant for a `Material` is not
		// interchangeable with one meant for an `ImageLabel`; each is
		// authored against the contract the pass that compiles it declares.
		//
		// **Replacing rather than refusing a name already held**, so an
		// author editing a `ShaderScript` sees the change - `Renderer::
		// AddShader` makes the same call for the identical reason.
		//
		// @param name  The shader's name - `gui::Picture::Shader`,
		//        `gui::DrawCommand::Shader` and `render::ShaderModule`'s own
		//        all name the same thing.
		// @param spirv The compiled words, from `render::ShaderLibrary::
		//        Find(name)->SpirV`.
		// @return `false` on a device, translation or pipeline failure. The
		//         caller keeps drawing with the pass's own shader either way.
		// @since v0.18
		bool AddShaderVariant(const core::Name &name, std::span<const uint32_t> spirv);

		// Releases a variant this pass no longer needs.
		//
		// @param name The shader's name.
		// @return `false` when nothing was held under it.
		// @since v0.18
		bool DropShaderVariant(const core::Name &name);

		// The list to draw next frame, and the canvas it was compiled against.
		//
		// **Copied rather than held by reference.** The list belongs to a
		// `gui::Compiled` the caller owns and rebuilds; a pointer kept across
		// the frame boundary would dangle the first time an element was added.
		//
		// @param list   This frame's compiled list.
		// @param canvas The target size in pixels, which the vertex shader
		//        divides by.
		void Submit(const gui::DrawList &list, const core::Vector2 &canvas, ecs::Store &store);

		// Uploads this frame's vertices and indices.
		//
		// @param commandBuffer The frame's `SDL_GPUCommandBuffer *`.
		// @return `false` when there is nothing to draw.
		bool Prepare(void *commandBuffer) override;

		// Records the batches.
		//
		// @param commandBuffer The frame's `SDL_GPUCommandBuffer *`.
		// @param renderPass    An open pass bound to the interface target.
		void Record(void *commandBuffer, void *renderPass) override;

		uint32_t RecordWorld(
			void *commandBuffer,
			void *renderPass,
			const glm::mat4 &viewProjection,
			const core::CFrame &camera,
			const core::Color3 &ambient,
			const core::Vector3 &sun,
			uint32_t width,
			uint32_t height,
			bool alwaysOnTop
		) override;

		// How to resolve a content name to a texture.
		//
		// **The hook `ui::ImageSource` is on this side of the seam**, and it is
		// the caller's for the same reason: this module has no business
		// resolving a game's content names, and `gui::DrawCommand::Image` is a
		// name rather than a handle precisely so that whoever owns the textures
		// decides. Unset means every image draws the atlas's white texel - a
		// visible flat rectangle rather than nothing, so a missing image reads
		// as missing.
		//
		// **The cell comes back with the handle**, because a `.gif` bakes to an
		// ordinary texture and only the thing that uploaded it knows the sheet
		// is a sheet. Returning the handle alone is what made an animated
		// `ImageLabel` a still of its first frame.
		//
		// @param resolve Called with a content name and a cell to fill; returns
		//        an `SDL_GPUTexture *` or null. The cell is left at the identity
		//        for anything that is not a sheet.
		void SetImageSource(std::function<InterfaceImage(const core::Name &)> resolve) {
			Images = std::move(resolve);
		}

		// How to resolve the live scene image behind a `ViewportFrame`.
		void SetViewportSource(std::function<InterfaceImage(ecs::Entity)> resolve) {
			Viewports = std::move(resolve);
		}

		// The atlas, for a caller that wants to measure text with it.
		const GlyphAtlas &Atlas() const {
			return Glyphs;
		}

		// How many batches the last recorded frame submitted.
		size_t LastBatchCount() const {
			return Recorded;
		}

	  private:
		bool UploadAtlas(void *commandBuffer);

		void *Device = nullptr;
		void *Pipeline = nullptr;
		void *SpatialPipeline = nullptr;
		void *SpatialTopPipeline = nullptr;
		void *Sampler = nullptr;

		// The nearest-filter twin, for `gui::ResampleMode::Pixelated`. See
		// `Initialise` for why it is a second sampler and not a second pipeline.
		//
		// @since v0.18
		void *PixelSampler = nullptr;
		void *AtlasTexture = nullptr;
		void *AtlasTransferBuffer = nullptr;
		uint32_t SwapchainFormat = 0;

		// A shader-named pipeline, built by `AddShaderVariant`. Keyed by
		// `core::Name::Id`, matching every other name-keyed cache in this
		// module.
		//
		// @since v0.18
		std::unordered_map<uint32_t, void *> ShaderVariants;

		void *VertexBuffer = nullptr;
		void *IndexBuffer = nullptr;
		void *TransferBuffer = nullptr;
		uint32_t VertexCapacity = 0;
		uint32_t IndexCapacity = 0;
		uint32_t TransferCapacity = 0;

		GlyphAtlas Glyphs;
		InterfaceMesh Mesh;
		gui::DrawList Pending;
		core::Vector2 Canvas;
		struct SpatialCollector {
			ecs::Entity Collector;
			gui::SpatialCanvas Canvas;
		};
		std::vector<SpatialCollector> SpatialCollectors;

		struct ResolvedImage {
			core::Name Name;
			ecs::Entity Viewport;
			InterfaceImage Value;
		};

		std::function<InterfaceImage(const core::Name &)> Images;
		std::function<InterfaceImage(ecs::Entity)> Viewports;
		std::vector<ResolvedImage> ResolvedImages;

		bool AtlasUploaded = false;
		size_t Recorded = 0;
	};
}
