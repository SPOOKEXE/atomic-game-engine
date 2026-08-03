#include <engine/core/Log.hpp>
#include <engine/core/Paths.hpp>
#include <engine/core/Profiling.hpp>
#include <engine/graph/Cull.hpp>
#include <engine/graph/Shadow.hpp>
#include <engine/render/Primitives.hpp>
#include <engine/render/Renderer.hpp>
#include <engine/scene/ActiveCamera.hpp>

#include <SDL3/SDL_gpu.h>
#include <SDL3/SDL_iostream.h>
#include <SDL3/SDL_video.h>
#include <glm/mat4x4.hpp>
#include <glm/vec4.hpp>

#include <array>
#include <cstring>
#include <vector>

namespace engine::render {

	namespace {

		using Vertex = MeshVertex;

		// One instance as the vertex shader reads it.
		//
		// The device layout, and the reason it is private: a `mat4` and a packed
		// RGBA are what a GPU wants and are exactly what may not appear in the
		// `shared` type a headless world publishes. `scene::DrawInstance` is that
		// type; this is what it becomes, once, on the way into the transfer
		// buffer.
		struct GpuInstance {
			glm::mat4 Model{1.0f};
			glm::vec4 Colour{1.0f, 1.0f, 1.0f, 1.0f};
		};

		// A draw instance in the layout the opaque pipeline binds.
		//
		// The half-extent is folded into the matrix rather than sent beside it:
		// the cube in `Primitives.hpp` is a unit cube about its own origin, so
		// scaling its columns is what turns one mesh into every box size in the
		// scene. Scale on the right of the rotation, so it stays a scale rather
		// than becoming a shear.
		GpuInstance ToGpu(const scene::DrawInstance &instance) {
			GpuInstance gpu;
			gpu.Model = instance.Frame.ToMatrix();

			// Twice the half-extent, because the mesh is one metre across and
			// the field is half of what the box measures.
			gpu.Model[0] *= instance.HalfExtent.X * 2.0f;
			gpu.Model[1] *= instance.HalfExtent.Y * 2.0f;
			gpu.Model[2] *= instance.HalfExtent.Z * 2.0f;

			// **Alpha is one minus transparency**, because a shader blends by
			// coverage and an author authors by see-through-ness. Roblox's
			// `Transparency` is the same inversion, so a script's number means
			// what its author expects on the way in and what the blender wants
			// on the way out.
			gpu.Colour =
				glm::vec4{instance.Tint.R, instance.Tint.G, instance.Tint.B, 1.0f - instance.Transparency};
			return gpu;
		}

		struct FrameUniforms {
			glm::mat4 ViewProjection;

			// World space to the light's clip space. Passed rather than
			// recomputed in the shader, because a second derivation is a second
			// chance to disagree — and the symptom would be shadows offset from
			// what casts them.
			glm::mat4 LightViewProjection;

			// World space to the surface camera's clip space, for the planar
			// projection a mirror samples with. Identity when nothing renders to
			// a surface.
			glm::mat4 SurfaceViewProjection;
		};

		struct LightingUniforms {
			glm::vec4 Direction;
			glm::vec4 Ambient;

			// x: whether a shadow map was rendered. y: one shadow texel.
			// z: whether this draw samples the surface texture. w: unused, and
			// named so the struct's size is stated rather than implied.
			glm::vec4 Flags;
		};

		// The one directional light this pipeline has.
		//
		// **A constant, and it is honest about being one.** A light is a row in
		// a world the moment anything needs two, and `scene` is where that row
		// would live — putting it there now would be a component nothing writes.
		// What this buys today is that the shadow fit and the shading agree
		// about which way the sun points, which they did not have to before
		// because only one of them existed.
		constexpr glm::vec3 SUN_DIRECTION{-0.45f, -0.8f, -0.4f};
		constexpr glm::vec4 SUN_AMBIENT{0.26f, 0.28f, 0.34f, 1.0f};

		// **2048, which is a resolution rather than a guess.** The map covers
		// the whole scene, so its texel size is the scene's extent over this —
		// about six centimetres across a 128-metre world, which is under the
		// size of the smallest thing the demo draws. Halving it doubles that and
		// the stair-stepping becomes visible on a cube edge.
		constexpr uint32_t SHADOW_RESOLUTION = 2048;

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

		// Walks `PassOrder()` as the frame is submitted.
		//
		// **Two things are checked and they are checked by different means.**
		// That this list matches `graph::StandardPipeline` is a *test* — it is
		// arithmetic over names and needs no device, so it belongs in one.
		// That `Render`'s body submits in this order is a *runtime* check,
		// because the body is the one thing a headless test cannot look at.
		//
		// Skips are allowed and are the normal case: every pass here is
		// conditional, and `graph::Stage::Optional` already says so. What is
		// refused is going backwards — a pass submitted before one that
		// preceded it in the list. That is not pedantry about bookkeeping: the
		// order *is* the correctness. The colour pass samples the shadow map,
		// so a shadow pass moved below it draws a frame lit by whatever was in
		// that memory, which on a GPU is a plausible image rather than an
		// error. `Pipeline::Validate` catches that in the description; this
		// catches it in the submission.
		//
		// Logged rather than fatal. A renderer that kills the process over its
		// own bookkeeping is worse than the bug it found, and the mismatch is
		// visible in `FrameResult::Passes` either way.
		struct PassRecorder {
			uint8_t Ran = 0;
			uint8_t Furthest = 0;
			bool Complained = false;

			void Enter(Pass pass) {
				const auto index = static_cast<uint8_t>(pass);

				if (index < Furthest && !Complained) {
					ENGINE_ERROR(
						"render pass '{}' submitted after '{}', which PassOrder puts before it",
						PassOrder()[index].Text(),
						PassOrder()[Furthest].Text()
					);
					Complained = true;
				}

				if (index > Furthest) {
					Furthest = index;
				}

				Ran |= static_cast<uint8_t>(1u << index);
			}
		};
	}

	std::span<const core::Name> PassOrder() {
		// Function-local rather than a namespace-scope array, because a
		// `core::Name` interns on construction and interning before `main` is
		// how a static initialisation order bug is written.
		static const std::array<core::Name, static_cast<size_t>(Pass::Count)> order{
			core::Name("shadow"),
			core::Name("surface"),
			core::Name("opaque"),
			core::Name("transparent"),
			core::Name("overlay"),
		};
		return order;
	}

	// -----------------------------------------------------------------------

	struct Renderer::Impl {
		SDL_Window *Window = nullptr;
		SDL_GPUDevice *Device = nullptr;

		SDL_GPUGraphicsPipeline *OpaquePipeline = nullptr;

		// The same geometry and the same shaders as the opaque pipeline, with
		// blending on and depth writes off. Two pipelines rather than one with
		// a uniform, because blend state is baked into a pipeline on every
		// modern API and cannot be changed by a draw call.
		SDL_GPUGraphicsPipeline *TransparentPipeline = nullptr;

		// The submission order, rebuilt each frame and kept so it is not
		// reallocated per frame. See `scene::OrderForDrawing`.
		std::vector<uint32_t> DrawOrder;

		// What survived culling: the indices, and the instances themselves.
		//
		// **The copy is what buys the second pass a contiguous range.** Ordering
		// over a scattered index list would leave the draw unable to say "these
		// N in a row are opaque", and `first_instance` is the only thing that
		// splits one buffer into two draws.
		std::vector<uint32_t> Visible;
		std::vector<scene::DrawInstance> VisibleInstances;

		// The whole draw list, ordered for the surface camera. What the shadow
		// pass and the surface pass draw, because neither is the eye's: a caster
		// off screen still shadows, and a mirror shows what is behind the
		// viewer.
		std::vector<scene::DrawInstance> SceneInstances;
		std::vector<uint32_t> SceneOrder;
		SDL_GPUGraphicsPipeline *OverlayPipeline = nullptr;

		SDL_GPUBuffer *VertexBuffer = nullptr;
		SDL_GPUBuffer *IndexBuffer = nullptr;

		SDL_GPUBuffer *InstanceBuffer = nullptr;
		SDL_GPUTransferBuffer *InstanceTransfer = nullptr;
		uint32_t InstanceCapacity = 0;

		SDL_GPUTexture *DepthTexture = nullptr;
		uint32_t DepthWidth = 0;
		uint32_t DepthHeight = 0;

		// --- the shadow map -------------------------------------------------
		//
		// A depth texture and the pipeline that fills it. The **same instance
		// buffer** the colour pass binds, which is what makes a shadow map one
		// more draw over data that is already on the device.
		SDL_GPUGraphicsPipeline *ShadowPipeline = nullptr;
		SDL_GPUTexture *ShadowTexture = nullptr;
		SDL_GPUSampler *ShadowSampler = nullptr;

		// --- the surface target ----------------------------------------------
		//
		// Where a `SurfaceView` renders. Colour and depth, because a view is a
		// view: the geometry it draws needs sorting by depth exactly as the
		// swapchain's does.
		// **Two of them, ping-ponged, for one reason and not the two this
		// comment used to claim.** With a single texture the surface pass bound
		// its own render target as a sampler, which is undefined behaviour on
		// every backend that checks. Writing one and binding the other makes
		// that legal.
		//
		// **It does not buy a mirror inside a mirror, and saying it did was
		// wrong for as long as it was written here.** Surface instances are
		// partitioned out of the surface pass — `sceneReflected` excludes them,
		// because a mirror sits between its own reflection camera and the world
		// and would otherwise fill its texture with itself. So no mirror is ever
		// drawn into a mirror's texture and there is no recursion to be one
		// bounce deep. What the claim actually produced was a bug: the flag that
		// says "sample the surface texture" was set for the *whole* surface
		// pass, so the floor sampled the previous frame's reflection and came
		// out as its clear colour wherever that projection landed on untouched
		// texels. A black wedge in the mirror, found by eye and not by a test.
		//
		// Real recursion needs the pass to exclude only the surface being
		// rendered *for* rather than every surface, which is a per-view
		// exclusion this pipeline has no shape for. It is the render-node
		// system's, along with everything else about several views.
		SDL_GPUTexture *SurfaceTexture[2] = {nullptr, nullptr};
		SDL_GPUTexture *SurfaceDepth = nullptr;
		SDL_GPUSampler *SurfaceSampler = nullptr;
		uint32_t SurfaceWidth = 0;
		uint32_t SurfaceHeight = 0;

		// Which of the pair the surface pass writes this frame. The other is
		// what it samples.
		uint32_t SurfaceSlot = 0;

		// Whether either texture holds a frame yet.
		//
		// The first frame has nothing to show, so a mirror draws as its own tint
		// rather than sampling whatever the driver handed back.
		bool SurfaceReady = false;

		bool EnsureShadow();
		bool EnsureSurface(uint32_t width, uint32_t height);

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

		// **Two samplers now: the shadow map and the surface.** The count is
		// part of the shader object rather than of the pipeline, so a mismatch
		// with the `layout(set = 2, binding = n)` declarations is a bind that
		// silently reads nothing rather than a validation error.
		SDL_GPUShader *opaqueFragment = LoadShader("opaque.frag", SDL_GPU_SHADERSTAGE_FRAGMENT, 2, 1);

		SDL_GPUShader *shadowVertex = LoadShader("shadow.vert", SDL_GPU_SHADERSTAGE_VERTEX, 0, 1);
		SDL_GPUShader *shadowFragment = LoadShader("shadow.frag", SDL_GPU_SHADERSTAGE_FRAGMENT, 0, 0);
		SDL_GPUShader *overlayVertex = LoadShader("overlay.vert", SDL_GPU_SHADERSTAGE_VERTEX, 0, 0);
		SDL_GPUShader *overlayFragment = LoadShader("overlay.frag", SDL_GPU_SHADERSTAGE_FRAGMENT, 1, 0);

		if (!opaqueVertex || !opaqueFragment || !shadowVertex || !shadowFragment || !overlayVertex ||
			!overlayFragment) {
			return false;
		}

		const SDL_GPUTextureFormat swapchainFormat = SDL_GetGPUSwapchainTextureFormat(Device, Window);

		// --- opaque ---------------------------------------------------------

		const SDL_GPUVertexBufferDescription vertexBuffers[] = {
			{0, sizeof(Vertex), SDL_GPU_VERTEXINPUTRATE_VERTEX, 0},
			// One step per instance: the same 36 indices are replayed for every
			// entity, and only the matrix and colour change.
			{1, sizeof(GpuInstance), SDL_GPU_VERTEXINPUTRATE_INSTANCE, 0},
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
			{6, 1, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4, offsetof(GpuInstance, Colour)},
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

		// --- shadow ---------------------------------------------------------
		//
		// The opaque pipeline with **no colour target at all** and the other
		// face culled. Front-face culling is the classic trick: rendering back
		// faces into the map moves the recorded depth to the far side of each
		// object, which pushes self-shadowing acne behind the surface that would
		// have shown it. It costs a little peter-panning on thin geometry, which
		// the slope-scaled bias in the fragment shader is sized against.
		SDL_GPUGraphicsPipelineCreateInfo shadow = opaque;
		shadow.vertex_shader = shadowVertex;
		shadow.fragment_shader = shadowFragment;
		shadow.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_FRONT;
		shadow.target_info.color_target_descriptions = nullptr;
		shadow.target_info.num_color_targets = 0;

		ShadowPipeline = SDL_CreateGPUGraphicsPipeline(Device, &shadow);
		if (!ShadowPipeline) {
			ENGINE_ERROR("shadow pipeline: {}", SDL_GetError());
		}

		// --- transparent ----------------------------------------------------
		//
		// The opaque pipeline with two changes, and each one is the whole
		// reason a second pipeline exists:
		//
		// - **Blending on**, source alpha over one-minus-source-alpha. Not
		//   premultiplied, unlike the overlay below: these instances carry a
		//   straight `Color3` from `Visual::Tint`, and premultiplying it in the
		//   producer would make the same colour mean two things depending on
		//   which pass read it.
		// - **Depth writes off, depth *test* on.** A transparent pane must be
		//   hidden by an opaque wall in front of it, so the test stays; but it
		//   must not stop the pane behind it from being drawn, so the write
		//   goes. Leaving the write on is the classic version of this bug — the
		//   nearest pane silently erases everything behind it and the scene
		//   looks like the sort failed.
		SDL_GPUColorTargetDescription blendedTarget{};
		blendedTarget.format = swapchainFormat;
		blendedTarget.blend_state.enable_blend = true;
		blendedTarget.blend_state.color_blend_op = SDL_GPU_BLENDOP_ADD;
		blendedTarget.blend_state.alpha_blend_op = SDL_GPU_BLENDOP_ADD;
		blendedTarget.blend_state.src_color_blendfactor = SDL_GPU_BLENDFACTOR_SRC_ALPHA;
		blendedTarget.blend_state.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
		blendedTarget.blend_state.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
		blendedTarget.blend_state.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;

		SDL_GPUGraphicsPipelineCreateInfo transparent = opaque;
		transparent.depth_stencil_state.enable_depth_write = false;
		transparent.target_info.color_target_descriptions = &blendedTarget;

		// **Back faces are drawn too.** A cube with see-through walls shows its
		// own far side, and culling it leaves a shape that reads as hollow
		// rather than as glass.
		transparent.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;

		TransparentPipeline = SDL_CreateGPUGraphicsPipeline(Device, &transparent);
		if (!TransparentPipeline) {
			ENGINE_ERROR("transparent pipeline: {}", SDL_GetError());
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

		SDL_ReleaseGPUShader(Device, shadowVertex);
		SDL_ReleaseGPUShader(Device, shadowFragment);

		return OpaquePipeline != nullptr && TransparentPipeline != nullptr && ShadowPipeline != nullptr &&
			   OverlayPipeline != nullptr;
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

		const uint32_t bytes = capacity * static_cast<uint32_t>(sizeof(GpuInstance));

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

	bool Renderer::Impl::EnsureShadow() {
		if (ShadowTexture != nullptr) {
			return true;
		}

		SDL_GPUTextureCreateInfo info{};
		info.type = SDL_GPU_TEXTURETYPE_2D;
		info.format = SDL_GPU_TEXTUREFORMAT_D32_FLOAT;

		// **Both usages, and the sampler one is the point.** A depth attachment
		// that is only a target cannot be read, and a shadow map that cannot be
		// read is a pass that costs a draw and changes nothing.
		info.usage = SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
		info.width = SHADOW_RESOLUTION;
		info.height = SHADOW_RESOLUTION;
		info.layer_count_or_depth = 1;
		info.num_levels = 1;
		info.sample_count = SDL_GPU_SAMPLECOUNT_1;

		ShadowTexture = SDL_CreateGPUTexture(Device, &info);
		if (!ShadowTexture) {
			ENGINE_ERROR("shadow texture: {}", SDL_GetError());
			return false;
		}

		SDL_GPUSamplerCreateInfo sampler{};
		sampler.min_filter = SDL_GPU_FILTER_LINEAR;
		sampler.mag_filter = SDL_GPU_FILTER_LINEAR;
		sampler.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;

		// **Clamped to the edge, and the fragment shader also range-checks.**
		// Either alone would do; both, because a wrap mode would tile the map
		// across the world and the range check is what makes "outside the map is
		// lit" a stated rule rather than a property of a sampler setting.
		sampler.address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
		sampler.address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
		sampler.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;

		ShadowSampler = SDL_CreateGPUSampler(Device, &sampler);
		if (!ShadowSampler) {
			ENGINE_ERROR("shadow sampler: {}", SDL_GetError());
			return false;
		}
		return true;
	}

	bool Renderer::Impl::EnsureSurface(uint32_t width, uint32_t height) {
		if (SurfaceTexture[0] != nullptr && width == SurfaceWidth && height == SurfaceHeight) {
			return true;
		}

		for (SDL_GPUTexture *&texture : SurfaceTexture) {
			if (texture) {
				SDL_ReleaseGPUTexture(Device, texture);
				texture = nullptr;
			}
		}
		if (SurfaceDepth) {
			SDL_ReleaseGPUTexture(Device, SurfaceDepth);
			SurfaceDepth = nullptr;
		}

		// Resized, so whatever it held is gone. A mirror that showed the last
		// frame at the old resolution stretched across the new one would be a
		// visible artefact on exactly the frame a window was dragged.
		SurfaceReady = false;

		SDL_GPUTextureCreateInfo colour{};
		colour.type = SDL_GPU_TEXTURETYPE_2D;
		colour.format = SDL_GetGPUSwapchainTextureFormat(Device, Window);
		colour.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
		colour.width = width;
		colour.height = height;
		colour.layer_count_or_depth = 1;
		colour.num_levels = 1;
		colour.sample_count = SDL_GPU_SAMPLECOUNT_1;

		for (SDL_GPUTexture *&texture : SurfaceTexture) {
			texture = SDL_CreateGPUTexture(Device, &colour);
			if (!texture) {
				ENGINE_ERROR("surface texture {}x{}: {}", width, height, SDL_GetError());
				return false;
			}
		}

		SDL_GPUTextureCreateInfo depth{};
		depth.type = SDL_GPU_TEXTURETYPE_2D;
		depth.format = SDL_GPU_TEXTUREFORMAT_D32_FLOAT;
		depth.usage = SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET;
		depth.width = width;
		depth.height = height;
		depth.layer_count_or_depth = 1;
		depth.num_levels = 1;
		depth.sample_count = SDL_GPU_SAMPLECOUNT_1;

		SurfaceDepth = SDL_CreateGPUTexture(Device, &depth);
		if (!SurfaceDepth) {
			ENGINE_ERROR("surface depth {}x{}: {}", width, height, SDL_GetError());
			return false;
		}

		if (SurfaceSampler == nullptr) {
			SDL_GPUSamplerCreateInfo sampler{};
			sampler.min_filter = SDL_GPU_FILTER_LINEAR;
			sampler.mag_filter = SDL_GPU_FILTER_LINEAR;
			sampler.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
			sampler.address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
			sampler.address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
			sampler.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;

			SurfaceSampler = SDL_CreateGPUSampler(Device, &sampler);
			if (!SurfaceSampler) {
				ENGINE_ERROR("surface sampler: {}", SDL_GetError());
				return false;
			}
		}

		SurfaceWidth = width;
		SurfaceHeight = height;
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
		if (State->TransparentPipeline) {
			SDL_ReleaseGPUGraphicsPipeline(device, State->TransparentPipeline);
		}
		if (State->ShadowPipeline) {
			SDL_ReleaseGPUGraphicsPipeline(device, State->ShadowPipeline);
		}
		if (State->ShadowTexture) {
			SDL_ReleaseGPUTexture(device, State->ShadowTexture);
		}
		if (State->ShadowSampler) {
			SDL_ReleaseGPUSampler(device, State->ShadowSampler);
		}
		for (SDL_GPUTexture *texture : State->SurfaceTexture) {
			if (texture) {
				SDL_ReleaseGPUTexture(device, texture);
			}
		}
		if (State->SurfaceDepth) {
			SDL_ReleaseGPUTexture(device, State->SurfaceDepth);
		}
		if (State->SurfaceSampler) {
			SDL_ReleaseGPUSampler(device, State->SurfaceSampler);
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

	FrameResult Renderer::Render(
		const core::CFrame &cameraFrame,
		const scene::Camera &camera,
		std::span<const scene::DrawInstance> instances,
		OverlayImage &overlay,
		const SurfaceView *surface
	) {
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

		const auto totalCount = static_cast<uint32_t>(instances.size());
		bool haveInstances = false;
		bool haveOverlay = false;

		// **Culled, then ordered, then uploaded** — and the sequence is the
		// point. Culling first means the sort runs over what survives rather
		// than over the world, and the upload carries only what is drawn.
		//
		// The frustum comes from the same `ResolveCamera` the draw does, so it
		// cannot disagree with what was actually projected. A frustum built from
		// a field of view and an aspect ratio kept separately is the bug that
		// pops geometry at the screen edge on one machine and not another.
		// **Fitted to the whole draw list, not to what survived culling.** A
		// caster outside the camera's frustum still shadows into it, so the
		// light has to see everything — and fitting to the culled set is the
		// classic version of this bug: shadows that vanish as their casters
		// leave the screen.
		const glm::mat4 lightViewProjection = graph::FitDirectionalLight(
			graph::BoundsOfAll(instances), core::Vector3{SUN_DIRECTION.x, SUN_DIRECTION.y, SUN_DIRECTION.z}
		);

		// The surface camera's own view, used both to render into the texture
		// and — one frame later — to project it back onto whatever samples it.
		glm::mat4 surfaceViewProjection{1.0f};
		if (surface != nullptr) {
			const float surfaceAspect =
				static_cast<float>(surface->Width) / static_cast<float>(std::max(surface->Height, 1u));
			surfaceViewProjection =
				scene::ResolveCamera(surface->Frame, surface->Lens, surfaceAspect).ViewProjection;
		}

		size_t visibleCount = 0;
		{
			ENGINE_PROFILE_CAT("cull instances", core::ProfileCategory::Render);

			const float aspect = static_cast<float>(width) / static_cast<float>(height);
			const graph::Frustum frustum = graph::Frustum::FromViewProjection(
				scene::ResolveCamera(cameraFrame, camera, aspect).ViewProjection
			);
			visibleCount = graph::Cull(instances, frustum, State->Visible);
		}

		// Ordered over the survivors, so the two passes are two ranges of one
		// buffer. Opaque first in whatever order the world produced, then the
		// transparent tail back to front from where the camera is.
		State->VisibleInstances.resize(visibleCount);
		for (size_t index = 0; index < visibleCount; index++) {
			State->VisibleInstances[index] = instances[State->Visible[index]];
		}

		size_t opaqueCount = 0;
		{
			ENGINE_PROFILE_CAT("order instances", core::ProfileCategory::Render);
			opaqueCount =
				scene::OrderForDrawing(State->VisibleInstances, cameraFrame.Position, State->DrawOrder);
		}
		const auto transparentCount = static_cast<uint32_t>(visibleCount - opaqueCount);

		// **Surface instances moved to the back of the opaque head**, so the
		// camera range is three contiguous runs — plain opaque, then mirrors,
		// then transparent — and each is one draw with one `first_instance`.
		// Whether an instance samples the surface is per instance and the
		// uniform that says so is per draw, so the alternative is a branch on
		// data the fragment shader does not have.
		//
		// Stable, for the reason the ordering itself is: an opaque scene with no
		// mirrors must come out of this exactly as it went in.
		uint32_t surfaceInCamera = 0;
		if (opaqueCount > 0) {
			ENGINE_PROFILE_CAT("partition surfaces", core::ProfileCategory::Render);

			const auto opaqueEnd = State->DrawOrder.begin() + static_cast<ptrdiff_t>(opaqueCount);
			const auto boundary =
				std::stable_partition(State->DrawOrder.begin(), opaqueEnd, [&](uint32_t index) {
					return State->VisibleInstances[index].Surface < 0;
				});

			surfaceInCamera = static_cast<uint32_t>(std::distance(boundary, opaqueEnd));
		}
		const auto plainOpaque = static_cast<uint32_t>(opaqueCount) - surfaceInCamera;

		// **A second range holding everything, for the two passes that are not
		// the camera's.** A caster outside the camera's frustum still shadows
		// into it, and a mirror shows what is behind the viewer — so culling to
		// the eye would give shadows that vanish as their casters leave the
		// screen and a mirror that reflects only what is already on screen.
		// Both are the classic version of this mistake.
		//
		// Ordered from the surface camera when there is one, because the surface
		// pass is the only consumer that needs an order at all — a depth-only
		// pass does not care.
		const bool wantSurface = surface != nullptr && State->EnsureSurface(surface->Width, surface->Height);
		const core::Vector3 sceneEye = wantSurface ? surface->Frame.Position : cameraFrame.Position;

		State->SceneInstances.assign(instances.begin(), instances.end());

		size_t sceneOpaque = 0;
		{
			ENGINE_PROFILE_CAT("order scene", core::ProfileCategory::Render);
			sceneOpaque = scene::OrderForDrawing(State->SceneInstances, sceneEye, State->SceneOrder);
		}
		const auto sceneCount = static_cast<uint32_t>(State->SceneInstances.size());
		const auto sceneTransparent = static_cast<uint32_t>(sceneCount - sceneOpaque);

		// **Surface instances to the back of the scene's opaque head, so the
		// surface pass can skip them.** A mirror sits between its own reflection
		// camera and the world — the camera is *behind* the plane looking
		// through it — so drawing the pane into its own reflection fills the
		// texture with the pane. The mirror then shows itself, which reads as a
		// mirror that is not working at all.
		//
		// Physically right as well as necessary: nothing sees itself in its own
		// reflection.
		uint32_t sceneSurfaces = 0;
		if (sceneOpaque > 0) {
			const auto opaqueEnd = State->SceneOrder.begin() + static_cast<ptrdiff_t>(sceneOpaque);
			const auto boundary =
				std::stable_partition(State->SceneOrder.begin(), opaqueEnd, [&](uint32_t index) {
					return State->SceneInstances[index].Surface < 0;
				});

			sceneSurfaces = static_cast<uint32_t>(std::distance(boundary, opaqueEnd));
		}
		const auto sceneReflected = static_cast<uint32_t>(sceneOpaque) - sceneSurfaces;

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

		const auto instanceCount = static_cast<uint32_t>(visibleCount);
		result.Culled = totalCount - instanceCount;

		// **One buffer, two ranges.** The scene range first so its offset is
		// zero and the camera range starts where it ends — which is what makes
		// each pass one `first_instance` rather than a second buffer and a
		// second bind.
		const uint32_t uploadCount = sceneCount + instanceCount;

		if (uploadCount > 0) {
			bool capacity = false;
			{
				// Grows the device buffer when the scene does. Separate from the
				// copy below because one is a GPU allocation and the other is a
				// memcpy, and a spike in either means something different.
				ENGINE_PROFILE_CAT("ensure instance capacity", core::ProfileCategory::Render);
				capacity = State->EnsureInstanceCapacity(uploadCount);
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
					// Converted straight into the mapped buffer rather than
					// into a vector that is then memcpy'd. Eighty bytes an
					// entity go into write-combined memory either way, and the
					// staging copy would be that traffic paid a third time —
					// once by the world filling its draw list, once here, and
					// once again on the way out.
					//
					// This is where a `CFrame` and a `Color3` become a `mat4`
					// and an RGBA, and it is the only place in the engine that
					// happens. A world produces scene data; a device layout is
					// this module's business.
					ENGINE_PROFILE_CAT("convert instances", core::ProfileCategory::Render);

					auto *out = static_cast<GpuInstance *>(mapped);

					for (size_t index = 0; index < State->SceneOrder.size(); index++) {
						out[index] = ToGpu(State->SceneInstances[State->SceneOrder[index]]);
					}

					auto *camera = out + State->SceneOrder.size();
					for (size_t index = 0; index < State->DrawOrder.size(); index++) {
						camera[index] = ToGpu(State->VisibleInstances[State->DrawOrder[index]]);
					}
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
					uploadCount * static_cast<uint32_t>(sizeof(GpuInstance)),
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

		// The passes below, recorded as they are submitted. See `PassRecorder`.
		PassRecorder passes;

		// --- shadow pass ----------------------------------------------------
		//
		// **The scene range, not the camera's**, and no colour target at all.
		// Every caster casts, whether or not the eye can see it.
		const bool haveShadow = haveInstances && sceneCount > 0 && State->EnsureShadow();

		if (haveShadow) {
			ENGINE_PROFILE_CAT("shadow pass", core::ProfileCategory::Render);
			passes.Enter(Pass::Shadow);

			SDL_GPUDepthStencilTargetInfo shadowTarget{};
			shadowTarget.texture = State->ShadowTexture;
			shadowTarget.clear_depth = 1.0f;
			shadowTarget.load_op = SDL_GPU_LOADOP_CLEAR;

			// **Stored, unlike the colour pass's depth.** This one is read by
			// the next pass, which is the entire point of rendering it.
			shadowTarget.store_op = SDL_GPU_STOREOP_STORE;
			shadowTarget.stencil_load_op = SDL_GPU_LOADOP_DONT_CARE;
			shadowTarget.stencil_store_op = SDL_GPU_STOREOP_DONT_CARE;
			shadowTarget.cycle = true;

			SDL_GPURenderPass *pass = SDL_BeginGPURenderPass(command, nullptr, 0, &shadowTarget);
			SDL_BindGPUGraphicsPipeline(pass, State->ShadowPipeline);

			const SDL_GPUBufferBinding vertexBindings[] = {
				{State->VertexBuffer, 0},
				{State->InstanceBuffer, 0},
			};
			SDL_BindGPUVertexBuffers(pass, 0, vertexBindings, 2);

			const SDL_GPUBufferBinding indexBinding{State->IndexBuffer, 0};
			SDL_BindGPUIndexBuffer(pass, &indexBinding, SDL_GPU_INDEXELEMENTSIZE_16BIT);

			SDL_PushGPUVertexUniformData(command, 0, &lightViewProjection, sizeof(lightViewProjection));

			// **Only the opaque part of the scene casts.** A transparent pane
			// writing full depth into the shadow map would cast a solid shadow,
			// which is the most obviously wrong thing glass can do.
			SDL_DrawGPUIndexedPrimitives(
				pass, static_cast<uint32_t>(CUBE_INDICES.size()), static_cast<uint32_t>(sceneOpaque), 0, 0, 0
			);

			SDL_EndGPURenderPass(pass);
			result.DrawCalls++;
		}

		// --- surface pass ----------------------------------------------------
		//
		// The same scene range, from the surface camera, into a texture. What a
		// mirror shows next frame — the one-frame staleness `ViewChannel`
		// already assumed, and what breaks the dependency cycle between a mirror
		// and what it reflects.
		if (wantSurface && haveInstances && sceneCount > 0) {
			ENGINE_PROFILE_CAT("surface pass", core::ProfileCategory::Render);
			passes.Enter(Pass::Surface);

			// Flipped before the pass, so what it writes is not what it reads.
			State->SurfaceSlot ^= 1u;

			SDL_GPUColorTargetInfo surfaceColour{};
			surfaceColour.texture = State->SurfaceTexture[State->SurfaceSlot];
			surfaceColour.clear_color = SDL_FColor{0.05f, 0.06f, 0.09f, 1.0f};
			surfaceColour.load_op = SDL_GPU_LOADOP_CLEAR;
			surfaceColour.store_op = SDL_GPU_STOREOP_STORE;
			surfaceColour.cycle = true;

			SDL_GPUDepthStencilTargetInfo surfaceDepth{};
			surfaceDepth.texture = State->SurfaceDepth;
			surfaceDepth.clear_depth = 1.0f;
			surfaceDepth.load_op = SDL_GPU_LOADOP_CLEAR;
			surfaceDepth.store_op = SDL_GPU_STOREOP_DONT_CARE;
			surfaceDepth.stencil_load_op = SDL_GPU_LOADOP_DONT_CARE;
			surfaceDepth.stencil_store_op = SDL_GPU_STOREOP_DONT_CARE;
			surfaceDepth.cycle = true;

			SDL_GPURenderPass *pass = SDL_BeginGPURenderPass(command, &surfaceColour, 1, &surfaceDepth);
			SDL_BindGPUGraphicsPipeline(pass, State->OpaquePipeline);

			const SDL_GPUBufferBinding vertexBindings[] = {
				{State->VertexBuffer, 0},
				{State->InstanceBuffer, 0},
			};
			SDL_BindGPUVertexBuffers(pass, 0, vertexBindings, 2);

			const SDL_GPUBufferBinding indexBinding{State->IndexBuffer, 0};
			SDL_BindGPUIndexBuffer(pass, &indexBinding, SDL_GPU_INDEXELEMENTSIZE_16BIT);

			const FrameUniforms surfaceFrame{
				surfaceViewProjection,
				lightViewProjection,
				glm::mat4{1.0f},
			};
			SDL_PushGPUVertexUniformData(command, 0, &surfaceFrame, sizeof(surfaceFrame));

			// **Shadowed, and pointedly not surfaced.** The mirror's own view
			// gets the shadow map, so what it reflects is lit the way the screen
			// lights it.
			//
			// **`Flags.z` is zero, and it has to be.** It means "this draw
			// samples the surface texture instead of its own tint", and the
			// screen pass sets it for exactly one draw — the instances that
			// carry a `Surface`. This pass has none: `sceneReflected` partitions
			// them out. Setting it here therefore cannot reach a mirror; it can
			// only reach everything that is *not* one, and that is what it did.
			// Every object in the reflection sampled the previous frame's
			// surface texture, projected from this camera, and the floor came
			// out as the clear colour wherever that landed on texels the last
			// frame never wrote — a black wedge in the mirror that survived
			// deleting every caster, the frame and the near-plane hack, and
			// moved when the camera was re-aimed but not when the floor was.
			// It was pinned to a projected texture coordinate, and this uniform
			// is the only thing in this pass that has one.
			const LightingUniforms surfaceLighting{
				glm::vec4{SUN_DIRECTION, 0.0f},
				SUN_AMBIENT,
				glm::vec4{haveShadow ? 1.0f : 0.0f, 1.0f / static_cast<float>(SHADOW_RESOLUTION), 0.0f, 0.0f},
			};
			SDL_PushGPUFragmentUniformData(command, 0, &surfaceLighting, sizeof(surfaceLighting));

			SDL_GPUTexture *const previous = State->SurfaceTexture[State->SurfaceSlot ^ 1u];
			const SDL_GPUTextureSamplerBinding samplers[] = {
				{State->ShadowTexture != nullptr ? State->ShadowTexture : previous,
				 State->ShadowSampler != nullptr ? State->ShadowSampler : State->SurfaceSampler},
				{previous, State->SurfaceSampler},
			};
			SDL_BindGPUFragmentSamplers(pass, 0, samplers, 2);

			// **The same two-pipeline split the screen pass makes.** Drawing the
			// whole range with the opaque pipeline would reflect a glass pane as
			// a solid one, which is the sort of difference between a mirror and
			// the world it shows that nobody looks for.
			if (sceneReflected > 0) {
				SDL_DrawGPUIndexedPrimitives(
					pass, static_cast<uint32_t>(CUBE_INDICES.size()), sceneReflected, 0, 0, 0
				);
				result.DrawCalls++;
			}

			if (sceneTransparent > 0) {
				SDL_BindGPUGraphicsPipeline(pass, State->TransparentPipeline);
				SDL_DrawGPUIndexedPrimitives(
					pass,
					static_cast<uint32_t>(CUBE_INDICES.size()),
					sceneTransparent,
					0,
					0,
					static_cast<uint32_t>(sceneOpaque)
				);
				result.DrawCalls++;
			}

			SDL_EndGPURenderPass(pass);

			// From here a surface texture holds a frame, so the screen pass may
			// sample the one just written and the next surface pass may sample
			// it as its previous.
			State->SurfaceReady = true;
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

			// Entered unconditionally, and that is the honest reading rather
			// than a convenience: the stage clears colour and depth, so a frame
			// with nothing in it still ran this pass — the background is what it
			// drew. `Validate` sees the same thing, because the stage's writes
			// are marked `Clear`.
			passes.Enter(Pass::Opaque);

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

				// `scene::ResolveCamera`, not a projection built here. It is the
				// one place the engine decides what a camera's matrices are, and
				// a second copy is a second chance to disagree about handedness,
				// clip depth or the order of the product — a disagreement that
				// reads as z-fighting rather than as a matrix mistake.
				//
				// The Y convention that used to need a comment here lives there
				// too: no flip, because SDL's Vulkan backend already submits a
				// negative-height viewport "for consistency with other
				// backends".
				//
				// The aspect ratio is the swapchain's rather than a caller's, so
				// a frame taken mid-resize is projected for the image it is
				// actually drawn into.
				const FrameUniforms frame{
					scene::ResolveCamera(cameraFrame, camera, aspect).ViewProjection,
					lightViewProjection,
					surfaceViewProjection,
				};
				SDL_PushGPUVertexUniformData(command, 0, &frame, sizeof(frame));

				// **The surface flag is off for the opaque range and on for a
				// second draw over the instances that carry one.** Whether an
				// instance samples the surface is per instance and the uniform
				// is per draw, so the split is a third draw rather than a
				// per-fragment branch on data the shader does not have.
				const LightingUniforms lighting{
					glm::vec4{SUN_DIRECTION, 0.0f},
					SUN_AMBIENT,
					glm::vec4{
						haveShadow ? 1.0f : 0.0f, 1.0f / static_cast<float>(SHADOW_RESOLUTION), 0.0f, 0.0f
					},
				};
				SDL_PushGPUFragmentUniformData(command, 0, &lighting, sizeof(lighting));

				// Both samplers, every draw. A shadow map that was not rendered
				// binds the surface texture in its place rather than nothing:
				// the flag above is what stops it being read, and an unbound
				// sampler is undefined behaviour on several backends where a
				// wrongly-bound one is merely ignored.
				SDL_GPUTexture *const shadow =
					State->ShadowTexture != nullptr ? State->ShadowTexture : State->OverlayTexture;
				SDL_GPUSampler *const shadowSampler =
					State->ShadowSampler != nullptr ? State->ShadowSampler : State->OverlaySampler;
				// **The one just written**, so the screen shows this frame's
				// reflection rather than last frame's. Only what a mirror sees
				// *of another mirror* is a frame behind.
				SDL_GPUTexture *const surfaceMap = State->SurfaceTexture[State->SurfaceSlot] != nullptr
													   ? State->SurfaceTexture[State->SurfaceSlot]
													   : shadow;
				SDL_GPUSampler *const surfaceSampler =
					State->SurfaceSampler != nullptr ? State->SurfaceSampler : shadowSampler;

				if (shadow != nullptr && surfaceMap != nullptr) {
					const SDL_GPUTextureSamplerBinding samplers[] = {
						{shadow, shadowSampler},
						{surfaceMap, surfaceSampler},
					};
					SDL_BindGPUFragmentSamplers(pass, 0, samplers, 2);
				}

				// **Two draws over one buffer, split at the boundary the
				// ordering produced.** `first_instance` is what makes that
				// possible without a second upload: the instance attributes are
				// per-instance vertex data, so the offset picks up where the
				// opaque range left off.
				if (plainOpaque > 0) {
					SDL_DrawGPUIndexedPrimitives(
						pass, static_cast<uint32_t>(CUBE_INDICES.size()), plainOpaque, 0, 0, sceneCount
					);
					result.DrawCalls++;
				}

				// **The surface draw, over the instances that show one.** They
				// were partitioned to the front of the *camera* range by the
				// ordering below, so this is one contiguous run rather than a
				// per-instance branch.
				if (surfaceInCamera > 0 && State->SurfaceReady) {
					const LightingUniforms mirrored{
						glm::vec4{SUN_DIRECTION, 0.0f},
						SUN_AMBIENT,
						glm::vec4{
							haveShadow ? 1.0f : 0.0f, 1.0f / static_cast<float>(SHADOW_RESOLUTION), 1.0f, 0.0f
						},
					};
					SDL_PushGPUFragmentUniformData(command, 0, &mirrored, sizeof(mirrored));

					SDL_DrawGPUIndexedPrimitives(
						pass,
						static_cast<uint32_t>(CUBE_INDICES.size()),
						surfaceInCamera,
						0,
						0,
						sceneCount + plainOpaque
					);

					result.DrawCalls++;
					result.SurfaceInstances = surfaceInCamera;

					// Back to the ordinary uniform, so the transparent draw
					// below does not inherit the mirror flag.
					SDL_PushGPUFragmentUniformData(command, 0, &lighting, sizeof(lighting));
				}

				if (transparentCount > 0) {
					// Same pass, same depth attachment, different pipeline —
					// blending on and depth writes off. A separate render pass
					// would have to reload the depth buffer, and the whole point
					// is that these fragments are tested against what the opaque
					// pass already wrote.
					//
					// Still its own stage, sharing a render pass. What the list
					// describes is what is drawn and in what order, not how many
					// times a target is bound.
					passes.Enter(Pass::Transparent);

					SDL_BindGPUGraphicsPipeline(pass, State->TransparentPipeline);
					SDL_DrawGPUIndexedPrimitives(
						pass,
						static_cast<uint32_t>(CUBE_INDICES.size()),
						transparentCount,
						0,
						0,
						sceneCount + static_cast<uint32_t>(opaqueCount)
					);
					result.DrawCalls++;
				}

				result.Triangles = static_cast<uint64_t>(CUBE_INDICES.size() / 3) * instanceCount;
			}

			SDL_EndGPURenderPass(pass);
		}

		// --- overlay pass ---------------------------------------------------

		if (haveOverlay) {
			ENGINE_PROFILE_CAT("overlay pass", core::ProfileCategory::Render);
			passes.Enter(Pass::Overlay);

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

		// Before the submit rather than after it, so a frame that fails to
		// submit still reports what it built.
		result.Passes = passes.Ran;

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
