// Building a device pipeline, and installing an authored graph.
//
// **The two halves belong together because a graph is only accepted once every
// node in it has a backend.** `CompileRenderPipeline` refuses a document whose
// resources do not resolve or whose shaders do not build, and it refuses it at
// `SetPipeline` rather than in a frame - so the failure names the node and the
// reason instead of appearing as a black screen. `tests/Passes.cpp` is the
// suite that holds that line.

#include "DisplayColour.hpp"
#include "RenderTypes.hpp"
#include "RendererState.hpp"
#include "ShaderBinary.hpp"
#include "VulkanTimestamps.hpp"

#include <engine/core/Log.hpp>
#include <engine/core/Profiling.hpp>
#include <engine/graph/PipelineDocument.hpp>
#include <engine/render/ShaderCompiler.hpp>
#include <engine/render/ShaderLibrary.hpp>
#include <engine/resources/Shaders.hpp>
#include <engine/scene/Sunlight.hpp>

#include <SDL3/SDL_gpu.h>
#include <glm/mat4x4.hpp>
#include <glm/vec4.hpp>

#include <algorithm>
#include <array>
#include <cstring>
#include <span>
#include <string>
#include <unordered_set>
#include <vector>

namespace engine::render {

	namespace {
		// Keep the GPU vertex layout identical to the asset vertex layout.
		using Vertex = assets::MeshVertex;

		bool CompileRenderPipeline(
			const graph::RenderGraph &pipeline,
			graph::CompiledGraph &compiled,
			graph::ExecutionSchedule &schedule,
			core::Name &offender,
			std::string &reason,
			const DeviceCaps *caps = nullptr,
			std::span<const core::Name> customKinds = {}
		) {
			const graph::GraphStatus graphStatus = pipeline.Compile(compiled, offender);
			if (graphStatus != graph::GraphStatus::Ok) {
				reason = graph::Describe(graphStatus);
				return false;
			}
			const graph::ScheduleStatus status = graph::CompileSchedule(pipeline, schedule, offender);
			if (status != graph::ScheduleStatus::Ok) {
				reason = graph::Describe(status);
				return false;
			}

			// Resource edges, not canvas or declaration position, own execution.
			// SDL records each dependency wave serially, preserving the schedule on a
			// backend that exposes one portable command stream.
			compiled.Shared.clear();
			compiled.PerView.clear();
			compiled.Final.clear();
			for (const graph::ExecutionWave &wave : schedule.Waves) {
				for (const graph::ScheduledNode &scheduled : wave.Nodes) {
					const graph::Node *node = pipeline.Find(scheduled.Node);
					if (node == nullptr) {
						offender = {};
						reason = "the compiled schedule names no node";
						return false;
					}
					switch (node->Scope) {
					case graph::NodeScope::World:
						compiled.Shared.push_back(scheduled.Node);
						break;
					case graph::NodeScope::View:
						compiled.PerView.push_back(scheduled.Node);
						break;
					case graph::NodeScope::Frame:
						compiled.Final.push_back(scheduled.Node);
						break;
					}
				}
			}

			NodeTable available = BackendTable([](const graph::RunContext &) { return true; });
			for (const core::Name kind : customKinds) {
				available.Set(kind, [](const graph::RunContext &) { return true; });
			}
			const std::vector<core::Name> missing = available.Missing(pipeline);
			if (!missing.empty()) {
				offender = missing.front();
				reason = "the renderer has no backend node for this kind";
				return false;
			}

			std::unordered_set<uint32_t> seen;
			for (const graph::ExecutionWave &wave : schedule.Waves) {
				for (const graph::ScheduledNode &scheduled : wave.Nodes) {
					const graph::Node *node = pipeline.Find(scheduled.Node);
					if (node == nullptr) {
						offender = {};
						reason = "the compiled schedule names no node";
						return false;
					}

					const graph::NodeKindSpec *spec = graph::NodeCatalogue::Find(node->Kind);
					if (spec == nullptr) {
						offender = node->Name;
						reason = "the render catalogue has no declaration for this kind";
						return false;
					}
					graph::NodeRequirements needs = spec->Needs;
					if (node->Kind == core::Name("blit")) {
						if (node->Reads.size() != 1 || node->Writes.size() != 1) {
							offender = node->Name;
							reason = "blit needs exactly one source and one target";
							return false;
						}
						graph::ResourceFormat targetFormat = graph::ResourceFormat::RGBA16F;
						if (const std::string *format = node->Parameter(core::Name("format"));
							format != nullptr && !graph::ParseResourceFormat(*format, targetFormat)) {
							offender = node->Name;
							reason = "blit target format is not recognised";
							return false;
						}
						const graph::ResourceDesc *target = pipeline.FindResource(node->Writes.front());
						if (target == nullptr || target->Format != targetFormat) {
							offender = node->Name;
							reason = "blit target resource does not use its selected format";
							return false;
						}
						needs.Formats = {targetFormat};
					}
					if (caps != nullptr) {
						const CapabilityCheck capability = CheckCapabilities(*caps, needs);
						if (!capability.Accepted()) {
							offender = node->Name;
							reason = Describe(capability.Status);
							if (capability.Status == CapabilityStatus::MissingFormat) {
								reason += ": ";
								reason += graph::Describe(capability.Format);
							}
							return false;
						}
					}
					if (seen.contains(node->Kind.Id()) && !spec->Repeatable) {
						offender = node->Name;
						reason = "this render node kind may appear only once";
						return false;
					}
					if (node->Scope != spec->Scope && !spec->FlexibleScope) {
						offender = node->Name;
						reason = "this backend node cannot run at the authored scope";
						return false;
					}
					seen.insert(node->Kind.Id());
					if (const std::string *queue = node->Parameter(core::Name("queue"));
						queue != nullptr && *queue != "auto" && *queue != graph::Describe(spec->Queue)) {
						offender = node->Name;
						reason = "this backend node requires the " +
								 std::string(graph::Describe(spec->Queue)) + " queue";
						return false;
					}
					if (const std::string *culling = node->Parameter(core::Name("culling"));
						culling != nullptr) {
						if (*culling != "inherit" && node->Kind != core::Name("cull-frustum")) {
							offender = node->Name;
							reason = "culling belongs on an entity filter node, not this backend node";
							return false;
						}
						// Occlusion is accepted since the backend grew its depth
						// pyramid and indirect draw path, but it composes behind
						// the gbuffer pass: that pass's early phase is what
						// seeds the pyramid the cull tests against, so a
						// document without it authored a cull nothing can feed.
						if (*culling == "occlusion") {
							bool depthWriter = false;
							for (uint32_t value = 1; value <= pipeline.Count() && !depthWriter; value++) {
								const graph::Node *writer = pipeline.Find(graph::NodeId{value});
								depthWriter = writer != nullptr && writer->Enabled &&
											  writer->Kind == core::Name("gbuffer");
							}
							if (!depthWriter) {
								offender = node->Name;
								reason = "occlusion culling needs the gbuffer pass to seed its "
										 "depth pyramid";
								return false;
							}
						}
					}
				}
			}
			return true;
		}
	}

	SDL_GPUShader *Renderer::Impl::LoadShader(
		std::string_view name,
		SDL_GPUShaderStage stage,
		uint32_t samplers,
		uint32_t uniformBuffers,
		uint32_t storageBuffers
	) const {
		// Staged under the owning module's name, so that two modules cannot
		// collide on a common file name like fullscreen.vert. The built-in GLSL
		// is `Engine::resources`, which is what that name is.
		//
		// The form comes from the device: the build stages a `.spv` and a `.msl`
		// for every shader, and which of them SDL will accept is what
		// `SDL_GetGPUShaderFormats` answered.
		const auto path = resources::Shader(name, Binary.Form);

		const auto code = ReadFile(path);
		if (code.empty()) {
			ENGINE_ERROR("shader not found or empty: {}", path.string());
			return nullptr;
		}

		SDL_GPUShaderCreateInfo info{};
		info.code = code.data();
		info.code_size = code.size();
		info.entrypoint = Binary.EntryPoint;
		info.format = Binary.Format;
		info.stage = stage;
		info.num_samplers = samplers;
		info.num_uniform_buffers = uniformBuffers;
		info.num_storage_buffers = storageBuffers;

		SDL_GPUShader *shader = SDL_CreateGPUShader(Device, &info);
		if (!shader) {
			ENGINE_ERROR("SDL_CreateGPUShader failed for {}: {}", name, SDL_GetError());
		}
		return shader;
	}

	SDL_GPUComputePipeline *Renderer::Impl::LoadComputePipeline(
		std::string_view name,
		uint32_t samplers,
		uint32_t readStorageBuffers,
		uint32_t writeStorageTextures,
		uint32_t writeStorageBuffers,
		uint32_t threadsX,
		uint32_t threadsY
	) const {
		const auto path = resources::Shader(name, Binary.Form);
		const auto code = ReadFile(path);
		if (code.empty()) {
			ENGINE_ERROR("shader not found or empty: {}", path.string());
			return nullptr;
		}

		SDL_GPUComputePipelineCreateInfo info{};
		info.code = code.data();
		info.code_size = code.size();
		info.entrypoint = Binary.EntryPoint;
		info.format = Binary.Format;
		info.num_samplers = samplers;
		info.num_readonly_storage_buffers = readStorageBuffers;
		info.num_readwrite_storage_textures = writeStorageTextures;
		info.num_readwrite_storage_buffers = writeStorageBuffers;
		info.num_uniform_buffers = 1;
		info.threadcount_x = threadsX;
		info.threadcount_y = threadsY;
		info.threadcount_z = 1;

		SDL_GPUComputePipeline *pipeline = SDL_CreateGPUComputePipeline(Device, &info);
		if (!pipeline) {
			ENGINE_ERROR("SDL_CreateGPUComputePipeline failed for {}: {}", name, SDL_GetError());
		}
		return pipeline;
	}

	bool Renderer::Impl::CreatePipelines() {
		SDL_GPUShader *opaqueVertex = LoadShader("opaque.vert", SDL_GPU_SHADERSTAGE_VERTEX, 0, 1, 2);

		// **Two samplers now: the shadow map and the surface.** The count is
		// part of the shader object rather than of the pipeline, so a mismatch
		// with the `layout(set = 2, binding = n)` declarations is a bind that
		// silently reads nothing rather than a validation error.
		SDL_GPUShader *opaqueFragment = LoadShader(
			// **Two uniform buffers now, not one.** The count is part of the
			// shader object rather than of the pipeline, so a mismatch with the
			// `layout(set = 3, binding = n)` declarations is a push that lands
			// nowhere rather than a validation error - the same trap the sampler
			// count above records.
			"opaque.frag",
			SDL_GPU_SHADERSTAGE_FRAGMENT,
			10,
			3
		);

		SDL_GPUShader *shadowVertex = LoadShader("shadow.vert", SDL_GPU_SHADERSTAGE_VERTEX, 0, 1, 2);
		SDL_GPUShader *shadowFragment = LoadShader("shadow.frag", SDL_GPU_SHADERSTAGE_FRAGMENT, 1, 1);
		SDL_GPUShader *overlayVertex = LoadShader("overlay.vert", SDL_GPU_SHADERSTAGE_VERTEX, 0, 0);
		SDL_GPUShader *imageFragment = LoadShader("image.frag", SDL_GPU_SHADERSTAGE_FRAGMENT, 1, 1);
		SDL_GPUShader *overlayFragment = LoadShader("overlay.frag", SDL_GPU_SHADERSTAGE_FRAGMENT, 1, 0);
		SDL_GPUShader *gbufferFragment = LoadShader("gbuffer.frag", SDL_GPU_SHADERSTAGE_FRAGMENT, 10, 1);
		SDL_GPUShader *depthLinearFragment =
			LoadShader("depth-linearise.frag", SDL_GPU_SHADERSTAGE_FRAGMENT, 1, 1);
		SDL_GPUShader *ssaoFragment = LoadShader("ssao.frag", SDL_GPU_SHADERSTAGE_FRAGMENT, 2, 1);
		// Nine samplers: the seven G-buffer and shadow inputs plus the two seam
		// light-field captures - `MAX_SEAM_LIGHTS`, bound last.
		SDL_GPUShader *deferredLightingFragment =
			LoadShader("deferred-lighting.frag", SDL_GPU_SHADERSTAGE_FRAGMENT, 9, 2);
		SDL_GPUShader *skyFragment = LoadShader("sky.frag", SDL_GPU_SHADERSTAGE_FRAGMENT, 3, 1);
		SDL_GPUShader *tonemapFragment = LoadShader("tonemap.frag", SDL_GPU_SHADERSTAGE_FRAGMENT, 1, 0);

		if (!opaqueVertex || !opaqueFragment || !shadowVertex || !shadowFragment || !overlayVertex ||
			!imageFragment || !overlayFragment || !gbufferFragment || !depthLinearFragment || !ssaoFragment ||
			!deferredLightingFragment || !skyFragment || !tonemapFragment) {
			return false;
		}

		const SDL_GPUTextureFormat swapchainFormat = ColourFormat();

		// --- opaque ---------------------------------------------------------

		const SDL_GPUVertexBufferDescription vertexBuffers[] = {
			{0, sizeof(Vertex), SDL_GPU_VERTEXINPUTRATE_VERTEX, 0},
		};

		// Only mesh vertices are attributes. Packed instance rows and the view's
		// ordered resident-slot indices arrive through vertex storage buffers, so
		// another camera can reorder the same rows without re-uploading them.
		const SDL_GPUVertexAttribute attributes[] = {
			{0, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3, offsetof(Vertex, Position)},
			{1, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3, offsetof(Vertex, Normal)},
			{2, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, offsetof(Vertex, TexCoord)},
		};

		SDL_GPUColorTargetDescription opaqueTarget{};
		opaqueTarget.format = swapchainFormat;

		SDL_GPUGraphicsPipelineCreateInfo opaque{};
		opaque.vertex_shader = opaqueVertex;
		opaque.fragment_shader = opaqueFragment;
		opaque.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
		opaque.vertex_input_state.vertex_buffer_descriptions = vertexBuffers;
		opaque.vertex_input_state.num_vertex_buffers = 1;
		opaque.vertex_input_state.vertex_attributes = attributes;
		opaque.vertex_input_state.num_vertex_attributes = 3;
		opaque.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
		opaque.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_BACK;
		// The cube winds counter-clockwise when seen from outside.
		opaque.rasterizer_state.front_face = SDL_GPU_FRONTFACE_COUNTER_CLOCKWISE;

		// **Clip against the near plane rather than clamping to it, which a
		// portal depends on and everything else quietly wanted.** SDL zeroes
		// this field, and false means *clamp*: a fragment in front of the near
		// plane is not discarded, it is pushed to depth zero and drawn. For an
		// ordinary camera that is a wall you have walked into filling the screen
		// instead of vanishing, which reads as a bug nobody files.
		//
		// For a portal it is the whole feature failing. The oblique clip works
		// by skewing the near plane onto the destination's pane, so *everything
		// the hole should not show is behind the near plane* - the wall the
		// destination is set into, its back face, and the room behind it.
		// Clamped, all of it draws at depth zero and fills the hole with the
		// wall it leads through. The matrix was right, the placement was right,
		// and the picture was a flat grey wall. See `scene::SurfaceLens`.
		opaque.rasterizer_state.enable_depth_clip = true;
		opaque.depth_stencil_state.enable_depth_test = true;
		opaque.depth_stencil_state.enable_depth_write = true;
		opaque.depth_stencil_state.compare_op = SDL_GPU_COMPAREOP_LESS;
		opaque.target_info.color_target_descriptions = &opaqueTarget;
		opaque.target_info.num_color_targets = 1;
		opaque.target_info.depth_stencil_format = DepthFormat;
		opaque.target_info.has_depth_stencil_target = true;

		OpaquePipeline = SDL_CreateGPUGraphicsPipeline(Device, &opaque);
		if (!OpaquePipeline) {
			ENGINE_ERROR("opaque pipeline: {}", SDL_GetError());
		}
		SDL_GPUColorTargetDescription forwardTarget{};
		forwardTarget.format = SDL_GPU_TEXTUREFORMAT_R10G10B10A2_UNORM;
		SDL_GPUGraphicsPipelineCreateInfo forward = opaque;
		forward.target_info.color_target_descriptions = &forwardTarget;
		ForwardPipeline = SDL_CreateGPUGraphicsPipeline(Device, &forward);
		if (ForwardPipeline == nullptr) {
			ENGINE_ERROR("forward pipeline: {}", SDL_GetError());
		}

		// --- wireframe view mode ----------------------------------------------
		//
		// **A second pipeline rather than a bindable state, because SDL_GPU has
		// no bindable state for it.** `rasterizer_state.fill_mode` is baked into
		// the pipeline object at creation, the same way `cull_mode` is for
		// `TransparentPipeline` above - there is no `SDL_SetGPU...FillMode` to
		// call between draws. So a toggle here means a second object with
		// everything else unchanged, exactly the shape `TransparentPipeline`
		// already is relative to `OpaquePipeline`.
		//
		// **Culling off, so a wireframe view shows every edge and not only the
		// ones facing the eye.** A wireframe box with back-face culling on
		// draws as three visible faces and reads as broken geometry; a
		// developer reaching for this view wants the far side of the mesh
		// too.
		//
		// **Bound in `BindPipeline`, not at each of the dozens of call sites
		// that ask for `OpaquePipeline` or `TransparentPipeline`.** Every one
		// of them already funnels through that one function to keep
		// `ActivePipeline` and `ActiveFamily` correct for `DrawSlots`'
		// restore - see its own header - so a family-keyed substitution there
		// reaches the screen pass, the surface pass and every mirror without
		// a second line anywhere else. A part with its own `ShaderScript`
		// keeps its own shader even so: `DrawSlots` binds a variant by name
		// over whatever `BindPipeline` left active, and a debug view is not
		// the place to override an author's own material.
		//
		// **Failure here is a diagnostic and a feature quietly unavailable,
		// never a reason `CreatePipelines` itself fails.** `fillModeNonSolid`
		// is an optional device feature on some backends; a machine without it
		// still renders every ordinary frame correctly and only loses a debug
		// toggle, which is not worth the whole renderer refusing to start
		// over.
		SDL_GPUGraphicsPipelineCreateInfo wireframeOpaque = opaque;
		wireframeOpaque.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_LINE;
		wireframeOpaque.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
		WireframeOpaquePipeline = SDL_CreateGPUGraphicsPipeline(Device, &wireframeOpaque);
		if (!WireframeOpaquePipeline) {
			ENGINE_WARN("wireframe opaque pipeline unavailable: {}", SDL_GetError());
		}

		// --- default PBR graph -----------------------------------------------
		const auto hasFormat = [this](graph::ResourceFormat format) {
			return std::find(Caps.Formats.begin(), Caps.Formats.end(), format) != Caps.Formats.end();
		};
		const bool pbrSupported = Caps.MaxColourTargets >= 4 && hasFormat(graph::ResourceFormat::RGBA16F) &&
								  hasFormat(graph::ResourceFormat::R32F);

		SDL_GPUColorTargetDescription gbufferTargets[4]{};
		gbufferTargets[0].format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM_SRGB;
		gbufferTargets[1].format = SDL_GPU_TEXTUREFORMAT_R10G10B10A2_UNORM;
		gbufferTargets[2].format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
		gbufferTargets[3].format = SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT;

		SDL_GPUGraphicsPipelineCreateInfo gbuffer = opaque;
		gbuffer.fragment_shader = gbufferFragment;
		gbuffer.target_info.color_target_descriptions = gbufferTargets;
		gbuffer.target_info.num_color_targets = 4;
		if (pbrSupported) {
			GBufferPipeline = SDL_CreateGPUGraphicsPipeline(Device, &gbuffer);
			if (GBufferPipeline == nullptr) {
				ENGINE_ERROR("gbuffer pipeline: {}", SDL_GetError());
			}
		}

		const auto fullscreen = [&](SDL_GPUShader *fragment, SDL_GPUTextureFormat format) {
			SDL_GPUColorTargetDescription target{};
			target.format = format;

			SDL_GPUGraphicsPipelineCreateInfo info{};
			info.vertex_shader = overlayVertex;
			info.fragment_shader = fragment;
			info.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
			info.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
			info.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
			info.target_info.color_target_descriptions = &target;
			info.target_info.num_color_targets = 1;
			return SDL_CreateGPUGraphicsPipeline(Device, &info);
		};

		if (pbrSupported) {
			DepthLinearPipeline = fullscreen(depthLinearFragment, SDL_GPU_TEXTUREFORMAT_R32_FLOAT);
			SsaoPipeline = fullscreen(ssaoFragment, SDL_GPU_TEXTUREFORMAT_R8_UNORM);
			DeferredLightingPipeline =
				fullscreen(deferredLightingFragment, SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT);
			SkyPipeline = fullscreen(skyFragment, SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT);
			TonemapPipeline = fullscreen(tonemapFragment, swapchainFormat);
			if (DepthLinearPipeline == nullptr || SsaoPipeline == nullptr ||
				DeferredLightingPipeline == nullptr || SkyPipeline == nullptr || TonemapPipeline == nullptr) {
				ENGINE_ERROR("default PBR fullscreen pipeline: {}", SDL_GetError());
			}
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
		//   goes. Leaving the write on is the classic version of this bug - the
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

		// **The grid, which is the transparent state with no vertex input.** It
		// blends like a transparent surface, tests depth like one and writes
		// none - the difference is that its geometry is a fullscreen triangle
		// and its depth comes from the fragment shader rather than from a
		// vertex. `grid.frag` carries why that is the whole feature.
		{
			SDL_GPUShader *gridFragment = LoadShader("grid.frag", SDL_GPU_SHADERSTAGE_FRAGMENT, 0, 1);
			if (gridFragment == nullptr) {
				ENGINE_WARN("ground grid unavailable: {}", SDL_GetError());
			} else {
				SDL_GPUGraphicsPipelineCreateInfo grid = transparent;
				grid.vertex_shader = overlayVertex;
				grid.fragment_shader = gridFragment;
				grid.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;

				// No vertex buffer: the triangle is three indices, exactly as
				// every other fullscreen pass in this file draws it.
				grid.vertex_input_state = SDL_GPUVertexInputState{};

				GridPipeline = SDL_CreateGPUGraphicsPipeline(Device, &grid);
				if (GridPipeline == nullptr) {
					ENGINE_WARN("ground grid pipeline unavailable: {}", SDL_GetError());
				}
				SDL_ReleaseGPUShader(Device, gridFragment);
			}
		}

		// The wireframe twin, for `WireframeOpaquePipeline`'s own reason.
		// Culling is already off on `transparent`, so only the fill mode
		// changes.
		SDL_GPUGraphicsPipelineCreateInfo wireframeTransparent = transparent;
		wireframeTransparent.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_LINE;
		WireframeTransparentPipeline = SDL_CreateGPUGraphicsPipeline(Device, &wireframeTransparent);
		if (!WireframeTransparentPipeline) {
			ENGINE_WARN("wireframe transparent pipeline unavailable: {}", SDL_GetError());
		}

		// --- what a variant is derived from ---------------------------------
		//
		// The two descriptors above, copied whole with their arrays, so a shader
		// arriving later builds a pipeline that differs from these in exactly
		// one field. Copied rather than rebuilt: a second description of the
		// vertex layout is a second thing to keep in step, and the one that
		// would be missed is this one - it is only read when somebody selects a
		// custom shader.
		std::memcpy(VariantBuffers, vertexBuffers, sizeof(VariantBuffers));
		std::memcpy(VariantAttributes, attributes, sizeof(VariantAttributes));
		VariantOpaqueTarget = opaqueTarget;
		VariantBlendedTarget = blendedTarget;

		VariantOpaqueInfo = opaque;
		VariantOpaqueInfo.vertex_input_state.vertex_buffer_descriptions = VariantBuffers;
		VariantOpaqueInfo.vertex_input_state.vertex_attributes = VariantAttributes;
		VariantOpaqueInfo.target_info.color_target_descriptions = &VariantOpaqueTarget;

		VariantBlendedInfo = transparent;
		VariantBlendedInfo.vertex_input_state.vertex_buffer_descriptions = VariantBuffers;
		VariantBlendedInfo.vertex_input_state.vertex_attributes = VariantAttributes;
		VariantBlendedInfo.target_info.color_target_descriptions = &VariantBlendedTarget;

		// The vertex stage is kept rather than released with the others below,
		// because a variant built after this function has run needs it.
		OpaqueVertexShader = opaqueVertex;
		VariantsReady = true;

		// --- particles ------------------------------------------------------
		//
		// **No vertex buffer for the quad and no index buffer at all.** The
		// billboard's four corners come out of `gl_VertexIndex` in the shader, and
		// the primitive is a triangle strip - so one particle is one instance of
		// four vertices with nothing fetched. At half a million particles that is
		// half a million cache lines never read.
		//
		// Slot 0 is the per-instance particle, which is why the descriptions below
		// name one buffer where the opaque pipeline names two.

		SDL_GPUShader *particleVertex = LoadShader("particle.vert", SDL_GPU_SHADERSTAGE_VERTEX, 0, 1);
		SDL_GPUShader *particleFragment = LoadShader("particle.frag", SDL_GPU_SHADERSTAGE_FRAGMENT, 1, 1);

		if (particleVertex != nullptr && particleFragment != nullptr) {
			const SDL_GPUVertexBufferDescription particleBuffers[] = {
				{0, sizeof(effects::ParticleInstance), SDL_GPU_VERTEXINPUTRATE_INSTANCE, 0},
			};

			// **Three of the four are unsigned integers rather than floats**,
			// because that is what they are: a packed size, a packed rotation and
			// cell, and an RGBA8 colour. Declaring them as floats would make the
			// driver convert bits that are not a float, which is not a slow path -
			// it is a different number.
			const SDL_GPUVertexAttribute particleAttributes[] = {
				{0, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3, offsetof(effects::ParticleInstance, Position)},
				{1, 0, SDL_GPU_VERTEXELEMENTFORMAT_UINT, offsetof(effects::ParticleInstance, Size)},
				{2,
				 0,
				 SDL_GPU_VERTEXELEMENTFORMAT_UINT,
				 offsetof(effects::ParticleInstance, RotationAndCell)},
				{3, 0, SDL_GPU_VERTEXELEMENTFORMAT_UINT, offsetof(effects::ParticleInstance, Colour)},
				{4, 0, SDL_GPU_VERTEXELEMENTFORMAT_UINT, offsetof(effects::ParticleInstance, Slot)},
			};

			SDL_GPUColorTargetDescription particleTarget{};
			particleTarget.format = swapchainFormat;
			particleTarget.blend_state.enable_blend = true;
			particleTarget.blend_state.color_blend_op = SDL_GPU_BLENDOP_ADD;
			particleTarget.blend_state.alpha_blend_op = SDL_GPU_BLENDOP_ADD;
			particleTarget.blend_state.src_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
			particleTarget.blend_state.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
			particleTarget.blend_state.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
			particleTarget.blend_state.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;

			SDL_GPUGraphicsPipelineCreateInfo particle{};
			particle.vertex_shader = particleVertex;
			particle.fragment_shader = particleFragment;
			particle.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLESTRIP;
			particle.vertex_input_state.vertex_buffer_descriptions = particleBuffers;
			particle.vertex_input_state.num_vertex_buffers = 1;
			particle.vertex_input_state.vertex_attributes = particleAttributes;
			particle.vertex_input_state.num_vertex_attributes = 5;
			particle.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;

			// **Nothing is culled.** A billboard is a flat quad turned to face the
			// eye, so which way it winds depends on where the camera is - and a
			// cull mode would make half the particles in a scene vanish depending
			// on which side of the emitter you stood.
			particle.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;

			// **Depth tested and not depth written**, which is the same pair the
			// transparent pipeline has and for the same reason: a particle must be
			// hidden by the wall in front of it, and must not hide the particle
			// behind it.
			particle.depth_stencil_state.enable_depth_test = true;
			particle.depth_stencil_state.enable_depth_write = false;
			particle.depth_stencil_state.compare_op = SDL_GPU_COMPAREOP_LESS;
			particle.target_info.color_target_descriptions = &particleTarget;
			particle.target_info.num_color_targets = 1;
			particle.target_info.depth_stencil_format = DepthFormat;
			particle.target_info.has_depth_stencil_target = true;

			ParticlePipeline = SDL_CreateGPUGraphicsPipeline(Device, &particle);
			if (ParticlePipeline == nullptr) {
				ENGINE_ERROR("particle pipeline: {}", SDL_GetError());
			}

			// The additive twin. **One destination factor apart**, and that one
			// factor is what makes the blend commutative and therefore
			// order-independent - which is why an additive emitter needs no sort.
			SDL_GPUColorTargetDescription additiveTarget = particleTarget;
			additiveTarget.blend_state.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
			additiveTarget.blend_state.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;

			SDL_GPUGraphicsPipelineCreateInfo additive = particle;
			additive.target_info.color_target_descriptions = &additiveTarget;

			AdditiveParticlePipeline = SDL_CreateGPUGraphicsPipeline(Device, &additive);
			if (AdditiveParticlePipeline == nullptr) {
				ENGINE_ERROR("additive particle pipeline: {}", SDL_GetError());
			}

			SDL_ReleaseGPUShader(Device, particleVertex);
			SDL_ReleaseGPUShader(Device, particleFragment);
		}

		// --- ribbons --------------------------------------------------------
		//
		// A triangle strip over a real vertex buffer, where the particle pass has
		// no buffer at all. `ribbon.vert` says why the geometry is built on the
		// CPU rather than expanded here.

		SDL_GPUShader *ribbonVertex = LoadShader("ribbon.vert", SDL_GPU_SHADERSTAGE_VERTEX, 0, 1);
		SDL_GPUShader *ribbonFragment = LoadShader("ribbon.frag", SDL_GPU_SHADERSTAGE_FRAGMENT, 1, 1);

		if (ribbonVertex != nullptr && ribbonFragment != nullptr) {
			const SDL_GPUVertexBufferDescription ribbonBuffers[] = {
				{0, sizeof(effects::RibbonVertex), SDL_GPU_VERTEXINPUTRATE_VERTEX, 0},
			};

			const SDL_GPUVertexAttribute ribbonAttributes[] = {
				{0, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3, offsetof(effects::RibbonVertex, Position)},
				{1, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, offsetof(effects::RibbonVertex, Coordinate)},
				{2, 0, SDL_GPU_VERTEXELEMENTFORMAT_UINT, offsetof(effects::RibbonVertex, Colour)},
			};

			SDL_GPUColorTargetDescription ribbonTarget{};
			ribbonTarget.format = swapchainFormat;
			ribbonTarget.blend_state.enable_blend = true;
			ribbonTarget.blend_state.color_blend_op = SDL_GPU_BLENDOP_ADD;
			ribbonTarget.blend_state.alpha_blend_op = SDL_GPU_BLENDOP_ADD;
			ribbonTarget.blend_state.src_color_blendfactor = SDL_GPU_BLENDFACTOR_SRC_ALPHA;
			ribbonTarget.blend_state.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
			ribbonTarget.blend_state.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
			ribbonTarget.blend_state.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;

			SDL_GPUGraphicsPipelineCreateInfo ribbon{};
			ribbon.vertex_shader = ribbonVertex;
			ribbon.fragment_shader = ribbonFragment;
			ribbon.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLESTRIP;
			ribbon.vertex_input_state.vertex_buffer_descriptions = ribbonBuffers;
			ribbon.vertex_input_state.num_vertex_buffers = 1;
			ribbon.vertex_input_state.vertex_attributes = ribbonAttributes;
			ribbon.vertex_input_state.num_vertex_attributes = 3;
			ribbon.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;

			// **Nothing is culled**, for the billboard's reason: a camera-facing
			// strip's winding depends on where the camera is, so a cull mode
			// would make a beam vanish from one side.
			ribbon.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
			ribbon.depth_stencil_state.enable_depth_test = true;
			ribbon.depth_stencil_state.enable_depth_write = false;
			ribbon.depth_stencil_state.compare_op = SDL_GPU_COMPAREOP_LESS;
			ribbon.target_info.color_target_descriptions = &ribbonTarget;
			ribbon.target_info.num_color_targets = 1;
			ribbon.target_info.depth_stencil_format = DepthFormat;
			ribbon.target_info.has_depth_stencil_target = true;

			RibbonPipeline = SDL_CreateGPUGraphicsPipeline(Device, &ribbon);
			if (RibbonPipeline == nullptr) {
				ENGINE_ERROR("ribbon pipeline: {}", SDL_GetError());
			}

			SDL_GPUColorTargetDescription additiveRibbon = ribbonTarget;
			additiveRibbon.blend_state.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
			additiveRibbon.blend_state.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;

			SDL_GPUGraphicsPipelineCreateInfo additive = ribbon;
			additive.target_info.color_target_descriptions = &additiveRibbon;

			AdditiveRibbonPipeline = SDL_CreateGPUGraphicsPipeline(Device, &additive);
			if (AdditiveRibbonPipeline == nullptr) {
				ENGINE_ERROR("additive ribbon pipeline: {}", SDL_GetError());
			}

			SDL_ReleaseGPUShader(Device, ribbonVertex);
			SDL_ReleaseGPUShader(Device, ribbonFragment);
		}

		// --- image and overlay ----------------------------------------------

		SDL_GPUColorTargetDescription imageTarget{};
		imageTarget.format = swapchainFormat;

		SDL_GPUGraphicsPipelineCreateInfo image{};
		image.vertex_shader = overlayVertex;
		image.fragment_shader = imageFragment;
		image.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
		image.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
		image.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
		image.target_info.color_target_descriptions = &imageTarget;
		image.target_info.num_color_targets = 1;

		ImagePipeline = SDL_CreateGPUGraphicsPipeline(Device, &image);
		if (!ImagePipeline) {
			ENGINE_ERROR("image pipeline: {}", SDL_GetError());
		}

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
		// outlive their creation. **`opaqueVertex` is the exception and is
		// released in `Shutdown` instead** - see `OpaqueVertexShader`.
		SDL_ReleaseGPUShader(Device, opaqueFragment);
		SDL_ReleaseGPUShader(Device, gbufferFragment);
		SDL_ReleaseGPUShader(Device, depthLinearFragment);
		SDL_ReleaseGPUShader(Device, ssaoFragment);
		SDL_ReleaseGPUShader(Device, deferredLightingFragment);
		SDL_ReleaseGPUShader(Device, skyFragment);
		SDL_ReleaseGPUShader(Device, tonemapFragment);
		SDL_ReleaseGPUShader(Device, overlayVertex);
		SDL_ReleaseGPUShader(Device, imageFragment);
		SDL_ReleaseGPUShader(Device, overlayFragment);

		SDL_ReleaseGPUShader(Device, shadowVertex);
		SDL_ReleaseGPUShader(Device, shadowFragment);

		// The occlusion culling compute set. Not in the conjunction below for
		// the particle pipelines' reason: a build these failed on still draws a
		// world, and the validation path refuses `culling = "occlusion"`
		// documents with the failure named rather than the client dying here.
		if (Caps.HasCompute) {
			Occlusion.Seed = LoadComputePipeline("hzb-seed.comp", 1, 0, 1, 0, 8, 8);
			Occlusion.Reduce = LoadComputePipeline("hzb-reduce.comp", 1, 0, 1, 0, 8, 8);
			Occlusion.Cull = LoadComputePipeline("occlusion-cull.comp", PYRAMID_LEVEL_LIMIT, 2, 0, 2, 64, 1);
			Occlusion.Args = LoadComputePipeline("occlusion-args.comp", 0, 2, 0, 1, 64, 1);

			// The particle simulation. Emission reads the resident work and parameter
			// tables, then writes newborn state and per-emitter counters. Integration
			// reads those plus curves and portal panes, then writes the state pool and
			// instance stream. The scatter updates changed parameter or curve rows.
			ParticleStep = LoadComputePipeline("particle-step.comp", 0, 4, 0, 2, 64, 1);
			ParticleEmit = LoadComputePipeline("particle-emission.comp", 0, 2, 0, 2, 64, 1);
			ParticleScatter = LoadComputePipeline("particle-scatter.comp", 0, 1, 0, 1, 64, 1);
			EnvironmentCompute = LoadComputePipeline("environment.comp", 6, 0, 1, 0, 8, 8);
		}

		// **The particle pipelines are deliberately not in this conjunction.** A
		// build whose particle shaders failed to compile still draws a world, and
		// failing initialisation over an effect would take the whole client down
		// for something a scene may not even use. `DrawParticles` checks for null
		// and draws nothing, with the error already in the log above.
		return OpaquePipeline != nullptr && ForwardPipeline != nullptr && TransparentPipeline != nullptr &&
			   ShadowPipeline != nullptr && ImagePipeline != nullptr && OverlayPipeline != nullptr &&
			   (!pbrSupported || (GBufferPipeline != nullptr && DepthLinearPipeline != nullptr &&
								  SsaoPipeline != nullptr && DeferredLightingPipeline != nullptr &&
								  SkyPipeline != nullptr && TonemapPipeline != nullptr)) &&
			   (!Caps.HasCompute || EnvironmentCompute != nullptr);
	}

	void Renderer::Impl::BindPipeline(
		SDL_GPURenderPass *pass, SDL_GPUGraphicsPipeline *pipeline, PipelineFamily family
	) {
		// **The one substitution point for the whole renderer.** Every caller
		// still names `OpaquePipeline` or `TransparentPipeline`, exactly as
		// before wireframe existed - this is where that request becomes a
		// line-drawing one, so the screen pass, the surface pass and every
		// mirror get it with nothing edited at their own call sites. See
		// `WireframeOpaquePipeline`'s own header for the rest of the argument.
		if (WireframeMode) {
			if (family == PipelineFamily::Opaque && WireframeOpaquePipeline != nullptr) {
				pipeline = WireframeOpaquePipeline;
			} else if (family == PipelineFamily::Transparent && WireframeTransparentPipeline != nullptr) {
				pipeline = WireframeTransparentPipeline;
			}
		}

		SDL_BindGPUGraphicsPipeline(pass, pipeline);
		ActivePipeline = pipeline;
		ActiveFamily = family;
	}

	bool Renderer::Impl::AddShaderVariant(const core::Name &name, std::span<const uint32_t> spirv) {
		if (!name.IsValid() || spirv.empty() || !VariantsReady || OpaqueVertexShader == nullptr) {
			return false;
		}

		// **The same counts `opaque.frag` declares, and they are the interface.**
		// A shader object carries its sampler and uniform-buffer counts rather
		// than the pipeline doing so, so a module declaring different ones binds
		// and silently reads nothing - the trap `CreatePipelines` already records
		// for the built-in fragment shader. That is why a `ShaderScript` is a
		// fragment shader written against `opaque.frag`'s slots and not against
		// a blank page.
		// **The runtime half of the translation, and it is the half a build step
		// cannot do.** A `ShaderScript` does not exist when the shaders are
		// compiled, so a device that takes MSL gets nothing at all unless the
		// engine can translate one while it runs. Same function as
		// `mono.tools/shadercross` calls, so a script and a built-in land on the
		// same Metal indices.
		//
		// A failure is a log line and a refused variant rather than a fatal, for
		// the reason `ShaderLibrary` gives about a compile failure: the part
		// keeps the engine's own shader and the world keeps running.
		const bool toMsl = Binary.Form == resources::ShaderForm::Msl;

		std::string translated;
		if (toMsl) {
			msl::Translation result = msl::Translate(spirv);
			if (result.Failed) {
				ENGINE_ERROR("shader '{}' cannot be translated to MSL: {}", name.Text(), result.Error);
				return false;
			}
			translated = std::move(result.Source);
		}

		SDL_GPUShaderCreateInfo info{};
		// Metal's backend reads the source with `initWithBytes:length:`, so the
		// length is the text and not the text plus its terminator.
		info.code = toMsl ? reinterpret_cast<const Uint8 *>(translated.data())
						  : reinterpret_cast<const Uint8 *>(spirv.data());
		info.code_size = toMsl ? translated.size() : spirv.size() * sizeof(uint32_t);
		info.entrypoint = Binary.EntryPoint;
		info.format = Binary.Format;
		info.stage = SDL_GPU_SHADERSTAGE_FRAGMENT;
		info.num_samplers = 10;
		info.num_uniform_buffers = 3;

		SDL_GPUShader *fragment = SDL_CreateGPUShader(Device, &info);
		if (fragment == nullptr) {
			ENGINE_ERROR("shader '{}': {}", name.Text(), SDL_GetError());
			return false;
		}

		SDL_GPUGraphicsPipelineCreateInfo opaque = VariantOpaqueInfo;
		opaque.vertex_shader = OpaqueVertexShader;
		opaque.fragment_shader = fragment;

		SDL_GPUGraphicsPipelineCreateInfo blended = VariantBlendedInfo;
		blended.vertex_shader = OpaqueVertexShader;
		blended.fragment_shader = fragment;

		ShaderVariant variant;
		variant.Fragment = fragment;
		variant.Opaque = SDL_CreateGPUGraphicsPipeline(Device, &opaque);
		variant.Transparent = SDL_CreateGPUGraphicsPipeline(Device, &blended);

		// **Both or neither.** A variant with one pipeline would draw a wall
		// toon-shaded and the pane in front of it with the engine's shader,
		// which reads as the material not applying to glass rather than as a
		// pipeline that failed to build.
		if (variant.Opaque == nullptr || variant.Transparent == nullptr) {
			ENGINE_ERROR("shader '{}' pipeline: {}", name.Text(), SDL_GetError());
			if (variant.Opaque != nullptr) {
				SDL_ReleaseGPUGraphicsPipeline(Device, variant.Opaque);
			}
			if (variant.Transparent != nullptr) {
				SDL_ReleaseGPUGraphicsPipeline(Device, variant.Transparent);
			}
			SDL_ReleaseGPUShader(Device, fragment);
			return false;
		}

		// Replacing is the ordinary case: an author editing a `ShaderScript`
		// bumps its revision every keystroke that lands, and the library hands
		// the new words straight back here.
		DropShaderVariant(name);
		ShaderVariants[name.Id()] = variant;
		return true;
	}

	void Renderer::Impl::DropShaderVariant(const core::Name &name) {
		const auto found = ShaderVariants.find(name.Id());
		if (found == ShaderVariants.end()) {
			return;
		}

		// **Ordered after the last frame that could have bound it**, which is
		// what `WaitForFrame` in `Renderer::DropShader` is for: releasing a
		// pipeline a command buffer in flight still references is a use after
		// free inside the driver.
		SDL_ReleaseGPUGraphicsPipeline(Device, found->second.Opaque);
		SDL_ReleaseGPUGraphicsPipeline(Device, found->second.Transparent);
		SDL_ReleaseGPUShader(Device, found->second.Fragment);
		ShaderVariants.erase(found);
	}

	void Renderer::Impl::ReleaseShaderVariants() {
		for (auto &entry : ShaderVariants) {
			SDL_ReleaseGPUGraphicsPipeline(Device, entry.second.Opaque);
			SDL_ReleaseGPUGraphicsPipeline(Device, entry.second.Transparent);
			SDL_ReleaseGPUShader(Device, entry.second.Fragment);
		}
		ShaderVariants.clear();

		if (OpaqueVertexShader != nullptr) {
			SDL_ReleaseGPUShader(Device, OpaqueVertexShader);
			OpaqueVertexShader = nullptr;
		}
		VariantsReady = false;
	}

	SDL_GPUGraphicsPipeline *Renderer::Impl::VariantFor(const core::Name &shader) const {
		if (!shader.IsValid() || ShaderVariants.empty()) {
			return nullptr;
		}

		// **An unknown name draws with the engine's shader rather than not at
		// all**, which is `MeshTable::Resolve`'s rule: a shader that has not
		// arrived is the ordinary state of a world still loading, and a part
		// that vanished until it did would be a worse symptom than one drawn
		// plainly. The name being wrong rather than late is reported by
		// `ShaderLibrary`, where the world can still be asked about it.
		const auto found = ShaderVariants.find(shader.Id());
		if (found == ShaderVariants.end()) {
			return nullptr;
		}

		switch (ActiveFamily) {
		case PipelineFamily::Opaque:
			return found->second.Opaque;
		case PipelineFamily::Transparent:
			return found->second.Transparent;
		case PipelineFamily::Other:
			break;
		}
		return nullptr;
	}

	bool Renderer::Impl::GraphShaderCode(
		const graph::Node &node, ShaderStage stage, std::vector<uint8_t> &bytes, std::string &entryPoint
	) const {
		bytes.clear();
		entryPoint = Binary.EntryPoint;
		const std::string *source = node.Parameter(core::Name("source"));
		if (source == nullptr || source->empty()) {
			const std::string *shader = node.Parameter(core::Name("shader"));
			if (shader == nullptr || shader->empty()) {
				ENGINE_WARN("'{}' has no shader or GLSL source, so it records no work", node.Name.Text());
				return false;
			}
			bytes = ReadFile(resources::Shader(*shader, Binary.Form));
			if (bytes.empty()) {
				ENGINE_WARN("'{}' names shader '{}' but no staged module exists", node.Name.Text(), *shader);
				return false;
			}
			return true;
		}

		ShaderCompiler compiler;
		const ShaderCompilation compiled = compiler.Compile(*source, stage, node.Name.Text());
		if (compiled.Failed) {
			ENGINE_WARN("shader for '{}': {}", node.Name.Text(), compiled.Error);
			return false;
		}

		if (Binary.Form == resources::ShaderForm::Msl) {
			msl::Translation translated = msl::Translate(compiled.SpirV);
			if (translated.Failed) {
				ENGINE_WARN(
					"shader for '{}' cannot be translated to MSL: {}", node.Name.Text(), translated.Error
				);
				return false;
			}
			bytes.assign(translated.Source.begin(), translated.Source.end());
			return true;
		}

		const auto *first = reinterpret_cast<const uint8_t *>(compiled.SpirV.data());
		bytes.assign(first, first + compiled.SpirV.size() * sizeof(uint32_t));
		return true;
	}

	SDL_GPUGraphicsPipeline *Renderer::Impl::GraphRasterFor(
		const NamedPipeline &pipeline, const graph::Node &node, SDL_GPUTextureFormat format, uint32_t samplers
	) {
		for (const GraphRasterPipeline &entry : GraphRasterPipelines) {
			if (entry.Pipeline == pipeline.Name && entry.Node == node.Name && entry.Format == format &&
				entry.Samplers == samplers) {
				return entry.Handle;
			}
		}

		// **Past the cache scan, so this reads zero on every frame but one.** A
		// miss compiles GLSL through shaderc and builds a device pipeline, in
		// the middle of the frame that first drew the node - tens to hundreds of
		// milliseconds, with nothing naming it. A hitch the first time a graph
		// node is used is not a mystery once it has a bar.
		ENGINE_PROFILE_CAT("compile graph pipeline", core::ProfileCategory::Render);

		SDL_GPUShader *vertex = LoadShader("overlay.vert", SDL_GPU_SHADERSTAGE_VERTEX, 0, 0);
		std::vector<uint8_t> bytes;
		std::string entryPoint;
		SDL_GPUShader *fragment = nullptr;
		if (GraphShaderCode(node, ShaderStage::Fragment, bytes, entryPoint)) {
			SDL_GPUShaderCreateInfo shader{};
			shader.code = bytes.data();
			shader.code_size = bytes.size();
			shader.entrypoint = entryPoint.c_str();
			shader.format = Binary.Format;
			shader.stage = SDL_GPU_SHADERSTAGE_FRAGMENT;
			shader.num_samplers = samplers;
			shader.num_uniform_buffers = 1;
			fragment = SDL_CreateGPUShader(Device, &shader);
			if (fragment == nullptr) {
				ENGINE_WARN("fragment shader for '{}': {}", node.Name.Text(), SDL_GetError());
			}
		}

		SDL_GPUGraphicsPipeline *built = nullptr;
		if (vertex != nullptr && fragment != nullptr) {
			SDL_GPUColorTargetDescription target{};
			target.format = format;
			SDL_GPUGraphicsPipelineCreateInfo info{};
			info.vertex_shader = vertex;
			info.fragment_shader = fragment;
			info.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
			info.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
			info.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
			info.target_info.color_target_descriptions = &target;
			info.target_info.num_color_targets = 1;
			built = SDL_CreateGPUGraphicsPipeline(Device, &info);
			if (built == nullptr) {
				ENGINE_WARN("raster pipeline for '{}': {}", node.Name.Text(), SDL_GetError());
			}
		}
		if (vertex != nullptr) {
			SDL_ReleaseGPUShader(Device, vertex);
		}
		if (fragment != nullptr) {
			SDL_ReleaseGPUShader(Device, fragment);
		}
		GraphRasterPipelines.push_back({pipeline.Name, node.Name, format, samplers, built});
		return built;
	}

	SDL_GPUComputePipeline *Renderer::Impl::GraphComputeFor(
		const NamedPipeline &pipeline,
		const graph::Node &node,
		uint32_t samplers,
		uint32_t storage,
		uint32_t localX,
		uint32_t localY,
		uint32_t localZ
	) {
		for (const GraphComputePipeline &entry : GraphComputePipelines) {
			if (entry.Pipeline == pipeline.Name && entry.Node == node.Name && entry.Samplers == samplers &&
				entry.Storage == storage && entry.LocalX == localX && entry.LocalY == localY &&
				entry.LocalZ == localZ) {
				return entry.Handle;
			}
		}

		// Past the cache scan, exactly as `GraphRasterFor` above. See its note.
		ENGINE_PROFILE_CAT("compile graph pipeline", core::ProfileCategory::Render);

		std::vector<uint8_t> bytes;
		std::string entryPoint;
		SDL_GPUComputePipeline *built = nullptr;
		if (GraphShaderCode(node, ShaderStage::Compute, bytes, entryPoint)) {
			SDL_GPUComputePipelineCreateInfo info{};
			info.code = bytes.data();
			info.code_size = bytes.size();
			info.entrypoint = entryPoint.c_str();
			info.format = Binary.Format;
			info.num_samplers = samplers;
			info.num_readwrite_storage_textures = storage;
			info.threadcount_x = localX;
			info.threadcount_y = localY;
			info.threadcount_z = localZ;
			built = SDL_CreateGPUComputePipeline(Device, &info);
			if (built == nullptr) {
				ENGINE_WARN("compute pipeline for '{}': {}", node.Name.Text(), SDL_GetError());
			}
		}
		GraphComputePipelines.push_back(
			{pipeline.Name, node.Name, samplers, storage, localX, localY, localZ, built}
		);
		return built;
	}

	void Renderer::Impl::ReleaseGraphState(core::Name pipeline) {
		std::erase_if(GraphCaptureReceipts, [pipeline](const GraphCaptureReceipt &receipt) {
			return receipt.Pipeline == pipeline;
		});
		for (size_t index = GraphTargets.size(); index > 0; index--) {
			GraphTarget &target = GraphTargets[index - 1];
			if (target.Pipeline != pipeline) {
				continue;
			}
			if (target.Texture != nullptr && Device != nullptr) {
				gpu::ReleaseTexture(Device, target.Texture);
			}
			GraphTargets.erase(GraphTargets.begin() + static_cast<ptrdiff_t>(index - 1));
		}
		for (size_t index = GraphRasterPipelines.size(); index > 0; index--) {
			GraphRasterPipeline &entry = GraphRasterPipelines[index - 1];
			if (entry.Pipeline != pipeline) {
				continue;
			}
			if (entry.Handle != nullptr && Device != nullptr) {
				SDL_ReleaseGPUGraphicsPipeline(Device, entry.Handle);
			}
			GraphRasterPipelines.erase(GraphRasterPipelines.begin() + static_cast<ptrdiff_t>(index - 1));
		}
		for (size_t index = GraphComputePipelines.size(); index > 0; index--) {
			GraphComputePipeline &entry = GraphComputePipelines[index - 1];
			if (entry.Pipeline != pipeline) {
				continue;
			}
			if (entry.Handle != nullptr && Device != nullptr) {
				SDL_ReleaseGPUComputePipeline(Device, entry.Handle);
			}
			GraphComputePipelines.erase(GraphComputePipelines.begin() + static_cast<ptrdiff_t>(index - 1));
		}
	}

	void Renderer::Impl::ReleaseAllGraphState() {
		while (!GraphTargets.empty()) {
			ReleaseGraphState(GraphTargets.back().Pipeline);
		}
		while (!GraphRasterPipelines.empty()) {
			ReleaseGraphState(GraphRasterPipelines.back().Pipeline);
		}
		while (!GraphComputePipelines.empty()) {
			ReleaseGraphState(GraphComputePipelines.back().Pipeline);
		}
		GraphCaptureReceipts.clear();
		GraphCaptureFrames.clear();
	}

	bool Renderer::SetPipeline(core::Name name, const graph::RenderGraph &pipeline) {
		RequireOwningThread("SetPipeline");
		if (!name.IsValid()) {
			ENGINE_ERROR("a render pipeline needs a name a view can select");
			return false;
		}

		graph::CompiledGraph compiled;
		graph::ExecutionSchedule schedule;
		core::Name offender;
		std::string reason;
		std::vector<core::Name> customKinds;
		customKinds.reserve(CustomNodeHandlers.size());
		for (const InstalledNodeHandler &installed : CustomNodeHandlers) {
			customKinds.push_back(installed.Kind);
		}
		const DeviceCaps *caps = State->Device != nullptr ? &State->Caps : nullptr;
		if (!CompileRenderPipeline(pipeline, compiled, schedule, offender, reason, caps, customKinds)) {
			ENGINE_ERROR("pipeline '{}' refused: {} at '{}'", name.Text(), reason, offender.Text());
			return false;
		}

		for (Impl::NamedPipeline &installed : State->NamedPipelines) {
			if (installed.Name != name) {
				continue;
			}
			if (State->Device != nullptr) {
				(void)WaitForFrame();
			}
			State->ReleaseGraphState(name);
			installed.Graph = pipeline;
			installed.Compiled = std::move(compiled);
			installed.Buffers = graph::PlanCommandBuffers(schedule);
			installed.Schedule = std::move(schedule);
			return true;
		}

		std::vector<graph::PlannedCommandBuffer> buffers = graph::PlanCommandBuffers(schedule);
		State->NamedPipelines.push_back(
			Impl::NamedPipeline{name, pipeline, std::move(compiled), std::move(schedule), std::move(buffers)}
		);
		return true;
	}

	bool Renderer::InstallNodeHandler(core::Name kind, NodeHandler handler, NodeHandlerLifecycle lifecycle) {
		RequireOwningThread("InstallNodeHandler");
		const graph::NodeKindSpec *spec = graph::NodeCatalogue::Find(kind);
		if (spec == nullptr || spec->BuiltInBackend || !handler) {
			return false;
		}

		const BackendHandles handles = Backend();
		const bool live = handles.Device != nullptr;
		if (live && lifecycle.Reinstall && !lifecycle.Reinstall(handles)) {
			return false;
		}

		for (InstalledNodeHandler &installed : CustomNodeHandlers) {
			if (installed.Kind != kind) {
				continue;
			}
			if (installed.Live && installed.Lifecycle.Release) {
				installed.Lifecycle.Release(handles);
			}
			installed = InstalledNodeHandler{kind, std::move(handler), std::move(lifecycle), live};
			return true;
		}

		CustomNodeHandlers.push_back(
			InstalledNodeHandler{kind, std::move(handler), std::move(lifecycle), live}
		);
		return true;
	}

	bool Renderer::RemovePipeline(core::Name name) {
		RequireOwningThread("RemovePipeline");
		for (size_t index = 0; index < State->NamedPipelines.size(); index++) {
			if (State->NamedPipelines[index].Name != name) {
				continue;
			}
			if (State->Device != nullptr) {
				(void)WaitForFrame();
			}
			State->ReleaseGraphState(name);
			State->NamedPipelines.erase(State->NamedPipelines.begin() + static_cast<ptrdiff_t>(index));
			return true;
		}
		return false;
	}

	std::vector<core::Name> Renderer::Pipelines() const {
		std::vector<core::Name> names;
		names.reserve(State->NamedPipelines.size());
		for (const Impl::NamedPipeline &pipeline : State->NamedPipelines) {
			names.push_back(pipeline.Name);
		}
		std::sort(names.begin(), names.end(), [](core::Name first, core::Name second) {
			return first.Text() < second.Text();
		});
		return names;
	}

	void Renderer::ResetPipelines() {
		RequireOwningThread("ResetPipelines");
		if (State->Device != nullptr && !State->NamedPipelines.empty()) {
			(void)WaitForFrame();
		}
		for (const Impl::NamedPipeline &pipeline : State->NamedPipelines) {
			State->ReleaseGraphState(pipeline.Name);
		}
		State->NamedPipelines.clear();
	}

	bool Renderer::InstallEngineDefault(const graph::PipelineDocument &document) {
		graph::RenderGraph pipeline;
		core::Name offender;
		if (graph::Build(document, pipeline, offender) != graph::PipelineDocumentStatus::Ok) {
			ENGINE_ERROR("engine default render graph did not build at '{}'", offender.Text());
			return false;
		}

		graph::CompiledGraph compiled;
		graph::ExecutionSchedule schedule;
		std::string reason;
		if (!CompileRenderPipeline(pipeline, compiled, schedule, offender, reason)) {
			ENGINE_ERROR("engine default render graph refused: {} at '{}'", reason, offender.Text());
			return false;
		}
		std::vector<graph::PlannedCommandBuffer> buffers = graph::PlanCommandBuffers(schedule);
		State->EngineDefault = Impl::NamedPipeline{
			core::Name("Engine Default"),
			pipeline,
			std::move(compiled),
			std::move(schedule),
			std::move(buffers)
		};
		return true;
	}

	Renderer::Renderer() : State(std::make_unique<Impl>()), Owner(std::this_thread::get_id()) {
		(void)InstallEngineDefault(graph::DefaultPbrDocument());
	}

	Renderer::~Renderer() {
		Shutdown();
	}
}
