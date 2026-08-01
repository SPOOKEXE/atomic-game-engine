#include <engine/core/Log.hpp>
#include <engine/core/Paths.hpp>
#include <engine/core/Profiling.hpp>
#include <engine/render/Primitives.hpp>
#include <engine/render/Renderer.hpp>

#include <SDL3/SDL_gpu.h>
#include <SDL3/SDL_iostream.h>
#include <SDL3/SDL_video.h>
#include <glm/gtc/matrix_transform.hpp>

#include <array>
#include <cstring>
#include <vector>

namespace engine::render {

	namespace {

		using Vertex = MeshVertex;

		struct FrameUniforms {
			glm::mat4 ViewProjection;
		};

		struct LightingUniforms {
			glm::vec4 Direction;
			glm::vec4 Ambient;
		};

		std::vector<uint8_t> ReadFile(const std::filesystem::path &path) {
			size_t size = 0;
			void *data = SDL_LoadFile(path.string().c_str(), &size);
			if (!data) {
				return {};
			}

			std::vector<uint8_t> bytes(
				static_cast<const uint8_t *>(data), static_cast<const uint8_t *>(data) + size
			);
			SDL_free(data);
			return bytes;
		}
	}

	// -----------------------------------------------------------------------

	struct Renderer::Impl {
		SDL_Window *Window = nullptr;
		SDL_GPUDevice *Device = nullptr;

		SDL_GPUGraphicsPipeline *OpaquePipeline = nullptr;
		SDL_GPUGraphicsPipeline *OverlayPipeline = nullptr;

		SDL_GPUBuffer *VertexBuffer = nullptr;
		SDL_GPUBuffer *IndexBuffer = nullptr;

		SDL_GPUBuffer *InstanceBuffer = nullptr;
		SDL_GPUTransferBuffer *InstanceTransfer = nullptr;
		uint32_t InstanceCapacity = 0;

		SDL_GPUTexture *DepthTexture = nullptr;
		uint32_t DepthWidth = 0;
		uint32_t DepthHeight = 0;

		SDL_GPUTexture *OverlayTexture = nullptr;
		SDL_GPUTransferBuffer *OverlayTransfer = nullptr;
		SDL_GPUSampler *OverlaySampler = nullptr;
		int OverlayWidth = 0;
		int OverlayHeight = 0;

		// Set when the overlay texture is created and cleared by the first
		// upload after it, which is made to cover the whole image rather than
		// only the region that changed.
		bool OverlayUninitialised = false;

		std::string Backend;

		SDL_GPUShader *LoadShader(
			std::string_view name, SDL_GPUShaderStage stage, uint32_t samplers, uint32_t uniformBuffers
		) const;

		bool CreatePipelines();
		bool CreateGeometry();
		bool EnsureInstanceCapacity(uint32_t count);
		bool EnsureDepth(uint32_t width, uint32_t height);
		bool EnsureOverlay(int width, int height);
	};

	SDL_GPUShader *Renderer::Impl::LoadShader(
		std::string_view name, SDL_GPUShaderStage stage, uint32_t samplers, uint32_t uniformBuffers
	) const {
		// Staged under the owning module's name, so that two modules cannot
		// collide on a common file name like fullscreen.vert.
		const auto path = core::Paths::Shaders("render") / (std::string(name) + ".spv");

		const auto code = ReadFile(path);
		if (code.empty()) {
			ENGINE_ERROR("shader not found or empty: {}", path.string());
			return nullptr;
		}

		SDL_GPUShaderCreateInfo info{};
		info.code = code.data();
		info.code_size = code.size();
		info.entrypoint = "main";
		info.format = SDL_GPU_SHADERFORMAT_SPIRV;
		info.stage = stage;
		info.num_samplers = samplers;
		info.num_uniform_buffers = uniformBuffers;

		SDL_GPUShader *shader = SDL_CreateGPUShader(Device, &info);
		if (!shader) {
			ENGINE_ERROR("SDL_CreateGPUShader failed for {}: {}", name, SDL_GetError());
		}
		return shader;
	}

	bool Renderer::Impl::CreatePipelines() {
		SDL_GPUShader *opaqueVertex = LoadShader("opaque.vert", SDL_GPU_SHADERSTAGE_VERTEX, 0, 1);
		SDL_GPUShader *opaqueFragment = LoadShader("opaque.frag", SDL_GPU_SHADERSTAGE_FRAGMENT, 0, 1);
		SDL_GPUShader *overlayVertex = LoadShader("overlay.vert", SDL_GPU_SHADERSTAGE_VERTEX, 0, 0);
		SDL_GPUShader *overlayFragment = LoadShader("overlay.frag", SDL_GPU_SHADERSTAGE_FRAGMENT, 1, 0);

		if (!opaqueVertex || !opaqueFragment || !overlayVertex || !overlayFragment) {
			return false;
		}

		const SDL_GPUTextureFormat swapchainFormat = SDL_GetGPUSwapchainTextureFormat(Device, Window);

		// --- opaque ---------------------------------------------------------

		const SDL_GPUVertexBufferDescription vertexBuffers[] = {
			{0, sizeof(Vertex), SDL_GPU_VERTEXINPUTRATE_VERTEX, 0},
			// One step per instance: the same 36 indices are replayed for every
			// entity, and only the matrix and colour change.
			{1, sizeof(Instance), SDL_GPU_VERTEXINPUTRATE_INSTANCE, 0},
		};

		const SDL_GPUVertexAttribute attributes[] = {
			{0, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3, offsetof(Vertex, Position)},
			{1, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3, offsetof(Vertex, Normal)},
			// A mat4 attribute is four float4 locations; there is no matrix
			// vertex format.
			{2, 1, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4, 0},
			{3, 1, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4, sizeof(float) * 4},
			{4, 1, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4, sizeof(float) * 8},
			{5, 1, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4, sizeof(float) * 12},
			{6, 1, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4, offsetof(Instance, Colour)},
		};

		SDL_GPUColorTargetDescription opaqueTarget{};
		opaqueTarget.format = swapchainFormat;

		SDL_GPUGraphicsPipelineCreateInfo opaque{};
		opaque.vertex_shader = opaqueVertex;
		opaque.fragment_shader = opaqueFragment;
		opaque.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
		opaque.vertex_input_state.vertex_buffer_descriptions = vertexBuffers;
		opaque.vertex_input_state.num_vertex_buffers = 2;
		opaque.vertex_input_state.vertex_attributes = attributes;
		opaque.vertex_input_state.num_vertex_attributes = 7;
		opaque.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
		opaque.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_BACK;
		// The cube winds counter-clockwise when seen from outside.
		opaque.rasterizer_state.front_face = SDL_GPU_FRONTFACE_COUNTER_CLOCKWISE;
		opaque.depth_stencil_state.enable_depth_test = true;
		opaque.depth_stencil_state.enable_depth_write = true;
		opaque.depth_stencil_state.compare_op = SDL_GPU_COMPAREOP_LESS;
		opaque.target_info.color_target_descriptions = &opaqueTarget;
		opaque.target_info.num_color_targets = 1;
		opaque.target_info.depth_stencil_format = SDL_GPU_TEXTUREFORMAT_D32_FLOAT;
		opaque.target_info.has_depth_stencil_target = true;

		OpaquePipeline = SDL_CreateGPUGraphicsPipeline(Device, &opaque);
		if (!OpaquePipeline) {
			ENGINE_ERROR("opaque pipeline: {}", SDL_GetError());
		}

		// --- overlay --------------------------------------------------------

		SDL_GPUColorTargetDescription overlayTarget{};
		overlayTarget.format = swapchainFormat;
		overlayTarget.blend_state.enable_blend = true;
		overlayTarget.blend_state.color_blend_op = SDL_GPU_BLENDOP_ADD;
		overlayTarget.blend_state.alpha_blend_op = SDL_GPU_BLENDOP_ADD;
		// OverlayImage stores premultiplied RGB, so multiplying by source alpha
		// here would apply alpha twice and darken every translucent panel pixel.
		overlayTarget.blend_state.src_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
		overlayTarget.blend_state.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
		overlayTarget.blend_state.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
		overlayTarget.blend_state.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;

		SDL_GPUGraphicsPipelineCreateInfo overlay{};
		overlay.vertex_shader = overlayVertex;
		overlay.fragment_shader = overlayFragment;
		overlay.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
		overlay.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
		overlay.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
		// No depth: the overlay is on top of everything by definition.
		overlay.target_info.color_target_descriptions = &overlayTarget;
		overlay.target_info.num_color_targets = 1;

		OverlayPipeline = SDL_CreateGPUGraphicsPipeline(Device, &overlay);
		if (!OverlayPipeline) {
			ENGINE_ERROR("overlay pipeline: {}", SDL_GetError());
		}

		// The pipelines hold what they need; the shader objects do not have to
		// outlive their creation.
		SDL_ReleaseGPUShader(Device, opaqueVertex);
		SDL_ReleaseGPUShader(Device, opaqueFragment);
		SDL_ReleaseGPUShader(Device, overlayVertex);
		SDL_ReleaseGPUShader(Device, overlayFragment);

		return OpaquePipeline != nullptr && OverlayPipeline != nullptr;
	}

	bool Renderer::Impl::CreateGeometry() {
		constexpr uint32_t VERTEX_BYTES = sizeof(CUBE_VERTICES);
		constexpr uint32_t INDEX_BYTES = sizeof(CUBE_INDICES);

		SDL_GPUBufferCreateInfo vertexInfo{};
		vertexInfo.usage = SDL_GPU_BUFFERUSAGE_VERTEX;
		vertexInfo.size = VERTEX_BYTES;
		VertexBuffer = SDL_CreateGPUBuffer(Device, &vertexInfo);

		SDL_GPUBufferCreateInfo indexInfo{};
		indexInfo.usage = SDL_GPU_BUFFERUSAGE_INDEX;
		indexInfo.size = INDEX_BYTES;
		IndexBuffer = SDL_CreateGPUBuffer(Device, &indexInfo);

		if (!VertexBuffer || !IndexBuffer) {
			ENGINE_ERROR("cube buffers: {}", SDL_GetError());
			return false;
		}

		// The mesh never changes, so the transfer buffer is temporary — unlike
		// the instance one, which is kept for the life of the renderer.
		SDL_GPUTransferBufferCreateInfo transferInfo{};
		transferInfo.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
		transferInfo.size = VERTEX_BYTES + INDEX_BYTES;

		SDL_GPUTransferBuffer *transfer = SDL_CreateGPUTransferBuffer(Device, &transferInfo);
		if (!transfer) {
			ENGINE_ERROR("cube transfer buffer: {}", SDL_GetError());
			return false;
		}

		auto *mapped = static_cast<uint8_t *>(SDL_MapGPUTransferBuffer(Device, transfer, false));
		std::memcpy(mapped, CUBE_VERTICES.data(), VERTEX_BYTES);
		std::memcpy(mapped + VERTEX_BYTES, CUBE_INDICES.data(), INDEX_BYTES);
		SDL_UnmapGPUTransferBuffer(Device, transfer);

		SDL_GPUCommandBuffer *command = SDL_AcquireGPUCommandBuffer(Device);
		SDL_GPUCopyPass *copy = SDL_BeginGPUCopyPass(command);

		SDL_GPUTransferBufferLocation source{transfer, 0};
		SDL_GPUBufferRegion destination{VertexBuffer, 0, VERTEX_BYTES};
		SDL_UploadToGPUBuffer(copy, &source, &destination, false);

		source.offset = VERTEX_BYTES;
		destination = SDL_GPUBufferRegion{IndexBuffer, 0, INDEX_BYTES};
		SDL_UploadToGPUBuffer(copy, &source, &destination, false);

		SDL_EndGPUCopyPass(copy);
		SDL_SubmitGPUCommandBuffer(command);
		SDL_ReleaseGPUTransferBuffer(Device, transfer);

		return true;
	}

	bool Renderer::Impl::EnsureInstanceCapacity(uint32_t count) {
		if (count <= InstanceCapacity) {
			return true;
		}

		// Grow in powers of two. A scene that gains one entity per frame would
		// otherwise reallocate every frame.
		uint32_t capacity = InstanceCapacity == 0 ? 256 : InstanceCapacity;
		while (capacity < count) {
			capacity *= 2;
		}

		if (InstanceBuffer) {
			SDL_ReleaseGPUBuffer(Device, InstanceBuffer);
		}
		if (InstanceTransfer) {
			SDL_ReleaseGPUTransferBuffer(Device, InstanceTransfer);
		}

		const uint32_t bytes = capacity * static_cast<uint32_t>(sizeof(Instance));

		SDL_GPUBufferCreateInfo bufferInfo{};
		bufferInfo.usage = SDL_GPU_BUFFERUSAGE_VERTEX;
		bufferInfo.size = bytes;
		InstanceBuffer = SDL_CreateGPUBuffer(Device, &bufferInfo);

		SDL_GPUTransferBufferCreateInfo transferInfo{};
		transferInfo.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
		transferInfo.size = bytes;
		InstanceTransfer = SDL_CreateGPUTransferBuffer(Device, &transferInfo);

		if (!InstanceBuffer || !InstanceTransfer) {
			ENGINE_ERROR("instance buffer of {} entries: {}", capacity, SDL_GetError());
			InstanceCapacity = 0;
			return false;
		}

		InstanceCapacity = capacity;
		return true;
	}

	bool Renderer::Impl::EnsureDepth(uint32_t width, uint32_t height) {
		if (DepthTexture && width == DepthWidth && height == DepthHeight) {
			return true;
		}

		if (DepthTexture) {
			SDL_ReleaseGPUTexture(Device, DepthTexture);
			DepthTexture = nullptr;
		}

		SDL_GPUTextureCreateInfo info{};
		info.type = SDL_GPU_TEXTURETYPE_2D;
		info.format = SDL_GPU_TEXTUREFORMAT_D32_FLOAT;
		info.usage = SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET;
		info.width = width;
		info.height = height;
		info.layer_count_or_depth = 1;
		info.num_levels = 1;
		info.sample_count = SDL_GPU_SAMPLECOUNT_1;

		DepthTexture = SDL_CreateGPUTexture(Device, &info);
		if (!DepthTexture) {
			ENGINE_ERROR("depth texture {}x{}: {}", width, height, SDL_GetError());
			return false;
		}

		DepthWidth = width;
		DepthHeight = height;
		return true;
	}

	bool Renderer::Impl::EnsureOverlay(int width, int height) {
		if (OverlayTexture && width == OverlayWidth && height == OverlayHeight) {
			return true;
		}

		if (OverlayTexture) {
			SDL_ReleaseGPUTexture(Device, OverlayTexture);
			OverlayTexture = nullptr;
		}
		if (OverlayTransfer) {
			SDL_ReleaseGPUTransferBuffer(Device, OverlayTransfer);
			OverlayTransfer = nullptr;
		}

		SDL_GPUTextureCreateInfo info{};
		info.type = SDL_GPU_TEXTURETYPE_2D;
		info.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
		info.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
		info.width = static_cast<uint32_t>(width);
		info.height = static_cast<uint32_t>(height);
		info.layer_count_or_depth = 1;
		info.num_levels = 1;
		info.sample_count = SDL_GPU_SAMPLECOUNT_1;

		OverlayTexture = SDL_CreateGPUTexture(Device, &info);

		SDL_GPUTransferBufferCreateInfo transferInfo{};
		transferInfo.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
		transferInfo.size =
			static_cast<uint32_t>(width) * static_cast<uint32_t>(height) * OverlayImage::BYTES_PER_PIXEL;
		OverlayTransfer = SDL_CreateGPUTransferBuffer(Device, &transferInfo);

		if (!OverlayTexture || !OverlayTransfer) {
			ENGINE_ERROR("overlay texture {}x{}: {}", width, height, SDL_GetError());
			return false;
		}

		OverlayWidth = width;
		OverlayHeight = height;

		// A new texture holds whatever the driver had lying around. That did not
		// matter while every frame uploaded the whole image; now that a frame
		// uploads only the panels, everything outside them would be garbage
		// until something happened to draw over it. The next upload covers the
		// whole texture once to settle it.
		OverlayUninitialised = true;
		return true;
	}

	// -----------------------------------------------------------------------

	Renderer::Renderer() : State(std::make_unique<Impl>()) {}

	Renderer::~Renderer() {
		Shutdown();
	}

	bool Renderer::IsInitialised() const {
		return State->Device != nullptr;
	}

	std::string_view Renderer::BackendName() const {
		return State->Backend;
	}

	bool Renderer::SetVerticalSync(bool enabled) {
		if (!State->Device) {
			return false;
		}

		const SDL_GPUPresentMode mode = enabled ? SDL_GPU_PRESENTMODE_VSYNC : SDL_GPU_PRESENTMODE_IMMEDIATE;

		// VSYNC is the only mode required to exist. Asking for IMMEDIATE on a
		// backend without it fails rather than silently staying synchronised,
		// so check before setting — an unsupported mode would otherwise leave
		// the swapchain in whatever state the failed call left it.
		if (!SDL_WindowSupportsGPUPresentMode(State->Device, State->Window, mode)) {
			ENGINE_WARN("present mode unsupported on {}", State->Backend);
			return false;
		}

		if (!SDL_SetGPUSwapchainParameters(
				State->Device, State->Window, SDL_GPU_SWAPCHAINCOMPOSITION_SDR, mode
			)) {
			ENGINE_WARN("SDL_SetGPUSwapchainParameters: {}", SDL_GetError());
			return false;
		}

		return true;
	}

	bool Renderer::Initialise(SDL_Window *window) {
		if (!window) {
			ENGINE_ERROR("Renderer::Initialise called with no window");
			return false;
		}

		State->Window = window;

		// SPIR-V only. Metal needs the cross-compile step the module's
		// CMakeLists does on Apple targets; D3D12 needs DXIL, which is not
		// built yet. Asking for formats we cannot supply would find a device
		// and then fail at pipeline creation, which is a worse error.
		State->Device = SDL_CreateGPUDevice(SDL_GPU_SHADERFORMAT_SPIRV, false, nullptr);
		if (!State->Device) {
			ENGINE_ERROR("SDL_CreateGPUDevice: {}", SDL_GetError());
			return false;
		}

		if (!SDL_ClaimWindowForGPUDevice(State->Device, window)) {
			ENGINE_ERROR("SDL_ClaimWindowForGPUDevice: {}", SDL_GetError());
			Shutdown();
			return false;
		}

		const char *driver = SDL_GetGPUDeviceDriver(State->Device);
		State->Backend = driver ? driver : "unknown";

		SDL_GPUSamplerCreateInfo sampler{};
		// Nearest, because the overlay is pixel art at exactly one texel per
		// pixel. Linear would blur the 3x5 font into illegibility.
		sampler.min_filter = SDL_GPU_FILTER_NEAREST;
		sampler.mag_filter = SDL_GPU_FILTER_NEAREST;
		sampler.address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
		sampler.address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
		sampler.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
		State->OverlaySampler = SDL_CreateGPUSampler(State->Device, &sampler);

		if (!State->CreatePipelines() || !State->CreateGeometry()) {
			Shutdown();
			return false;
		}

		ENGINE_INFO("renderer ready on {}", State->Backend);
		return true;
	}

	void Renderer::Shutdown() {
		auto *device = State->Device;
		if (!device) {
			return;
		}

		// Everything below is still referenced by frames that may not have
		// finished. Waiting once here is simpler and no slower than tracking
		// per-resource fences for a shutdown path.
		SDL_WaitForGPUIdle(device);

		if (State->OpaquePipeline) {
			SDL_ReleaseGPUGraphicsPipeline(device, State->OpaquePipeline);
		}
		if (State->OverlayPipeline) {
			SDL_ReleaseGPUGraphicsPipeline(device, State->OverlayPipeline);
		}
		if (State->VertexBuffer) {
			SDL_ReleaseGPUBuffer(device, State->VertexBuffer);
		}
		if (State->IndexBuffer) {
			SDL_ReleaseGPUBuffer(device, State->IndexBuffer);
		}
		if (State->InstanceBuffer) {
			SDL_ReleaseGPUBuffer(device, State->InstanceBuffer);
		}
		if (State->InstanceTransfer) {
			SDL_ReleaseGPUTransferBuffer(device, State->InstanceTransfer);
		}
		if (State->DepthTexture) {
			SDL_ReleaseGPUTexture(device, State->DepthTexture);
		}
		if (State->OverlayTexture) {
			SDL_ReleaseGPUTexture(device, State->OverlayTexture);
		}
		if (State->OverlayTransfer) {
			SDL_ReleaseGPUTransferBuffer(device, State->OverlayTransfer);
		}
		if (State->OverlaySampler) {
			SDL_ReleaseGPUSampler(device, State->OverlaySampler);
		}

		if (State->Window) {
			SDL_ReleaseWindowFromGPUDevice(device, State->Window);
		}
		SDL_DestroyGPUDevice(device);

		*State = Impl{};
	}

	FrameResult
	Renderer::Render(const Camera &camera, std::span<const Instance> instances, OverlayImage &overlay) {
		ENGINE_PROFILE_CAT("Renderer::Render", core::ProfileCategory::Render);

		FrameResult result;
		if (!State->Device) {
			return result;
		}

		SDL_GPUCommandBuffer *command = nullptr;
		{
			ENGINE_PROFILE_CAT("acquire command buffer", core::ProfileCategory::Render);
			command = SDL_AcquireGPUCommandBuffer(State->Device);
		}
		if (!command) {
			ENGINE_ERROR("SDL_AcquireGPUCommandBuffer: {}", SDL_GetError());
			return result;
		}

		SDL_GPUTexture *swapchain = nullptr;
		uint32_t width = 0;
		uint32_t height = 0;
		bool acquired = false;
		{
			// Where the frame waits, and the reason this one has a span of its
			// own before anything else does. "WaitAnd" is not decoration: with
			// vertical sync on this blocks until the display is ready, and with
			// it off it blocks until the GPU has finished with a swapchain image
			// to hand back. Either way the time is real, the CPU is idle for it,
			// and it is not a cost anything above this can do anything about.
			//
			// A frame that looks slow with everything else on the panel adding
			// up to nothing is a frame that is waiting here — which means the
			// GPU is the limit, not the code above it.
			// Idle, not Render. Nothing is being rendered here — the thread is
			// asleep until the display is ready for another image, and counting
			// that as rendering work makes the renderer look like the most
			// expensive thing in a frame it spent waiting.
			ENGINE_PROFILE_CAT("acquire swapchain", core::ProfileCategory::Idle);
			acquired =
				SDL_WaitAndAcquireGPUSwapchainTexture(command, State->Window, &swapchain, &width, &height);
		}

		if (!acquired || !swapchain) {
			// Minimised, or mid-resize. Not an error, and not a reason to stop
			// ticking — the simulation carries on and the next frame presents.
			SDL_SubmitGPUCommandBuffer(command);
			return result;
		}

		{
			// Nothing at all on a steady window, and a texture allocation on the
			// frame after a resize. Worth telling apart from the pass that uses
			// it, because one is every frame and the other is one frame.
			ENGINE_PROFILE_CAT("ensure depth", core::ProfileCategory::Render);
			if (!State->EnsureDepth(width, height)) {
				SDL_SubmitGPUCommandBuffer(command);
				return result;
			}
		}

		// --- uploads --------------------------------------------------------

		const auto instanceCount = static_cast<uint32_t>(instances.size());
		bool haveInstances = false;
		bool haveOverlay = false;

		{
			// Allocation, on the frame an overlay first appears or changes size.
			// Zero on every other frame, which is what makes a reading here
			// worth looking at rather than background noise.
			// HasContent, not IsDirty. The texture keeps the last thing uploaded
			// to it, so a frame that redraws nothing still has a panel to show —
			// which is the whole point of the image living on the GPU rather
			// than being pushed there again every frame.
			ENGINE_PROFILE_CAT("ensure overlay", core::ProfileCategory::Render);
			haveOverlay = overlay.HasContent() && !overlay.IsEmpty() &&
						  State->EnsureOverlay(overlay.GetWidth(), overlay.GetHeight());
		}

		if (instanceCount > 0) {
			bool capacity = false;
			{
				// Grows the device buffer when the scene does. Separate from the
				// copy below because one is a GPU allocation and the other is a
				// memcpy, and a spike in either means something different.
				ENGINE_PROFILE_CAT("ensure instance capacity", core::ProfileCategory::Render);
				capacity = State->EnsureInstanceCapacity(instanceCount);
			}

			if (capacity) {
				ENGINE_PROFILE_CAT("upload instances", core::ProfileCategory::Render);

				void *mapped = nullptr;
				{
					// Mapping can stall: the driver hands back memory the GPU may
					// still be reading unless it cycles, and this asks it to.
					ENGINE_PROFILE_CAT("map instances", core::ProfileCategory::Render);
					mapped = SDL_MapGPUTransferBuffer(State->Device, State->InstanceTransfer, true);
				}
				{
					// Eighty bytes an entity, straight into write-combined
					// memory. This is the other half of the traffic that made
					// collect-instances memory-bound, paid a second time.
					ENGINE_PROFILE_CAT("copy instances", core::ProfileCategory::Render);
					std::memcpy(mapped, instances.data(), instances.size() * sizeof(Instance));
				}
				SDL_UnmapGPUTransferBuffer(State->Device, State->InstanceTransfer);
				haveInstances = true;
			}
		}

		// Only when something is actually waiting to go across. A panel redrawn
		// ten times a second and presented a thousand times has nothing to
		// upload on nine hundred and ninety of those frames.
		const bool uploadOverlay =
			haveOverlay && (State->OverlayUninitialised || overlay.UploadRegion().Width > 0);

		if (haveInstances || uploadOverlay) {
			ENGINE_PROFILE_CAT("copy pass", core::ProfileCategory::Render);

			SDL_GPUCopyPass *copy = SDL_BeginGPUCopyPass(command);

			if (haveInstances) {
				const SDL_GPUTransferBufferLocation source{State->InstanceTransfer, 0};
				const SDL_GPUBufferRegion destination{
					State->InstanceBuffer,
					0,
					instanceCount * static_cast<uint32_t>(sizeof(Instance)),
				};
				// Cycling hands back a fresh allocation rather than stalling on
				// the copy the previous frame may still be reading.
				SDL_UploadToGPUBuffer(copy, &source, &destination, true);
			}

			if (uploadOverlay) {
				// Only the part of the overlay that anything has drawn on.
				//
				// The image is the size of the window and the panels are a
				// corner of it, so sending all of it meant megabytes of
				// transparent pixels per frame that had not changed since the
				// program started — measured as the largest single cost in the
				// frame. The region covers what was drawn this frame and what
				// was drawn last frame, the second being how the pixels a
				// shrinking panel vacates get told they are transparent now.
				ENGINE_PROFILE_CAT("upload overlay", core::ProfileCategory::Render);

				// The whole image on the frame the texture was created, so that
				// the parts no panel ever covers are transparent rather than
				// whatever the driver handed back.
				const auto region = State->OverlayUninitialised
										? OverlayImage::Region{0, 0, overlay.GetWidth(), overlay.GetHeight()}
										: overlay.UploadRegion();
				State->OverlayUninitialised = false;

				const auto rowBytes = static_cast<size_t>(region.Width) * OverlayImage::BYTES_PER_PIXEL;

				void *mapped = nullptr;
				{
					ENGINE_PROFILE_CAT("map overlay", core::ProfileCategory::Render);
					mapped = SDL_MapGPUTransferBuffer(State->Device, State->OverlayTransfer, true);
				}

				{
					// Row by row, because the region is narrower than the image
					// and its rows are not adjacent in it. Packed tightly on the
					// way out, which is what pixels_per_row below promises.
					ENGINE_PROFILE_CAT("copy overlay", core::ProfileCategory::Render);

					auto *destination = static_cast<uint8_t *>(mapped);
					const uint8_t *pixels = overlay.GetPixels();
					const auto stride =
						static_cast<size_t>(overlay.GetWidth()) * OverlayImage::BYTES_PER_PIXEL;

					for (int row = 0; row < region.Height; row++) {
						const size_t offset = static_cast<size_t>(region.Y + row) * stride +
											  static_cast<size_t>(region.X) * OverlayImage::BYTES_PER_PIXEL;
						std::memcpy(
							destination + static_cast<size_t>(row) * rowBytes, pixels + offset, rowBytes
						);
					}
				}

				SDL_UnmapGPUTransferBuffer(State->Device, State->OverlayTransfer);

				SDL_GPUTextureTransferInfo source{};
				source.transfer_buffer = State->OverlayTransfer;
				source.pixels_per_row = static_cast<uint32_t>(region.Width);
				source.rows_per_layer = static_cast<uint32_t>(region.Height);

				SDL_GPUTextureRegion destination{};
				destination.texture = State->OverlayTexture;
				destination.x = static_cast<uint32_t>(region.X);
				destination.y = static_cast<uint32_t>(region.Y);
				destination.w = static_cast<uint32_t>(region.Width);
				destination.h = static_cast<uint32_t>(region.Height);
				destination.d = 1;

				// False, not true. Cycling hands back a *fresh* texture, and a
				// fresh texture is uninitialised everywhere this upload does not
				// reach — which is now everywhere outside the panels.
				SDL_UploadToGPUTexture(copy, &source, &destination, false);

				// The GPU matches the image now, so nothing is pending until
				// something draws again.
				overlay.MarkUploaded();
			}

			SDL_EndGPUCopyPass(copy);
		}

		// --- opaque pass ----------------------------------------------------

		SDL_GPUColorTargetInfo colourTarget{};
		colourTarget.texture = swapchain;
		colourTarget.clear_color = SDL_FColor{0.05f, 0.06f, 0.09f, 1.0f};
		colourTarget.load_op = SDL_GPU_LOADOP_CLEAR;
		colourTarget.store_op = SDL_GPU_STOREOP_STORE;

		SDL_GPUDepthStencilTargetInfo depthTarget{};
		depthTarget.texture = State->DepthTexture;
		depthTarget.clear_depth = 1.0f;
		depthTarget.load_op = SDL_GPU_LOADOP_CLEAR;
		// Nothing reads depth after the pass, so there is no reason to write it
		// back out to memory.
		depthTarget.store_op = SDL_GPU_STOREOP_DONT_CARE;
		depthTarget.stencil_load_op = SDL_GPU_LOADOP_DONT_CARE;
		depthTarget.stencil_store_op = SDL_GPU_STOREOP_DONT_CARE;
		depthTarget.cycle = true;

		{
			ENGINE_PROFILE_CAT("opaque pass", core::ProfileCategory::Render);

			SDL_GPURenderPass *pass = SDL_BeginGPURenderPass(command, &colourTarget, 1, &depthTarget);

			if (haveInstances) {
				SDL_BindGPUGraphicsPipeline(pass, State->OpaquePipeline);

				const SDL_GPUBufferBinding vertexBindings[] = {
					{State->VertexBuffer, 0},
					{State->InstanceBuffer, 0},
				};
				SDL_BindGPUVertexBuffers(pass, 0, vertexBindings, 2);

				const SDL_GPUBufferBinding indexBinding{State->IndexBuffer, 0};
				SDL_BindGPUIndexBuffer(pass, &indexBinding, SDL_GPU_INDEXELEMENTSIZE_16BIT);

				const float aspect = static_cast<float>(width) / static_cast<float>(height);

				// No Y flip. Vulkan's clip space does point down, but SDL's
				// Vulkan backend already submits a negative-height viewport
				// "for consistency with other backends", so what reaches a
				// shader here is Y-up on every backend. Correcting it again
				// turns the scene upside down and inverts the lighting with
				// it — which reads as a shading bug rather than an orientation
				// one, and is why this comment is longer than the code.
				//
				// GLM_FORCE_DEPTH_ZERO_TO_ONE is still required: the depth
				// range is 0..1 everywhere SDL's GPU API runs.
				const glm::mat4 projection =
					glm::perspective(camera.FieldOfViewRadians, aspect, camera.NearPlane, camera.FarPlane);

				const FrameUniforms frame{
					projection * glm::inverse(camera.Frame.ToMatrix()),
				};
				SDL_PushGPUVertexUniformData(command, 0, &frame, sizeof(frame));

				const LightingUniforms lighting{
					glm::vec4{-0.45f, -0.8f, -0.4f, 0.0f},
					glm::vec4{0.26f, 0.28f, 0.34f, 1.0f},
				};
				SDL_PushGPUFragmentUniformData(command, 0, &lighting, sizeof(lighting));

				SDL_DrawGPUIndexedPrimitives(
					pass, static_cast<uint32_t>(CUBE_INDICES.size()), instanceCount, 0, 0, 0
				);

				result.DrawCalls = 1;
				result.Triangles = static_cast<uint64_t>(CUBE_INDICES.size() / 3) * instanceCount;
			}

			SDL_EndGPURenderPass(pass);
		}

		// --- overlay pass ---------------------------------------------------

		if (haveOverlay) {
			ENGINE_PROFILE_CAT("overlay pass", core::ProfileCategory::Render);

			// Load rather than clear: the scene is already in the swapchain and
			// the overlay blends on top of it.
			colourTarget.load_op = SDL_GPU_LOADOP_LOAD;

			SDL_GPURenderPass *pass = SDL_BeginGPURenderPass(command, &colourTarget, 1, nullptr);
			SDL_BindGPUGraphicsPipeline(pass, State->OverlayPipeline);

			const SDL_GPUTextureSamplerBinding binding{
				State->OverlayTexture,
				State->OverlaySampler,
			};
			SDL_BindGPUFragmentSamplers(pass, 0, &binding, 1);

			// Three vertices, no buffer: the vertex shader builds a fullscreen
			// triangle from gl_VertexIndex.
			SDL_DrawGPUPrimitives(pass, 3, 1, 0, 0);
			SDL_EndGPURenderPass(pass);

			result.DrawCalls++;
		}

		{
			// Hands the whole buffer over and queues the present. The passes
			// above only *record* commands, so almost nothing that happens in
			// them is measured by their spans — this is where the driver gets
			// the work, and where any cost of building it lands.
			ENGINE_PROFILE_CAT("submit", core::ProfileCategory::Render);
			if (!SDL_SubmitGPUCommandBuffer(command)) {
				ENGINE_ERROR("SDL_SubmitGPUCommandBuffer: {}", SDL_GetError());
				return result;
			}
		}

		result.Presented = true;
		return result;
	}
}
