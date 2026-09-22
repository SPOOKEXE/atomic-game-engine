#include "GpuHeap.hpp"
#include "Primitives.hpp"
#include "ShaderBinary.hpp"

#include <engine/core/Log.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/render/InterfacePass.hpp>
#include <engine/render/ShaderLibrary.hpp>
#include <engine/render/WorldView.hpp>
#include <engine/resources/Shaders.hpp>

#include <SDL3/SDL_gpu.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <limits>
#include <vector>

namespace engine::render {

	std::optional<InterfaceGroupTarget> InterfaceGroupTargetFor(
		const core::Rect &bounds,
		const core::Rect &clip,
		const core::Vector2 &canvas,
		const core::Vector2 &targetPixels
	) {
		const auto finite = [](float value) { return std::isfinite(value); };
		if (!finite(bounds.Min.X) || !finite(bounds.Min.Y) || !finite(bounds.Max.X) ||
			!finite(bounds.Max.Y) || !finite(clip.Min.X) || !finite(clip.Min.Y) || !finite(clip.Max.X) ||
			!finite(clip.Max.Y) || !finite(canvas.X) || !finite(canvas.Y) || !finite(targetPixels.X) ||
			!finite(targetPixels.Y) || canvas.X <= 0.0f || canvas.Y <= 0.0f || targetPixels.X <= 0.0f ||
			targetPixels.Y <= 0.0f) {
			return std::nullopt;
		}

		const core::Rect clipped{
			{
				std::max({bounds.Min.X, clip.Min.X, 0.0f}),
				std::max({bounds.Min.Y, clip.Min.Y, 0.0f}),
			},
			{
				std::min({bounds.Max.X, clip.Max.X, canvas.X}),
				std::min({bounds.Max.Y, clip.Max.Y, canvas.Y}),
			},
		};
		const core::Vector2 extent = clipped.Max - clipped.Min;
		if (extent.X <= 0.0f || extent.Y <= 0.0f) return std::nullopt;

		const float width = std::ceil(extent.X * targetPixels.X / canvas.X);
		const float height = std::ceil(extent.Y * targetPixels.Y / canvas.Y);
		if (!finite(width) || !finite(height) || width <= 0.0f || height <= 0.0f ||
			width > MAXIMUM_INTERFACE_GROUP_TARGET_EDGE || height > MAXIMUM_INTERFACE_GROUP_TARGET_EDGE) {
			return std::nullopt;
		}
		return InterfaceGroupTarget{clipped, static_cast<uint32_t>(width), static_cast<uint32_t>(height)};
	}

	namespace {
		struct FragmentUniforms {
			float Clip[4]{};
			float MaskBounds[MAXIMUM_INTERFACE_MASKS][4]{};
			float MaskData[MAXIMUM_INTERFACE_MASKS][4]{};
			float MaskCount[4]{};
		};

		FragmentUniforms FragmentUniformsFor(const InterfaceBatch &batch) {
			FragmentUniforms uniforms{};
			uniforms.Clip[0] = batch.Clip.Min.X;
			uniforms.Clip[1] = batch.Clip.Min.Y;
			uniforms.Clip[2] = batch.Clip.Max.X;
			uniforms.Clip[3] = batch.Clip.Max.Y;
			uniforms.MaskCount[0] = static_cast<float>(batch.MaskCount);
			uniforms.MaskCount[1] = 1.0f;
			for (size_t index = 0; index < batch.MaskCount; index++) {
				const InterfaceMask &mask = batch.Masks[index];
				uniforms.MaskBounds[index][0] = mask.Bounds.Min.X;
				uniforms.MaskBounds[index][1] = mask.Bounds.Min.Y;
				uniforms.MaskBounds[index][2] = mask.Bounds.Max.X;
				uniforms.MaskBounds[index][3] = mask.Bounds.Max.Y;
				uniforms.MaskData[index][0] = mask.CornerRadius;
			}
			return uniforms;
		}

		FragmentUniforms FragmentUniformsFor(const core::Rect &clip) {
			InterfaceBatch batch;
			batch.Clip = clip;
			return FragmentUniformsFor(batch);
		}

		std::vector<uint8_t> ReadShader(std::string_view name, resources::ShaderForm form) {
			const auto path = resources::Shader(name, form);
			std::ifstream file(path, std::ios::binary | std::ios::ate);
			if (!file) {
				ENGINE_ERROR("interface pass: shader not found: {}", path.string());
				return {};
			}

			const std::streamsize size = file.tellg();
			file.seekg(0);
			std::vector<uint8_t> code(static_cast<size_t>(size));
			if (!file.read(reinterpret_cast<char *>(code.data()), size)) {
				return {};
			}
			return code;
		}

		SDL_GPUShader *Load(
			SDL_GPUDevice *device,
			std::string_view name,
			SDL_GPUShaderStage stage,
			uint32_t samplers,
			uint32_t uniforms
		) {
			// Asked of the device rather than assumed, the same way
			// `Renderer::LoadShader` asks. This pass creates its shaders from a
			// device somebody else made, so it has nowhere to carry the answer
			// and asks each time - twice per initialisation, against a string
			// compare inside SDL.
			const ShaderBinary binary = ShaderBinaryFor(device);

			const std::vector<uint8_t> code = ReadShader(name, binary.Form);
			if (code.empty()) {
				return nullptr;
			}

			SDL_GPUShaderCreateInfo info{};
			info.code = code.data();
			info.code_size = code.size();
			info.entrypoint = binary.EntryPoint;
			info.format = binary.Format;
			info.stage = stage;
			info.num_samplers = samplers;
			info.num_uniform_buffers = uniforms;

			SDL_GPUShader *shader = SDL_CreateGPUShader(device, &info);
			if (shader == nullptr) {
				ENGINE_ERROR("interface pass: SDL_CreateGPUShader({}): {}", name, SDL_GetError());
			}
			return shader;
		}
	}

	InterfacePass::~InterfacePass() {
		Shutdown();
	}

	bool InterfacePass::Initialise(void *device, uint32_t swapchainFormat, float pixelSize) {
		Shutdown();
		Device = device;
		SwapchainFormat = swapchainFormat;

		auto *gpu = static_cast<SDL_GPUDevice *>(device);
		if (gpu == nullptr) {
			return false;
		}

		// **The atlas first, because a pipeline with nothing to sample is a
		// pipeline that draws nothing.** A failure here is not fatal: the
		// interface still draws its rectangles and images, and the missing text
		// is visible as missing - `GlyphAtlas::Build` says why.
		Glyphs.Build(pixelSize);
		ShapedGlyphs.emplace(pixelSize);

		SDL_GPUShader *vertex = Load(gpu, "interface.vert", SDL_GPU_SHADERSTAGE_VERTEX, 0, 1);
		SDL_GPUShader *spatialVertex = Load(gpu, "interface_spatial.vert", SDL_GPU_SHADERSTAGE_VERTEX, 0, 1);
		SDL_GPUShader *fragment = Load(gpu, "interface.frag", SDL_GPU_SHADERSTAGE_FRAGMENT, 1, 1);
		if (vertex == nullptr || spatialVertex == nullptr || fragment == nullptr) {
			if (vertex != nullptr) {
				SDL_ReleaseGPUShader(gpu, vertex);
			}
			if (fragment != nullptr) {
				SDL_ReleaseGPUShader(gpu, fragment);
			}
			if (spatialVertex != nullptr) {
				SDL_ReleaseGPUShader(gpu, spatialVertex);
			}
			return false;
		}

		// The vertex layout, matching `InterfaceVertex` field for field. A
		// mismatch here is a shader reading a colour as a position, which draws
		// a spray of triangles across the screen rather than failing.
		SDL_GPUVertexBufferDescription buffer{};
		buffer.slot = 0;
		buffer.pitch = sizeof(InterfaceVertex);
		buffer.input_rate = SDL_GPU_VERTEXINPUTRATE_VERTEX;

		SDL_GPUVertexAttribute attributes[3]{};
		attributes[0].location = 0;
		attributes[0].buffer_slot = 0;
		attributes[0].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2;
		attributes[0].offset = offsetof(InterfaceVertex, X);

		attributes[1].location = 1;
		attributes[1].buffer_slot = 0;
		attributes[1].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2;
		attributes[1].offset = offsetof(InterfaceVertex, U);

		// **Normalised bytes, so the shader reads 0..1 without a divide.** The
		// vertex holds eight-bit channels because a full-screen interface is
		// tens of thousands of vertices and colour is the one attribute that
		// loses nothing to eight bits.
		attributes[2].location = 2;
		attributes[2].buffer_slot = 0;
		attributes[2].format = SDL_GPU_VERTEXELEMENTFORMAT_UBYTE4_NORM;
		attributes[2].offset = offsetof(InterfaceVertex, R);

		SDL_GPUColorTargetDescription target{};
		target.format = static_cast<SDL_GPUTextureFormat>(swapchainFormat);
		target.blend_state.enable_blend = true;
		target.blend_state.color_blend_op = SDL_GPU_BLENDOP_ADD;
		target.blend_state.alpha_blend_op = SDL_GPU_BLENDOP_ADD;

		// **Straight alpha, not premultiplied**, which is the opposite of the
		// overlay pipeline beside it and is deliberate: `gui::DrawCommand`
		// carries a tint and a transparency as separate numbers, so the vertex
		// colour reaching this blend has never had alpha folded into it.
		// Premultiplying would mean doing it somewhere, and the only somewhere
		// is a per-vertex multiply on the CPU for no gain.
		target.blend_state.src_color_blendfactor = SDL_GPU_BLENDFACTOR_SRC_ALPHA;
		target.blend_state.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
		target.blend_state.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
		target.blend_state.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;

		SDL_GPUGraphicsPipelineCreateInfo info{};
		info.vertex_shader = vertex;
		info.fragment_shader = fragment;
		info.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
		info.vertex_input_state.vertex_buffer_descriptions = &buffer;
		info.vertex_input_state.num_vertex_buffers = 1;
		info.vertex_input_state.vertex_attributes = attributes;
		info.vertex_input_state.num_vertex_attributes = 3;
		info.target_info.color_target_descriptions = &target;
		info.target_info.num_color_targets = 1;

		// **No depth and no culling.** The interface is drawn back to front in
		// the order the compile produced, so a depth test would hide a panel
		// behind a wall - and a quad wound the other way is invisible under
		// culling, which looks exactly like an element that failed to lay out.
		info.target_info.has_depth_stencil_target = false;
		info.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;

		Pipeline = SDL_CreateGPUGraphicsPipeline(gpu, &info);

		SDL_GPUGraphicsPipelineCreateInfo spatialInfo = info;
		spatialInfo.vertex_shader = spatialVertex;
		spatialInfo.vertex_input_state.vertex_buffer_descriptions = &buffer;
		spatialInfo.vertex_input_state.num_vertex_buffers = 1;
		spatialInfo.vertex_input_state.vertex_attributes = attributes;
		spatialInfo.vertex_input_state.num_vertex_attributes = 3;
		spatialInfo.target_info.color_target_descriptions = &target;
		spatialInfo.target_info.num_color_targets = 1;
		spatialInfo.target_info.has_depth_stencil_target = true;
		spatialInfo.target_info.depth_stencil_format = SDL_GPU_TEXTUREFORMAT_D32_FLOAT;
		spatialInfo.depth_stencil_state.enable_depth_test = true;
		spatialInfo.depth_stencil_state.enable_depth_write = false;
		spatialInfo.depth_stencil_state.compare_op = SDL_GPU_COMPAREOP_LESS_OR_EQUAL;
		SpatialPipeline = SDL_CreateGPUGraphicsPipeline(gpu, &spatialInfo);

		spatialInfo.depth_stencil_state.enable_depth_test = false;
		SpatialTopPipeline = SDL_CreateGPUGraphicsPipeline(gpu, &spatialInfo);
		const bool hdrSupported = SDL_GPUTextureSupportsFormat(
			gpu,
			SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT,
			SDL_GPU_TEXTURETYPE_2D,
			SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER
		);
		if (hdrSupported) {
			SDL_GPUColorTargetDescription hdrTarget = target;
			hdrTarget.format = SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT;
			spatialInfo.target_info.color_target_descriptions = &hdrTarget;
			HdrSpatialTopPipeline = SDL_CreateGPUGraphicsPipeline(gpu, &spatialInfo);
			spatialInfo.depth_stencil_state.enable_depth_test = true;
			HdrSpatialPipeline = SDL_CreateGPUGraphicsPipeline(gpu, &spatialInfo);
			spatialInfo.depth_stencil_state.enable_depth_write = true;
			HdrCapturePipeline = SDL_CreateGPUGraphicsPipeline(gpu, &spatialInfo);
		}

		SDL_ReleaseGPUShader(gpu, vertex);
		SDL_ReleaseGPUShader(gpu, spatialVertex);
		SDL_ReleaseGPUShader(gpu, fragment);

		if (Pipeline == nullptr || SpatialPipeline == nullptr || SpatialTopPipeline == nullptr ||
			(hdrSupported && (HdrSpatialPipeline == nullptr || HdrSpatialTopPipeline == nullptr ||
							  HdrCapturePipeline == nullptr))) {
			ENGINE_ERROR("interface pass: pipeline: {}", SDL_GetError());
			return false;
		}

		// **Linear, because an interface is drawn at fractional scales.** A
		// nearest sampler makes a glyph shimmer as a panel is dragged, which
		// reads as a font problem.
		SDL_GPUSamplerCreateInfo sampler{};
		sampler.min_filter = SDL_GPU_FILTER_LINEAR;
		sampler.mag_filter = SDL_GPU_FILTER_LINEAR;
		sampler.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;

		// Clamped, so a UV a rounding error pushed past the edge samples the
		// border rather than wrapping to the far side of the sheet - which
		// would draw an unrelated glyph.
		sampler.address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
		sampler.address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
		sampler.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;

		Sampler = SDL_CreateGPUSampler(gpu, &sampler);
		if (Sampler == nullptr) {
			ENGINE_ERROR("interface pass: sampler: {}", SDL_GetError());
			return false;
		}

		// **A second sampler rather than a second pipeline**, which is what
		// makes `ResampleMode` cheap: filtering is sampler state and nothing
		// else about the draw changes, so the two differ by one bind. The
		// batcher splits on the mode for the same reason it splits on a texture.
		sampler.min_filter = SDL_GPU_FILTER_NEAREST;
		sampler.mag_filter = SDL_GPU_FILTER_NEAREST;
		PixelSampler = SDL_CreateGPUSampler(gpu, &sampler);
		if (PixelSampler == nullptr) {
			ENGINE_ERROR("interface pass: pixel sampler: {}", SDL_GetError());
			return false;
		}

		if (Glyphs.Ready()) {
			SDL_GPUTextureCreateInfo texture{};
			texture.type = SDL_GPU_TEXTURETYPE_2D;

			// **RGBA even though the source is coverage.** SDL backends do not
			// expose one portable swizzle for an R8 sample: Vulkan and Metal may
			// place the missing channels differently. Expanding once at startup
			// makes every sampled texel white with coverage in alpha, which lets
			// glyphs, flat rectangles and images share the exact same shader.
			texture.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
			texture.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
			texture.width = Glyphs.Width();
			texture.height = Glyphs.Height();
			texture.layer_count_or_depth = 1;
			texture.num_levels = 1;

			AtlasTexture = gpu::CreateTexture(gpu, &texture);
			if (AtlasTexture == nullptr) {
				ENGINE_ERROR("interface pass: atlas texture: {}", SDL_GetError());
			}
		}

		// Upload before the first frame owns a command buffer. Keeping this copy
		// out of the frame command also makes the atlas available to every
		// recursive surface pass without relying on copy-to-render visibility
		// within one submission.
		if (AtlasTexture != nullptr) {
			SDL_GPUCommandBuffer *upload = SDL_AcquireGPUCommandBuffer(gpu);
			if (upload == nullptr || !UploadAtlas(upload)) {
				if (upload != nullptr) {
					SDL_CancelGPUCommandBuffer(upload);
				}
				ENGINE_ERROR("interface pass: atlas upload: {}", SDL_GetError());
				return false;
			}
			SDL_SubmitGPUCommandBuffer(upload);
			gpu::ReleaseTransferBuffer(gpu, static_cast<SDL_GPUTransferBuffer *>(AtlasTransferBuffer));
			AtlasTransferBuffer = nullptr;
		}

		return Pipeline != nullptr;
	}

	void InterfacePass::Shutdown() {
		auto *gpu = static_cast<SDL_GPUDevice *>(Device);
		if (gpu == nullptr) {
			Device = nullptr;
			return;
		}

		// Released in the reverse of the order they were made, which is what the
		// device expects and what keeps a validation layer quiet.
		if (TransferBuffer != nullptr) {
			gpu::ReleaseTransferBuffer(gpu, static_cast<SDL_GPUTransferBuffer *>(TransferBuffer));
		}
		if (AtlasTransferBuffer != nullptr) {
			gpu::ReleaseTransferBuffer(gpu, static_cast<SDL_GPUTransferBuffer *>(AtlasTransferBuffer));
		}
		for (void *buffer : ShapedAtlasTransferBuffers) {
			gpu::ReleaseTransferBuffer(gpu, static_cast<SDL_GPUTransferBuffer *>(buffer));
		}
		for (void *texture : ShapedAtlasTextures) {
			gpu::ReleaseTexture(gpu, static_cast<SDL_GPUTexture *>(texture));
		}
		if (IndexBuffer != nullptr) {
			gpu::ReleaseBuffer(gpu, static_cast<SDL_GPUBuffer *>(IndexBuffer));
		}
		if (VertexBuffer != nullptr) {
			gpu::ReleaseBuffer(gpu, static_cast<SDL_GPUBuffer *>(VertexBuffer));
		}
		if (CompositeTransferBuffer != nullptr) {
			gpu::ReleaseTransferBuffer(gpu, static_cast<SDL_GPUTransferBuffer *>(CompositeTransferBuffer));
		}
		if (CompositeIndexBuffer != nullptr) {
			gpu::ReleaseBuffer(gpu, static_cast<SDL_GPUBuffer *>(CompositeIndexBuffer));
		}
		if (CompositeVertexBuffer != nullptr) {
			gpu::ReleaseBuffer(gpu, static_cast<SDL_GPUBuffer *>(CompositeVertexBuffer));
		}
		for (const GroupLayer &layer : GroupLayers) {
			if (layer.Target != nullptr)
				gpu::ReleaseTexture(gpu, static_cast<SDL_GPUTexture *>(layer.Target));
		}
		GroupLayers.clear();
		Targets.Clear();
		PendingTargetWrites.clear();
		DirectTargetRanges.clear();
		if (AtlasTexture != nullptr) {
			gpu::ReleaseTexture(gpu, static_cast<SDL_GPUTexture *>(AtlasTexture));
		}
		if (PixelSampler != nullptr) {
			SDL_ReleaseGPUSampler(gpu, static_cast<SDL_GPUSampler *>(PixelSampler));
		}
		if (Sampler != nullptr) {
			SDL_ReleaseGPUSampler(gpu, static_cast<SDL_GPUSampler *>(Sampler));
		}
		if (Pipeline != nullptr) {
			SDL_ReleaseGPUGraphicsPipeline(gpu, static_cast<SDL_GPUGraphicsPipeline *>(Pipeline));
		}
		if (SpatialPipeline != nullptr) {
			SDL_ReleaseGPUGraphicsPipeline(gpu, static_cast<SDL_GPUGraphicsPipeline *>(SpatialPipeline));
		}
		if (SpatialTopPipeline != nullptr) {
			SDL_ReleaseGPUGraphicsPipeline(gpu, static_cast<SDL_GPUGraphicsPipeline *>(SpatialTopPipeline));
		}
		if (HdrSpatialPipeline != nullptr) {
			SDL_ReleaseGPUGraphicsPipeline(gpu, static_cast<SDL_GPUGraphicsPipeline *>(HdrSpatialPipeline));
		}
		if (HdrSpatialTopPipeline != nullptr) {
			SDL_ReleaseGPUGraphicsPipeline(
				gpu, static_cast<SDL_GPUGraphicsPipeline *>(HdrSpatialTopPipeline)
			);
		}
		for (const auto &[id, variant] : ShaderVariants) {
			(void)id;
			ReleaseShaderVariant(variant);
		}
		ShaderVariants.clear();
		ContentOwner = {};

		TransferBuffer = nullptr;
		CompositeTransferBuffer = nullptr;
		CompositeIndexBuffer = nullptr;
		CompositeVertexBuffer = nullptr;
		IndexBuffer = nullptr;
		VertexBuffer = nullptr;
		AtlasTexture = nullptr;
		AtlasTransferBuffer = nullptr;
		ShapedAtlasTransferBuffers.clear();
		ShapedAtlasTextures.clear();
		ShapedGlyphs.reset();
		ShapedUse = 0;
		Sampler = nullptr;
		PixelSampler = nullptr;
		Pipeline = nullptr;
		SpatialPipeline = nullptr;
		SpatialTopPipeline = nullptr;
		HdrSpatialPipeline = nullptr;
		if (HdrCapturePipeline)
			SDL_ReleaseGPUGraphicsPipeline(gpu, static_cast<SDL_GPUGraphicsPipeline *>(HdrCapturePipeline));
		HdrCapturePipeline = nullptr;
		HdrSpatialTopPipeline = nullptr;
		Device = nullptr;
		SwapchainFormat = 0;

		VertexCapacity = 0;
		IndexCapacity = 0;
		TransferCapacity = 0;
		CompositeCanvas = {};
		AtlasUploaded = false;
		SignatureValid = false;
		MeshDirty = true;
		LastUploadBytes = 0;
		Uploads = 0;
		Reuses = 0;
	}

	void InterfacePass::Submit(
		const gui::DrawList &list,
		const core::Vector2 &canvas,
		const core::Vector2 &targetPixels,
		ecs::Store &store
	) {
		Pending = list;
		SignatureValid = false;
		MeshDirty = true;
		PendingDamage.clear();
		PendingDamageValid = false;
		Canvas = canvas;
		TargetPixels = targetPixels;
		SpatialCollectors.clear();
		store.Each<const gui::SpatialCanvas>([&](ecs::Entity collector, const gui::SpatialCanvas &spatial) {
			SpatialCollectors.push_back(SpatialCollector{collector, spatial});
		});
	}

	void InterfacePass::Submit(
		const gui::DrawList &list,
		const core::Vector2 &canvas,
		const core::Vector2 &targetPixels,
		ecs::Store &store,
		uint64_t signature,
		bool damageValid,
		std::span<const gui::Compiled::DamageRegion> damage
	) {
		SubmitCommands(list, canvas, targetPixels, signature);
		PendingDamage.assign(damage.begin(), damage.end());
		PendingDamageValid = damageValid;
		SpatialCollectors.clear();
		store.Each<const gui::SpatialCanvas>([&](ecs::Entity collector, const gui::SpatialCanvas &spatial) {
			SpatialCollectors.push_back(SpatialCollector{collector, spatial});
		});
	}

	void InterfacePass::Submit(
		const WorldCameraFrame &frame, const core::Vector2 &canvas, const core::Vector2 &targetPixels
	) {
		SubmitCommands(frame.SpatialCommands, canvas, targetPixels, frame.Compiled.Signature());
		PendingDamage.clear();
		PendingDamageValid = false;
		SpatialCollectors = frame.SpatialCollectors;
	}

	void InterfacePass::SubmitCommands(
		const gui::DrawList &list,
		const core::Vector2 &canvas,
		const core::Vector2 &targetPixels,
		uint64_t signature
	) {
		if (!SignatureValid || PendingSignature != signature) {
			Pending = list;
			PendingSignature = signature;
			SignatureValid = true;
			MeshDirty = true;
		}
		Canvas = canvas;
		TargetPixels = targetPixels;
	}

	bool InterfacePass::AddShaderVariant(
		const core::Name &name, std::span<const uint32_t> spirv, core::Name owner
	) {
		if (!name.IsValid() || spirv.empty() || Device == nullptr || Pipeline == nullptr) {
			return false;
		}

		auto *gpu = static_cast<SDL_GPUDevice *>(Device);
		const ShaderBinary binary = ShaderBinaryFor(gpu);

		// **The runtime translation `Renderer::Impl::AddShaderVariant` does,
		// for its own reason**: a `ShaderScript` does not exist when the
		// build stages MSL, so a Metal device gets nothing at all unless this
		// translates while the engine runs.
		const bool toMsl = binary.Form == resources::ShaderForm::Msl;
		std::string translated;
		if (toMsl) {
			msl::Translation result = msl::Translate(spirv);
			if (result.Failed) {
				ENGINE_ERROR(
					"interface shader '{}' cannot be translated to MSL: {}", name.Text(), result.Error
				);
				return false;
			}
			translated = std::move(result.Source);
		}

		SDL_GPUShaderCreateInfo fragmentInfo{};
		fragmentInfo.code = toMsl ? reinterpret_cast<const Uint8 *>(translated.data())
								  : reinterpret_cast<const Uint8 *>(spirv.data());
		fragmentInfo.code_size = toMsl ? translated.size() : spirv.size() * sizeof(uint32_t);
		fragmentInfo.entrypoint = binary.EntryPoint;
		fragmentInfo.format = binary.Format;
		fragmentInfo.stage = SDL_GPU_SHADERSTAGE_FRAGMENT;

		// The fragment samples one image and reads the collector-space clip block.
		// SDL's pushed uniform data still needs a declared shader buffer slot.
		fragmentInfo.num_samplers = 1;
		fragmentInfo.num_uniform_buffers = 1;

		SDL_GPUShader *fragment = SDL_CreateGPUShader(gpu, &fragmentInfo);
		if (fragment == nullptr) {
			ENGINE_ERROR("interface shader '{}': {}", name.Text(), SDL_GetError());
			return false;
		}

		// Reloaded rather than kept, because SDL_GPU pipelines own what they
		// need from the shader objects that built them - `Initialise`
		// releases its own vertex shader the same way, right after the
		// pipeline that consumes it exists.
		SDL_GPUShader *vertex = Load(gpu, "interface.vert", SDL_GPU_SHADERSTAGE_VERTEX, 0, 1);
		if (vertex == nullptr) {
			SDL_ReleaseGPUShader(gpu, fragment);
			return false;
		}

		SDL_GPUVertexBufferDescription buffer{};
		buffer.slot = 0;
		buffer.pitch = sizeof(InterfaceVertex);
		buffer.input_rate = SDL_GPU_VERTEXINPUTRATE_VERTEX;

		SDL_GPUVertexAttribute attributes[3]{};
		attributes[0].location = 0;
		attributes[0].buffer_slot = 0;
		attributes[0].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2;
		attributes[0].offset = offsetof(InterfaceVertex, X);
		attributes[1].location = 1;
		attributes[1].buffer_slot = 0;
		attributes[1].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2;
		attributes[1].offset = offsetof(InterfaceVertex, U);
		attributes[2].location = 2;
		attributes[2].buffer_slot = 0;
		attributes[2].format = SDL_GPU_VERTEXELEMENTFORMAT_UBYTE4_NORM;
		attributes[2].offset = offsetof(InterfaceVertex, R);

		SDL_GPUColorTargetDescription target{};
		target.format = static_cast<SDL_GPUTextureFormat>(SwapchainFormat);
		target.blend_state.enable_blend = true;
		target.blend_state.color_blend_op = SDL_GPU_BLENDOP_ADD;
		target.blend_state.alpha_blend_op = SDL_GPU_BLENDOP_ADD;
		target.blend_state.src_color_blendfactor = SDL_GPU_BLENDFACTOR_SRC_ALPHA;
		target.blend_state.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
		target.blend_state.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
		target.blend_state.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;

		SDL_GPUGraphicsPipelineCreateInfo info{};
		info.vertex_shader = vertex;
		info.fragment_shader = fragment;
		info.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
		info.vertex_input_state.vertex_buffer_descriptions = &buffer;
		info.vertex_input_state.num_vertex_buffers = 1;
		info.vertex_input_state.vertex_attributes = attributes;
		info.vertex_input_state.num_vertex_attributes = 3;
		info.target_info.color_target_descriptions = &target;
		info.target_info.num_color_targets = 1;
		info.target_info.has_depth_stencil_target = false;
		info.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;

		ShaderVariant variant;
		variant.Screen = SDL_CreateGPUGraphicsPipeline(gpu, &info);
		SDL_GPUShader *spatialVertex = Load(gpu, "interface_spatial.vert", SDL_GPU_SHADERSTAGE_VERTEX, 0, 1);
		if (spatialVertex != nullptr) {
			info.vertex_shader = spatialVertex;
			info.target_info.has_depth_stencil_target = true;
			info.target_info.depth_stencil_format = SDL_GPU_TEXTUREFORMAT_D32_FLOAT;
			info.depth_stencil_state.enable_depth_test = true;
			info.depth_stencil_state.enable_depth_write = false;
			info.depth_stencil_state.compare_op = SDL_GPU_COMPAREOP_LESS_OR_EQUAL;
			variant.Spatial = SDL_CreateGPUGraphicsPipeline(gpu, &info);
			info.depth_stencil_state.enable_depth_test = false;
			variant.SpatialTop = SDL_CreateGPUGraphicsPipeline(gpu, &info);
			if (HdrSpatialPipeline != nullptr) {
				target.format = SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT;
				variant.HdrSpatialTop = SDL_CreateGPUGraphicsPipeline(gpu, &info);
				info.depth_stencil_state.enable_depth_test = true;
				variant.HdrSpatial = SDL_CreateGPUGraphicsPipeline(gpu, &info);
				info.depth_stencil_state.enable_depth_write = true;
				variant.HdrCapture = SDL_CreateGPUGraphicsPipeline(gpu, &info);
			}
			SDL_ReleaseGPUShader(gpu, spatialVertex);
		}
		SDL_ReleaseGPUShader(gpu, vertex);
		SDL_ReleaseGPUShader(gpu, fragment);
		if (variant.Screen == nullptr || variant.Spatial == nullptr || variant.SpatialTop == nullptr ||
			(HdrSpatialPipeline != nullptr &&
			 (variant.HdrSpatial == nullptr || variant.HdrSpatialTop == nullptr ||
			  variant.HdrCapture == nullptr))) {
			ENGINE_ERROR("interface shader '{}' pipelines: {}", name.Text(), SDL_GetError());
			ReleaseShaderVariant(variant);
			return false;
		}
		DropShaderVariant(name, owner);
		variant.CodeHash = assets::Hasher::Of(std::as_bytes(spirv));
		variant.AttemptHash = variant.CodeHash;
		ShaderVariants[ShaderKey(name, owner)] = variant;
		return true;
	}

	void InterfacePass::ReleaseShaderVariant(const ShaderVariant &variant) {
		if (Device == nullptr) return;
		for (void *pipeline :
			 {variant.Screen,
			  variant.Spatial,
			  variant.SpatialTop,
			  variant.HdrSpatial,
			  variant.HdrSpatialTop,
			  variant.HdrCapture}) {
			if (pipeline != nullptr)
				SDL_ReleaseGPUGraphicsPipeline(
					static_cast<SDL_GPUDevice *>(Device), static_cast<SDL_GPUGraphicsPipeline *>(pipeline)
				);
		}
	}

	bool InterfacePass::DropShaderVariant(const core::Name &name, core::Name owner) {
		const auto found = ShaderVariants.find(ShaderKey(name, owner));
		if (found == ShaderVariants.end()) {
			return false;
		}
		const bool held = found->second.Screen != nullptr;
		ReleaseShaderVariant(found->second);
		ShaderVariants.erase(found);
		return held;
	}

	bool InterfacePass::HasShaderVariant(core::Name name, core::Name owner) const {
		const auto found = ShaderVariants.find(ShaderKey(name, owner));
		return name.IsValid() && found != ShaderVariants.end() && found->second.Screen != nullptr;
	}

	size_t InterfacePass::RefreshShaders(
		std::span<const core::Name> demanded, const ShaderLibrary &library, core::Name owner
	) {
		if (Device == nullptr || Pipeline == nullptr) return 0;
		size_t changed = 0;
		for (auto entry = ShaderVariants.begin(); entry != ShaderVariants.end();) {
			const auto key = (entry++)->first;
			if (static_cast<uint32_t>(key >> 32) != owner.Id()) continue;
			const auto name = core::Name::FromId(static_cast<uint32_t>(key));
			const auto *module = library.Find(name, owner);
			if (std::find(demanded.begin(), demanded.end(), name) != demanded.end() && module != nullptr &&
				!module->CodeHash.IsZero())
				continue;
			changed += HasShaderVariant(name, owner);
			DropShaderVariant(name, owner);
		}
		for (const auto name : demanded) {
			const auto *module = library.Find(name, owner);
			if (module == nullptr || module->CodeHash.IsZero()) continue;
			const auto key = ShaderKey(name, owner);
			const auto found = ShaderVariants.find(key);
			if (found != ShaderVariants.end() && found->second.CodeHash == module->CodeHash) {
				found->second.AttemptHash = module->CodeHash;
				continue;
			}
			if (found != ShaderVariants.end() && found->second.AttemptHash == module->CodeHash) continue;
			changed += AddShaderVariant(name, module->SpirV, owner);
			// Remember refusals too, without discarding the previous accepted pipeline.
			ShaderVariants[key].AttemptHash = module->CodeHash;
		}
		return changed;
	}

	size_t InterfacePass::DropContentOwner(core::Name owner) {
		if (!owner.IsValid()) return 0;
		size_t removed = 0;
		for (auto entry = ShaderVariants.begin(); entry != ShaderVariants.end();) {
			const auto key = (entry++)->first;
			if (static_cast<uint32_t>(key >> 32) != owner.Id()) continue;
			removed += DropShaderVariant(core::Name::FromId(static_cast<uint32_t>(key)), owner);
		}
		return removed;
	}

	bool InterfacePass::UploadAtlas(void *commandBuffer) {
		if (AtlasUploaded || AtlasTexture == nullptr || !Glyphs.Ready()) {
			return AtlasUploaded;
		}

		auto *gpu = static_cast<SDL_GPUDevice *>(Device);
		const std::vector<uint8_t> &coverage = Glyphs.Coverage();
		std::vector<uint8_t> pixels(coverage.size() * 4);
		for (size_t index = 0; index < coverage.size(); index++) {
			pixels[index * 4 + 0] = 255;
			pixels[index * 4 + 1] = 255;
			pixels[index * 4 + 2] = 255;
			pixels[index * 4 + 3] = coverage[index];
		}

		SDL_GPUTransferBufferCreateInfo info{};
		info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
		info.size = static_cast<uint32_t>(pixels.size());

		SDL_GPUTransferBuffer *staging = gpu::CreateTransferBuffer(gpu, &info);
		if (staging == nullptr) {
			return false;
		}
		AtlasTransferBuffer = staging;

		void *mapped = SDL_MapGPUTransferBuffer(gpu, staging, false);
		if (mapped == nullptr) {
			gpu::ReleaseTransferBuffer(gpu, staging);
			AtlasTransferBuffer = nullptr;
			return false;
		}
		std::memcpy(mapped, pixels.data(), pixels.size());
		SDL_UnmapGPUTransferBuffer(gpu, staging);

		SDL_GPUCopyPass *copy = SDL_BeginGPUCopyPass(static_cast<SDL_GPUCommandBuffer *>(commandBuffer));

		SDL_GPUTextureTransferInfo source{};
		source.transfer_buffer = staging;
		source.offset = 0;
		source.pixels_per_row = Glyphs.Width();
		source.rows_per_layer = Glyphs.Height();

		SDL_GPUTextureRegion region{};
		region.texture = static_cast<SDL_GPUTexture *>(AtlasTexture);
		region.w = Glyphs.Width();
		region.h = Glyphs.Height();
		region.d = 1;

		SDL_UploadToGPUTexture(copy, &source, &region, false);
		SDL_EndGPUCopyPass(copy);

		AtlasUploaded = true;
		return true;
	}

	bool InterfacePass::UploadShapedAtlas(void *commandBuffer) {
		if (!ShapedGlyphs.has_value() || ShapedGlyphs->PageCount() == 0 || commandBuffer == nullptr) {
			return true;
		}

		auto *gpu = static_cast<SDL_GPUDevice *>(Device);
		const size_t pages = ShapedGlyphs->PageCount();
		while (ShapedAtlasTextures.size() < pages) {
			SDL_GPUTextureCreateInfo texture{};
			texture.type = SDL_GPU_TEXTURETYPE_2D;
			texture.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
			texture.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
			texture.width = ShapedGlyphAtlas::PAGE_EXTENT;
			texture.height = ShapedGlyphAtlas::PAGE_EXTENT;
			texture.layer_count_or_depth = 1;
			texture.num_levels = 1;
			void *created = gpu::CreateTexture(gpu, &texture);
			if (created == nullptr) {
				return false;
			}
			ShapedAtlasTextures.push_back(created);

			SDL_GPUTransferBufferCreateInfo staging{};
			staging.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
			staging.size =
				static_cast<uint32_t>(ShapedGlyphAtlas::PAGE_EXTENT) * ShapedGlyphAtlas::PAGE_EXTENT * 4;
			void *buffer = gpu::CreateTransferBuffer(gpu, &staging);
			if (buffer == nullptr) {
				gpu::ReleaseTexture(gpu, static_cast<SDL_GPUTexture *>(created));
				ShapedAtlasTextures.pop_back();
				return false;
			}
			ShapedAtlasTransferBuffers.push_back(buffer);
		}

		SDL_GPUCopyPass *copy = SDL_BeginGPUCopyPass(static_cast<SDL_GPUCommandBuffer *>(commandBuffer));
		if (copy == nullptr) return false;
		for (size_t page = 0; page < pages; page++) {
			const std::vector<uint8_t> &coverage = ShapedGlyphs->Coverage(static_cast<uint16_t>(page));
			if (coverage.size() !=
				static_cast<size_t>(ShapedGlyphAtlas::PAGE_EXTENT) * ShapedGlyphAtlas::PAGE_EXTENT) {
				continue;
			}
			void *mapped = SDL_MapGPUTransferBuffer(
				gpu, static_cast<SDL_GPUTransferBuffer *>(ShapedAtlasTransferBuffers[page]), false
			);
			if (mapped == nullptr) {
				SDL_EndGPUCopyPass(copy);
				return false;
			}
			auto *pixels = static_cast<uint8_t *>(mapped);
			for (size_t index = 0; index < coverage.size(); index++) {
				pixels[index * 4 + 0] = 255;
				pixels[index * 4 + 1] = 255;
				pixels[index * 4 + 2] = 255;
				pixels[index * 4 + 3] = coverage[index];
			}
			SDL_UnmapGPUTransferBuffer(
				gpu, static_cast<SDL_GPUTransferBuffer *>(ShapedAtlasTransferBuffers[page])
			);

			SDL_GPUTextureTransferInfo source{};
			source.transfer_buffer = static_cast<SDL_GPUTransferBuffer *>(ShapedAtlasTransferBuffers[page]);
			source.pixels_per_row = ShapedGlyphAtlas::PAGE_EXTENT;
			source.rows_per_layer = ShapedGlyphAtlas::PAGE_EXTENT;
			SDL_GPUTextureRegion target{};
			target.texture = static_cast<SDL_GPUTexture *>(ShapedAtlasTextures[page]);
			target.w = ShapedGlyphAtlas::PAGE_EXTENT;
			target.h = ShapedGlyphAtlas::PAGE_EXTENT;
			target.d = 1;
			SDL_UploadToGPUTexture(copy, &source, &target, false);
			LastUploadBytes += coverage.size() * 4;
		}
		SDL_EndGPUCopyPass(copy);
		return true;
	}

	bool InterfacePass::EnsureCompositeGeometry(void *commandBuffer) {
		if (CompositeVertexBuffer != nullptr && CompositeIndexBuffer != nullptr &&
			CompositeCanvas == Canvas) {
			return true;
		}
		if (commandBuffer == nullptr || Device == nullptr || Canvas.X <= 0.0f || Canvas.Y <= 0.0f) {
			return false;
		}
		auto *gpu = static_cast<SDL_GPUDevice *>(Device);
		if (CompositeTransferBuffer != nullptr) {
			gpu::ReleaseTransferBuffer(gpu, static_cast<SDL_GPUTransferBuffer *>(CompositeTransferBuffer));
			CompositeTransferBuffer = nullptr;
		}
		if (CompositeIndexBuffer != nullptr) {
			gpu::ReleaseBuffer(gpu, static_cast<SDL_GPUBuffer *>(CompositeIndexBuffer));
			CompositeIndexBuffer = nullptr;
		}
		if (CompositeVertexBuffer != nullptr) {
			gpu::ReleaseBuffer(gpu, static_cast<SDL_GPUBuffer *>(CompositeVertexBuffer));
			CompositeVertexBuffer = nullptr;
		}

		const InterfaceVertex vertices[4]{
			{0.0f, 0.0f, 0.0f, 0.0f},
			{1.0f, 0.0f, 1.0f, 0.0f},
			{1.0f, 1.0f, 1.0f, 1.0f},
			{0.0f, 1.0f, 0.0f, 1.0f},
		};
		const uint16_t indices[6]{0, 1, 2, 0, 2, 3};
		SDL_GPUBufferCreateInfo vertexInfo{};
		vertexInfo.usage = SDL_GPU_BUFFERUSAGE_VERTEX;
		vertexInfo.size = sizeof(vertices);
		CompositeVertexBuffer = gpu::CreateBuffer(gpu, &vertexInfo);
		SDL_GPUBufferCreateInfo indexInfo{};
		indexInfo.usage = SDL_GPU_BUFFERUSAGE_INDEX;
		indexInfo.size = sizeof(indices);
		CompositeIndexBuffer = gpu::CreateBuffer(gpu, &indexInfo);
		SDL_GPUTransferBufferCreateInfo transferInfo{};
		transferInfo.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
		transferInfo.size = sizeof(vertices) + sizeof(indices);
		CompositeTransferBuffer = gpu::CreateTransferBuffer(gpu, &transferInfo);
		if (CompositeVertexBuffer == nullptr || CompositeIndexBuffer == nullptr ||
			CompositeTransferBuffer == nullptr) {
			return false;
		}
		void *mapped = SDL_MapGPUTransferBuffer(
			gpu, static_cast<SDL_GPUTransferBuffer *>(CompositeTransferBuffer), true
		);
		if (mapped == nullptr) return false;
		std::memcpy(mapped, vertices, sizeof(vertices));
		std::memcpy(static_cast<uint8_t *>(mapped) + sizeof(vertices), indices, sizeof(indices));
		SDL_UnmapGPUTransferBuffer(gpu, static_cast<SDL_GPUTransferBuffer *>(CompositeTransferBuffer));
		SDL_GPUCopyPass *copy = SDL_BeginGPUCopyPass(static_cast<SDL_GPUCommandBuffer *>(commandBuffer));
		if (copy == nullptr) return false;
		SDL_GPUTransferBufferLocation source{};
		source.transfer_buffer = static_cast<SDL_GPUTransferBuffer *>(CompositeTransferBuffer);
		SDL_GPUBufferRegion target{};
		target.buffer = static_cast<SDL_GPUBuffer *>(CompositeVertexBuffer);
		target.size = sizeof(vertices);
		SDL_UploadToGPUBuffer(copy, &source, &target, true);
		source.offset = sizeof(vertices);
		target.buffer = static_cast<SDL_GPUBuffer *>(CompositeIndexBuffer);
		target.size = sizeof(indices);
		SDL_UploadToGPUBuffer(copy, &source, &target, true);
		SDL_EndGPUCopyPass(copy);
		CompositeCanvas = Canvas;
		LastUploadBytes += sizeof(vertices) + sizeof(indices);
		return true;
	}

	bool InterfacePass::Prepare(void *commandBuffer) {
		LastUploadBytes = 0;
		if (Pipeline == nullptr || commandBuffer == nullptr) {
			return false;
		}

		auto *gpu = static_cast<SDL_GPUDevice *>(Device);

		UploadAtlas(commandBuffer);

		std::vector<ResolvedImage> previousImages = std::move(ResolvedImages);
		ResolvedImages.clear();
		for (const gui::DrawCommand &draw : Pending.Commands) {
			const bool image = draw.Kind == gui::DrawKind::Image && draw.Image.IsValid();
			const bool viewport = draw.Kind == gui::DrawKind::Viewport;
			if (!image && !viewport) {
				continue;
			}

			const auto found =
				std::find_if(ResolvedImages.begin(), ResolvedImages.end(), [&](const ResolvedImage &entry) {
					return viewport ? entry.Viewport == draw.Source
									: entry.Viewport == ecs::NULL_ENTITY && entry.Name == draw.Image;
				});
			if (found == ResolvedImages.end()) {
				ResolvedImage resolved;
				resolved.Name = draw.Image;
				resolved.Viewport = viewport ? draw.Source : ecs::NULL_ENTITY;
				resolved.Value = viewport ? (Viewports ? Viewports(draw.Source) : InterfaceImage{})
										  : (Images ? Images(draw.Image) : InterfaceImage{});
				ResolvedImages.push_back(resolved);
			}
		}

		const auto sameGeometry = [](const ResolvedImage &left, const ResolvedImage &right) {
			return left.Name == right.Name && left.Viewport == right.Viewport &&
				   left.Value.Cell.Scale == right.Value.Cell.Scale &&
				   left.Value.Cell.OffsetU == right.Value.Cell.OffsetU &&
				   left.Value.Cell.OffsetV == right.Value.Cell.OffsetV &&
				   left.Value.UVMax == right.Value.UVMax && left.Value.Width == right.Value.Width &&
				   left.Value.Height == right.Value.Height;
		};
		if (previousImages.size() != ResolvedImages.size() ||
			!std::equal(previousImages.begin(), previousImages.end(), ResolvedImages.begin(), sameGeometry)) {
			MeshDirty = true;
		}

		if (!MeshDirty && VertexBuffer != nullptr && IndexBuffer != nullptr) {
			Reuses++;
			const bool drawable = !Mesh.Vertices().empty() && !Mesh.Indices().empty();
			return drawable && PrepareGroups(commandBuffer) && PrepareRetainedTargets(commandBuffer);
		}

		const auto information = [](const InterfaceImage &image) {
			return InterfaceImageInfo{
				core::Vector2{
					static_cast<float>(image.Width) * image.Cell.Scale,
					static_cast<float>(image.Height) * image.Cell.Scale,
				},
				image.Cell,
				image.UVMax,
			};
		};
		Mesh.Build(
			Pending,
			Glyphs,
			[&](const core::Name &name) {
				const auto found = std::find_if(
					ResolvedImages.begin(), ResolvedImages.end(), [&](const ResolvedImage &entry) {
						return entry.Viewport == ecs::NULL_ENTITY && entry.Name == name;
					}
				);
				if (found == ResolvedImages.end()) {
					return InterfaceImageInfo{};
				}
				return information(found->Value);
			},
			[&](ecs::Entity viewport) {
				const auto found = std::find_if(
					ResolvedImages.begin(), ResolvedImages.end(), [&](const ResolvedImage &entry) {
						return entry.Viewport == viewport;
					}
				);
				return found != ResolvedImages.end() ? information(found->Value) : InterfaceImageInfo{};
			},
			ShapedGlyphs ? &*ShapedGlyphs : nullptr,
			Fonts ? &*Fonts : nullptr,
			++ShapedUse
		);
		if (!UploadShapedAtlas(commandBuffer)) {
			return false;
		}
		Recorded = Mesh.Batches().size();

		const auto vertices = static_cast<uint32_t>(Mesh.Vertices().size());
		const auto indices = static_cast<uint32_t>(Mesh.Indices().size());
		if (vertices == 0 || indices == 0) {
			MeshDirty = false;
			return false;
		}

		const uint32_t vertexBytes = vertices * sizeof(InterfaceVertex);
		const uint32_t indexBytes = indices * sizeof(uint16_t);

		// **Grown and never shrunk.** An interface that opened a large panel
		// once and closed it would otherwise reallocate on the frame it opened
		// again - and a buffer reallocation mid-frame is a stall the panel gets
		// blamed for.
		if (VertexBuffer == nullptr || VertexCapacity < vertexBytes) {
			if (VertexBuffer != nullptr) {
				gpu::ReleaseBuffer(gpu, static_cast<SDL_GPUBuffer *>(VertexBuffer));
			}
			SDL_GPUBufferCreateInfo info{};
			info.usage = SDL_GPU_BUFFERUSAGE_VERTEX;
			info.size = vertexBytes;
			VertexBuffer = gpu::CreateBuffer(gpu, &info);
			VertexCapacity = VertexBuffer != nullptr ? vertexBytes : 0;
		}

		if (IndexBuffer == nullptr || IndexCapacity < indexBytes) {
			if (IndexBuffer != nullptr) {
				gpu::ReleaseBuffer(gpu, static_cast<SDL_GPUBuffer *>(IndexBuffer));
			}
			SDL_GPUBufferCreateInfo info{};
			info.usage = SDL_GPU_BUFFERUSAGE_INDEX;
			info.size = indexBytes;
			IndexBuffer = gpu::CreateBuffer(gpu, &info);
			IndexCapacity = IndexBuffer != nullptr ? indexBytes : 0;
		}

		if (VertexBuffer == nullptr || IndexBuffer == nullptr) {
			return false;
		}

		const uint32_t total = vertexBytes + indexBytes;
		if (TransferBuffer == nullptr || TransferCapacity < total) {
			if (TransferBuffer != nullptr) {
				gpu::ReleaseTransferBuffer(gpu, static_cast<SDL_GPUTransferBuffer *>(TransferBuffer));
			}
			SDL_GPUTransferBufferCreateInfo info{};
			info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
			info.size = total;
			TransferBuffer = gpu::CreateTransferBuffer(gpu, &info);
			TransferCapacity = TransferBuffer != nullptr ? total : 0;
		}

		if (TransferBuffer == nullptr) {
			return false;
		}

		auto *staging = static_cast<SDL_GPUTransferBuffer *>(TransferBuffer);

		// **Cycled, because this buffer was written last frame and may still be
		// in flight.** Without it the copy would overwrite bytes a queued frame
		// has not read yet, which draws last frame's interface at this frame's
		// positions - a tearing that only appears under load.
		if (auto *mapped = static_cast<uint8_t *>(SDL_MapGPUTransferBuffer(gpu, staging, true))) {
			std::memcpy(mapped, Mesh.Vertices().data(), vertexBytes);
			std::memcpy(mapped + vertexBytes, Mesh.Indices().data(), indexBytes);
			SDL_UnmapGPUTransferBuffer(gpu, staging);
		} else {
			return false;
		}

		SDL_GPUCopyPass *copy = SDL_BeginGPUCopyPass(static_cast<SDL_GPUCommandBuffer *>(commandBuffer));

		SDL_GPUTransferBufferLocation from{};
		from.transfer_buffer = staging;
		from.offset = 0;

		SDL_GPUBufferRegion to{};
		to.buffer = static_cast<SDL_GPUBuffer *>(VertexBuffer);
		to.offset = 0;
		to.size = vertexBytes;
		SDL_UploadToGPUBuffer(copy, &from, &to, true);

		from.offset = vertexBytes;
		to.buffer = static_cast<SDL_GPUBuffer *>(IndexBuffer);
		to.size = indexBytes;
		SDL_UploadToGPUBuffer(copy, &from, &to, true);

		SDL_EndGPUCopyPass(copy);
		MeshDirty = false;
		LastUploadBytes += total;
		Uploads++;
		return PrepareGroups(commandBuffer) && PrepareRetainedTargets(commandBuffer);
	}

	void InterfacePass::RecordScreenRange(
		void *commandBuffer,
		void *renderPass,
		size_t firstCommand,
		size_t commandCount,
		const core::Rect *damage,
		const core::Rect *targetBounds,
		const core::Vector2 *targetPixels
	) {
		auto *command = static_cast<SDL_GPUCommandBuffer *>(commandBuffer);
		auto *pass = static_cast<SDL_GPURenderPass *>(renderPass);
		if (command == nullptr || pass == nullptr || Pipeline == nullptr) {
			return;
		}

		auto *defaultPipeline = static_cast<SDL_GPUGraphicsPipeline *>(Pipeline);
		SDL_BindGPUGraphicsPipeline(pass, defaultPipeline);
		SDL_GPUGraphicsPipeline *boundPipeline = defaultPipeline;

		SDL_GPUBufferBinding vertex{};
		vertex.buffer = static_cast<SDL_GPUBuffer *>(VertexBuffer);
		SDL_BindGPUVertexBuffers(pass, 0, &vertex, 1);

		SDL_GPUBufferBinding index{};
		index.buffer = static_cast<SDL_GPUBuffer *>(IndexBuffer);
		SDL_BindGPUIndexBuffer(pass, &index, SDL_GPU_INDEXELEMENTSIZE_16BIT);

		auto *atlas = static_cast<SDL_GPUTexture *>(AtlasTexture);

		// **Rebound per batch only when it changes.** A bind is cheap and a
		// redundant one is not free; tracking the last one is four lines against
		// a bind per quad. The sampler is part of the same binding, so a change
		// of filtering counts as a change of texture for this purpose.
		SDL_GPUTexture *bound = nullptr;
		SDL_GPUSampler *boundSampler = nullptr;

		const core::Rect localBounds = targetBounds != nullptr ? *targetBounds : core::Rect{};
		const core::Vector2 localCanvas =
			targetBounds != nullptr ? localBounds.Max - localBounds.Min : Canvas;
		const core::Vector2 localPixels = targetPixels != nullptr ? *targetPixels : TargetPixels;
		const size_t endCommand = firstCommand + commandCount;
		for (const InterfaceBatch &batch : Mesh.Batches()) {
			if (batch.FirstCommand < firstCommand || batch.FirstCommand >= endCommand) {
				continue;
			}
			const bool spatial = std::any_of(
				SpatialCollectors.begin(), SpatialCollectors.end(), [&](const SpatialCollector &entry) {
					return entry.Collector == batch.Collector;
				}
			);
			if (spatial) {
				continue;
			}
			const auto transform = std::find_if(
				Pending.Transforms.begin(),
				Pending.Transforms.end(),
				[&](const gui::CollectorTransform &entry) { return entry.Collector == batch.Collector; }
			);
			const core::Vector2 origin = targetBounds != nullptr				 ? -localBounds.Min
										 : transform != Pending.Transforms.end() ? transform->Origin
																				 : core::Vector2::Zero;
			const core::Vector2 scale = targetBounds != nullptr					? core::Vector2{1.0f, 1.0f}
										: transform != Pending.Transforms.end() ? transform->Scale
																				: core::Vector2{1.0f, 1.0f};

			// Screen collectors use their logical canvas while world collectors
			// retain the display canvas submitted by the client. A shared list can
			// contain both, and a single canvas would scale one of those spaces
			// incorrectly when the interface scale differs from one.
			const float canvas[6] = {
				localCanvas.X > 0.0f ? localCanvas.X : 1.0f,
				localCanvas.Y > 0.0f ? localCanvas.Y : 1.0f,
				origin.X,
				origin.Y,
				scale.X,
				scale.Y,
			};
			SDL_PushGPUVertexUniformData(command, 0, canvas, sizeof(canvas));

			// **Bound per batch only when it changes**, `bound`'s own reason
			// applied to a pipeline instead of a texture. A batch naming no
			// shader - the ordinary case - or one whose variant has not
			// built yet draws with the pass's own shader rather than
			// nothing: the same "keep the last frame's" the codebase reaches
			// for whenever a resolve can fail.
			SDL_GPUGraphicsPipeline *wanted = defaultPipeline;
			if (batch.Shader.IsValid()) {
				const auto found = ShaderVariants.find(ShaderKey(batch.Shader, ContentOwner));
				if (found != ShaderVariants.end()) {
					wanted = static_cast<SDL_GPUGraphicsPipeline *>(found->second.Screen);
				}
			}
			if (wanted != boundPipeline) {
				SDL_BindGPUGraphicsPipeline(pass, wanted);
				boundPipeline = wanted;
			}

			SDL_GPUTexture *texture = atlas;
			if (batch.ShapedPage != UINT16_MAX && batch.ShapedPage < ShapedAtlasTextures.size()) {
				texture = static_cast<SDL_GPUTexture *>(ShapedAtlasTextures[batch.ShapedPage]);
			}

			if (batch.Image.IsValid() || batch.Viewport != ecs::NULL_ENTITY) {
				const auto found = std::find_if(
					ResolvedImages.begin(), ResolvedImages.end(), [&](const ResolvedImage &entry) {
						return batch.Viewport != ecs::NULL_ENTITY
								   ? entry.Viewport == batch.Viewport
								   : entry.Viewport == ecs::NULL_ENTITY && entry.Name == batch.Image;
					}
				);
				if (found != ResolvedImages.end() && found->Value.Texture != nullptr) {
					texture = static_cast<SDL_GPUTexture *>(found->Value.Texture);
				} else {
				}
				// **An unresolved name falls back to the atlas**, which draws
				// the image's bounds as a flat tinted rectangle. Visible on
				// purpose: an `ImageLabel` that drew nothing would look like the
				// label was broken rather than like the image was missing.
			}

			if (texture == nullptr) {
				continue;
			}

			auto *sampler = static_cast<SDL_GPUSampler *>(
				batch.Resample == gui::ResampleMode::Pixelated && PixelSampler != nullptr ? PixelSampler
																						  : Sampler
			);

			if (texture != bound || sampler != boundSampler) {
				SDL_GPUTextureSamplerBinding binding{};
				binding.texture = texture;
				binding.sampler = sampler;
				SDL_BindGPUFragmentSamplers(pass, 0, &binding, 1);
				bound = texture;
				boundSampler = sampler;
			}

			const FragmentUniforms uniforms = FragmentUniformsFor(batch);
			SDL_PushGPUFragmentUniformData(
				static_cast<SDL_GPUCommandBuffer *>(commandBuffer), 0, &uniforms, sizeof(uniforms)
			);

			// **The scissor is in device pixels and `Clip` is in canvas units.**
			// `ScissorFor` carries why those are not the same number and what
			// clipping the interface to a fraction of the panel looked like.
			core::Rect clippedCanvas = batch.Clip;
			if (damage != nullptr) {
				clippedCanvas.Min.X = std::max(clippedCanvas.Min.X, damage->Min.X);
				clippedCanvas.Min.Y = std::max(clippedCanvas.Min.Y, damage->Min.Y);
				clippedCanvas.Max.X = std::min(clippedCanvas.Max.X, damage->Max.X);
				clippedCanvas.Max.Y = std::min(clippedCanvas.Max.Y, damage->Max.Y);
			}
			const core::Rect presentationClip{
				origin + clippedCanvas.Min * scale,
				origin + clippedCanvas.Max * scale,
			};
			const InterfaceScissor clipped = ScissorFor(presentationClip, localCanvas, localPixels);
			if (clipped.Empty()) {
				continue;
			}

			const SDL_Rect scissor{clipped.X, clipped.Y, clipped.Width, clipped.Height};
			SDL_SetGPUScissor(pass, &scissor);

			SDL_DrawGPUIndexedPrimitives(pass, batch.IndexCount, 1, batch.FirstIndex, 0, 0);
		}
	}

	void InterfacePass::RecordComposite(
		void *commandBuffer,
		void *renderPass,
		void *texture,
		const core::Rect &bounds,
		float opacity,
		const core::Rect *targetBounds,
		const core::Vector2 *targetPixels
	) {
		auto *command = static_cast<SDL_GPUCommandBuffer *>(commandBuffer);
		auto *pass = static_cast<SDL_GPURenderPass *>(renderPass);
		if (command == nullptr || pass == nullptr || texture == nullptr || Pipeline == nullptr ||
			CompositeVertexBuffer == nullptr || CompositeIndexBuffer == nullptr) {
			return;
		}
		SDL_BindGPUGraphicsPipeline(pass, static_cast<SDL_GPUGraphicsPipeline *>(Pipeline));
		SDL_GPUBufferBinding vertex{};
		vertex.buffer = static_cast<SDL_GPUBuffer *>(CompositeVertexBuffer);
		SDL_BindGPUVertexBuffers(pass, 0, &vertex, 1);
		SDL_GPUBufferBinding index{};
		index.buffer = static_cast<SDL_GPUBuffer *>(CompositeIndexBuffer);
		SDL_BindGPUIndexBuffer(pass, &index, SDL_GPU_INDEXELEMENTSIZE_16BIT);
		const core::Vector2 extent = bounds.Max - bounds.Min;
		if (extent.X <= 0.0f || extent.Y <= 0.0f) return;
		const core::Rect destination = targetBounds != nullptr ? *targetBounds : core::Rect{};
		const core::Vector2 destinationExtent =
			targetBounds != nullptr ? destination.Max - destination.Min : Canvas;
		const core::Vector2 destinationPixels = targetPixels != nullptr ? *targetPixels : TargetPixels;
		const core::Vector2 origin = targetBounds != nullptr ? bounds.Min - destination.Min : bounds.Min;
		const float canvas[6]{
			destinationExtent.X, destinationExtent.Y, origin.X, origin.Y, extent.X, extent.Y
		};
		SDL_PushGPUVertexUniformData(command, 0, canvas, sizeof(canvas));
		FragmentUniforms uniforms = FragmentUniformsFor({{0.0f, 0.0f}, {1.0f, 1.0f}});
		uniforms.MaskCount[1] = std::clamp(opacity, 0.0f, 1.0f);
		SDL_PushGPUFragmentUniformData(command, 0, &uniforms, sizeof(uniforms));
		SDL_GPUTextureSamplerBinding binding{};
		binding.texture = static_cast<SDL_GPUTexture *>(texture);
		binding.sampler = static_cast<SDL_GPUSampler *>(Sampler);
		SDL_BindGPUFragmentSamplers(pass, 0, &binding, 1);
		const core::Rect localBounds{origin, origin + extent};
		const InterfaceScissor whole = ScissorFor(localBounds, destinationExtent, destinationPixels);
		if (whole.Empty()) return;
		const SDL_Rect scissor{whole.X, whole.Y, whole.Width, whole.Height};
		SDL_SetGPUScissor(pass, &scissor);
		SDL_DrawGPUIndexedPrimitives(pass, 6, 1, 0, 0, 0);
	}

	void InterfacePass::RecordGroupRange(
		void *commandBuffer,
		void *renderPass,
		size_t firstCommand,
		size_t endCommand,
		size_t parent,
		const core::Rect *targetBounds,
		const core::Vector2 *targetPixels
	) {
		size_t cursor = firstCommand;
		for (size_t index = 0; index < GroupLayers.size(); index++) {
			const GroupLayer &child = GroupLayers[index];
			if (child.Parent != parent || child.First < cursor || child.End > endCommand) continue;
			RecordScreenRange(
				commandBuffer, renderPass, cursor, child.First - cursor, nullptr, targetBounds, targetPixels
			);
			if (child.Target != nullptr) {
				RecordComposite(
					commandBuffer,
					renderPass,
					child.Target,
					child.Bounds,
					child.Opacity,
					targetBounds,
					targetPixels
				);
			} else {
				if (child.Opacity < 1.0f) {
					// The source range remains the only visible fallback when its
					// transient target could not be made. A direct batch cannot apply
					// one opacity after its children blended, so report the deliberate
					// degradation instead of pretending this is isolated composition.
					ENGINE_WARN_EVERY(
						5.0, "interface CanvasGroup target unavailable; drawing unisolated children"
					);
				}
				RecordGroupRange(
					commandBuffer, renderPass, child.First, child.End, index, targetBounds, targetPixels
				);
			}
			cursor = child.End;
		}
		RecordScreenRange(
			commandBuffer, renderPass, cursor, endCommand - cursor, nullptr, targetBounds, targetPixels
		);
	}

	bool InterfacePass::PrepareGroups(void *commandBuffer) {
		constexpr size_t maximumGroups = 8;
		constexpr uint64_t maximumBytes = 64ull * 1024 * 1024;
		auto *gpu = static_cast<SDL_GPUDevice *>(Device);
		for (const GroupLayer &layer : GroupLayers) {
			if (layer.Target != nullptr)
				gpu::ReleaseTexture(gpu, static_cast<SDL_GPUTexture *>(layer.Target));
		}
		GroupLayers.clear();
		if (Pending.Operations.empty() || Canvas.X <= 0.0f || Canvas.Y <= 0.0f) return true;
		// Groups retain their source commands when the shared composite geometry
		// cannot be prepared. Returning success lets Record use that direct path
		// rather than making a transient allocation failure erase the interface.
		if (!EnsureCompositeGeometry(commandBuffer)) return true;

		std::vector<size_t> stack;
		for (const gui::DrawOperation &operation : Pending.Operations) {
			if (operation.Kind == gui::DrawOperationKind::BeginGroup) {
				if (stack.size() >= maximumGroups) {
					ENGINE_WARN_EVERY(
						5.0, "interface group nesting exceeds {}; group was refused", maximumGroups
					);
					stack.push_back(SIZE_MAX);
					continue;
				}
				const core::Rect bounds{
					{
						std::max(operation.Bounds.Min.X, operation.Clip.Min.X),
						std::max(operation.Bounds.Min.Y, operation.Clip.Min.Y),
					},
					{
						std::min(operation.Bounds.Max.X, operation.Clip.Max.X),
						std::min(operation.Bounds.Max.Y, operation.Clip.Max.Y),
					},
				};
				size_t parent = SIZE_MAX;
				for (auto ancestor = stack.rbegin(); ancestor != stack.rend(); ++ancestor) {
					if (*ancestor != SIZE_MAX) {
						parent = *ancestor;
						break;
					}
				}
				GroupLayers.push_back(
					{operation.Command,
					 operation.Command,
					 parent,
					 bounds,
					 {},
					 1.0f - std::clamp(operation.Transparency, 0.0f, 1.0f),
					 nullptr}
				);
				stack.push_back(GroupLayers.size() - 1);
				continue;
			}
			if (operation.Kind != gui::DrawOperationKind::EndGroup || stack.empty()) continue;
			const size_t index = stack.back();
			stack.pop_back();
			if (index == SIZE_MAX || operation.Command <= GroupLayers[index].First ||
				operation.Command > Pending.Commands.size()) {
				continue;
			}
			GroupLayers[index].End = operation.Command;
		}

		uint64_t usedBytes = 0;
		for (size_t reverse = GroupLayers.size(); reverse > 0; reverse--) {
			GroupLayer &group = GroupLayers[reverse - 1];
			if (group.End <= group.First) continue;
			const auto targetRegion =
				InterfaceGroupTargetFor(group.Bounds, {{0.0f, 0.0f}, Canvas}, Canvas, TargetPixels);
			if (!targetRegion.has_value()) continue;
			group.Bounds = targetRegion->Bounds;
			const uint32_t width = targetRegion->Width;
			const uint32_t height = targetRegion->Height;
			const uint64_t bytes = SDL_CalculateGPUTextureFormatSize(
				static_cast<SDL_GPUTextureFormat>(SwapchainFormat), width, height, 1
			);
			if (width == 0 || height == 0 || bytes > maximumBytes || usedBytes > maximumBytes - bytes) {
				ENGINE_WARN_EVERY(
					5.0, "interface canvas groups exceed the {} byte transient limit", maximumBytes
				);
				continue;
			}
			SDL_GPUTextureCreateInfo texture{};
			texture.type = SDL_GPU_TEXTURETYPE_2D;
			texture.format = static_cast<SDL_GPUTextureFormat>(SwapchainFormat);
			texture.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
			texture.width = width;
			texture.height = height;
			texture.layer_count_or_depth = 1;
			texture.num_levels = 1;
			auto *target = gpu::CreateTexture(gpu, &texture);
			if (target == nullptr) continue;
			SDL_GPUColorTargetInfo colour{};
			colour.texture = target;
			colour.clear_color = SDL_FColor{0.0f, 0.0f, 0.0f, 0.0f};
			colour.load_op = SDL_GPU_LOADOP_CLEAR;
			colour.store_op = SDL_GPU_STOREOP_STORE;
			colour.cycle = true;
			auto *pass = SDL_BeginGPURenderPass(
				static_cast<SDL_GPUCommandBuffer *>(commandBuffer), &colour, 1, nullptr
			);
			if (pass == nullptr) {
				gpu::ReleaseTexture(gpu, target);
				continue;
			}
			group.Pixels = {static_cast<float>(width), static_cast<float>(height)};
			group.Target = target;
			RecordGroupRange(
				commandBuffer, pass, group.First, group.End, reverse - 1, &group.Bounds, &group.Pixels
			);
			SDL_EndGPURenderPass(pass);
			usedBytes += bytes;
		}
		return true;
	}

	bool InterfacePass::PrepareRetainedTargets(void *commandBuffer) {
		DirectTargetRanges.clear();
		if (!GroupLayers.empty()) {
			// `Record` selects RecordGroupRange whenever a CanvasGroup is present.
			// Preparing collector targets here would write images that this frame
			// cannot composite, then incorrectly advance their retained baselines in
			// CompleteFrame. The group path owns the full screen composition.
			return true;
		}
		if (Pending.CollectorRanges.empty()) return true;
		const uint32_t width = static_cast<uint32_t>(std::max(TargetPixels.X, Canvas.X));
		const uint32_t height = static_cast<uint32_t>(std::max(TargetPixels.Y, Canvas.Y));
		if (!EnsureCompositeGeometry(commandBuffer)) {
			// A retained target may already be ready from an earlier frame. Without
			// composite geometry `RecordComposite` cannot submit it, so force the
			// source batches through the direct path instead of dropping that UI.
			for (const gui::CollectorRange &range : Pending.CollectorRanges) {
				if (range.Spatial || range.Count == 0) continue;
				DirectTargetRanges.push_back(
					InterfaceTargetKey{range.Collector, 0, width, height, range.First, range.Count, false}
				);
			}
			return true;
		}
		if (Device == nullptr || Canvas.X <= 0.0f || Canvas.Y <= 0.0f) return false;
		auto *gpu = static_cast<SDL_GPUDevice *>(Device);
		if (width == 0 || height == 0 ||
			!SDL_GPUTextureSupportsFormat(
				gpu,
				static_cast<SDL_GPUTextureFormat>(SwapchainFormat),
				SDL_GPU_TEXTURETYPE_2D,
				SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER
			)) {
			// A target capability miss degrades to the direct screen path. The
			// source batches remain valid and an older target, if any, stays usable.
			return true;
		}
		const auto dynamicRange = [&](const gui::CollectorRange &range) {
			const size_t end = std::min(range.First + range.Count, Pending.Commands.size());
			return std::any_of(
				Pending.Commands.begin() + std::min(range.First, Pending.Commands.size()),
				Pending.Commands.begin() + end,
				[](const gui::DrawCommand &command) {
					return command.Kind == gui::DrawKind::Image || command.Kind == gui::DrawKind::Viewport;
				}
			);
		};
		for (const gui::CollectorRange &range : Pending.CollectorRanges) {
			if (range.Spatial || range.Count == 0) continue;
			const InterfaceTargetKey key{range.Collector, 0, width, height, range.First, range.Count, false};
			// Content texture revisions are resolved at render time and are not in
			// the compiled UI signature. Keep those ranges direct until the image
			// resolver exposes a stable revision for a retained key.
			if (dynamicRange(range)) {
				DirectTargetRanges.push_back(key);
				continue;
			}
			const InterfaceTargetPlan plan =
				Targets.Begin(key, PendingSignature, PendingDamageValid, PendingDamage);
			if (plan.Work == InterfaceTargetWork::Skip) continue;
			void *target = Targets.Target(key);
			if (target == nullptr) {
				SDL_GPUTextureCreateInfo texture{};
				texture.type = SDL_GPU_TEXTURETYPE_2D;
				texture.format = static_cast<SDL_GPUTextureFormat>(SwapchainFormat);
				texture.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
				texture.width = width;
				texture.height = height;
				texture.layer_count_or_depth = 1;
				texture.num_levels = 1;
				target = gpu::CreateTexture(gpu, &texture);
				const uint64_t bytes = SDL_CalculateGPUTextureFormatSize(texture.format, width, height, 1);
				if (target == nullptr || !Targets.Attach(key, target, bytes, [gpu](void *resource) {
						gpu::ReleaseTexture(gpu, static_cast<SDL_GPUTexture *>(resource));
					})) {
					continue;
				}
			}
			SDL_GPUColorTargetInfo colour{};
			colour.texture = static_cast<SDL_GPUTexture *>(target);
			colour.clear_color = SDL_FColor{0.0f, 0.0f, 0.0f, 0.0f};
			// Alpha blending cannot remove pixels left by a deleted command. Keep
			// the compiler's partial plan pending, but repaint this bounded target
			// from a clear baseline until the backend has a damage-rect clear path.
			colour.load_op = SDL_GPU_LOADOP_CLEAR;
			colour.store_op = SDL_GPU_STOREOP_STORE;
			colour.cycle = true;
			SDL_GPURenderPass *pass = SDL_BeginGPURenderPass(
				static_cast<SDL_GPUCommandBuffer *>(commandBuffer), &colour, 1, nullptr
			);
			if (pass == nullptr) {
				DirectTargetRanges.push_back(key);
				continue;
			}
			RecordScreenRange(commandBuffer, pass, range.First, range.Count, nullptr);
			SDL_EndGPURenderPass(pass);
			PendingTargetWrites.push_back(key);
		}
		return true;
	}

	void InterfacePass::CompleteFrame(bool submitted) {
		for (const InterfaceTargetKey &key : PendingTargetWrites) {
			Targets.Complete(key, submitted);
		}
		PendingTargetWrites.clear();
	}

	void InterfacePass::Record(void *commandBuffer, void *renderPass) {
		if (commandBuffer == nullptr || renderPass == nullptr || Pipeline == nullptr) return;
		if (!GroupLayers.empty()) {
			RecordGroupRange(
				commandBuffer, renderPass, 0, Pending.Commands.size(), SIZE_MAX, nullptr, nullptr
			);
			return;
		}
		const uint32_t width = static_cast<uint32_t>(std::max(TargetPixels.X, Canvas.X));
		const uint32_t height = static_cast<uint32_t>(std::max(TargetPixels.Y, Canvas.Y));
		const std::vector<InterfaceTargetComposite> composites = Targets.Composite(Pending, 0, width, height);
		for (const gui::CollectorRange &range : Pending.CollectorRanges) {
			if (range.Spatial || range.Count == 0) continue;
			const InterfaceTargetKey key{range.Collector, 0, width, height, range.First, range.Count, false};
			const bool forceDirect = std::find(DirectTargetRanges.begin(), DirectTargetRanges.end(), key) !=
									 DirectTargetRanges.end();
			const auto found = std::find_if(composites.begin(), composites.end(), [&](const auto &entry) {
				return entry.Range.Collector == range.Collector && entry.Range.First == range.First &&
					   entry.Range.Count == range.Count && entry.Range.Spatial == range.Spatial;
			});
			if (!forceDirect && found != composites.end()) {
				RecordComposite(commandBuffer, renderPass, found->Target, {{0.0f, 0.0f}, Canvas});
			} else {
				RecordScreenRange(commandBuffer, renderPass, range.First, range.Count, nullptr);
			}
		}
		if (Pending.CollectorRanges.empty()) {
			RecordScreenRange(commandBuffer, renderPass, 0, Pending.Commands.size(), nullptr);
		}
	}

	uint32_t InterfacePass::RecordWorld(
		void *commandBuffer,
		void *renderPass,
		const glm::mat4 &viewProjection,
		const core::CFrame &camera,
		const core::Color3 &ambient,
		const core::Vector3 &sun,
		uint32_t width,
		uint32_t height,
		bool alwaysOnTop,
		WorldColourTarget target
	) {
		return RecordWorldRange(
			commandBuffer,
			renderPass,
			viewProjection,
			camera,
			ambient,
			sun,
			width,
			height,
			alwaysOnTop,
			target,
			0,
			Mesh.Batches().size(),
			false
		);
	}

	bool InterfacePass::SupportsWorldLayers() const {
		return HdrCapturePipeline != nullptr;
	}
	bool InterfacePass::HasWorldOverlay() const {
		return std::any_of(SpatialCollectors.begin(), SpatialCollectors.end(), [](const auto &placed) {
			return placed.Canvas.Visible && placed.Canvas.AlwaysOnTop;
		});
	}

	uint32_t InterfacePass::RecordWorldBatch(const WorldInterfaceCapture &capture, size_t batch) {
		return RecordWorldRange(
			capture.Command,
			capture.Pass,
			capture.ViewProjection,
			capture.Camera,
			capture.Ambient,
			capture.Sun,
			capture.Width,
			capture.Height,
			false,
			WorldColourTarget::Hdr,
			batch,
			1,
			true
		);
	}

	uint32_t InterfacePass::RecordWorldRange(
		void *commandBuffer,
		void *renderPass,
		const glm::mat4 &viewProjection,
		const core::CFrame &camera,
		const core::Color3 &ambient,
		const core::Vector3 &sun,
		uint32_t width,
		uint32_t height,
		bool alwaysOnTop,
		WorldColourTarget target,
		size_t firstBatch,
		size_t batchCount,
		bool captureDepth
	) {
		auto *command = static_cast<SDL_GPUCommandBuffer *>(commandBuffer);
		auto *pass = static_cast<SDL_GPURenderPass *>(renderPass);
		void *spatialPipeline = target == WorldColourTarget::Hdr ? HdrSpatialPipeline : SpatialPipeline;
		void *spatialTopPipeline =
			target == WorldColourTarget::Hdr ? HdrSpatialTopPipeline : SpatialTopPipeline;
		if (command == nullptr || pass == nullptr || spatialPipeline == nullptr ||
			spatialTopPipeline == nullptr || SpatialCollectors.empty()) {
			return 0;
		}

		SDL_GPUBufferBinding vertex{};
		vertex.buffer = static_cast<SDL_GPUBuffer *>(VertexBuffer);
		SDL_BindGPUVertexBuffers(pass, 0, &vertex, 1);

		SDL_GPUBufferBinding index{};
		index.buffer = static_cast<SDL_GPUBuffer *>(IndexBuffer);
		SDL_BindGPUIndexBuffer(pass, &index, SDL_GPU_INDEXELEMENTSIZE_16BIT);

		const SDL_Rect whole{0, 0, static_cast<int>(width), static_cast<int>(height)};
		SDL_SetGPUScissor(pass, &whole);

		auto *atlas = static_cast<SDL_GPUTexture *>(AtlasTexture);
		SDL_GPUTexture *bound = nullptr;
		SDL_GPUSampler *boundSampler = nullptr;
		void *boundPipeline = nullptr;
		uint32_t drawn = 0;

		const auto &batches = Mesh.Batches();
		if (firstBatch >= batches.size()) return 0;
		const auto endBatch = firstBatch + std::min(batchCount, batches.size() - firstBatch);
		for (size_t batchIndex = firstBatch; batchIndex < endBatch; ++batchIndex) {
			const auto &batch = batches[batchIndex];
			const auto placed = std::find_if(
				SpatialCollectors.begin(), SpatialCollectors.end(), [&](const SpatialCollector &entry) {
					return entry.Collector == batch.Collector;
				}
			);
			if (placed == SpatialCollectors.end() || !placed->Canvas.Visible ||
				placed->Canvas.AlwaysOnTop != alwaysOnTop || placed->Canvas.Size.X <= 0.0f ||
				placed->Canvas.Size.Y <= 0.0f) {
				continue;
			}

			const gui::SpatialCanvas &spatial = placed->Canvas;
			const auto transform = std::find_if(
				Pending.Transforms.begin(),
				Pending.Transforms.end(),
				[&](const gui::CollectorTransform &entry) { return entry.Collector == batch.Collector; }
			);
			const core::Vector2 transformOrigin =
				transform != Pending.Transforms.end() ? transform->Origin : core::Vector2::Zero;
			const core::Vector2 transformScale =
				transform != Pending.Transforms.end() ? transform->Scale : core::Vector2{1.0f, 1.0f};
			core::Vector3 origin = spatial.Origin;
			core::Vector3 axisX = spatial.AxisX;
			core::Vector3 axisY = spatial.AxisY;
			core::Vector3 normal = spatial.Normal;
			const core::Vector3 toCamera = camera.Position - spatial.Origin;
			const float distance = toCamera.Magnitude();

			if (spatial.MaxDistance > 0.0f && distance > spatial.MaxDistance) {
				continue;
			}

			if (spatial.Kind == gui::SpatialCanvasKind::Surface) {
				if (!CanvasFacesViewer(normal, toCamera)) {
					continue;
				}
			} else {
				// **The billboard's quad, worked out where a test can reach it.**
				// The pixels-per-stud measurement, the studs-plus-pixels size and
				// the centring on the anchor were all inline here until v0.19,
				// which is the gap `docs/ARCH_REVIEW.md` B recorded and
				// `src/Primitives.hpp` closes. `tests/Primitives.cpp` holds the
				// two facts that were never asserted: the quad is centred on its
				// anchor, and its normal is *not* the cross product of its axes.
				const float canvasHeight = Canvas.Y > 0.0f ? Canvas.Y : static_cast<float>(height);
				const SpatialQuad quad = BillboardQuad(
					spatial.Origin,
					camera.RightVector(),
					camera.UpVector(),
					toCamera,
					spatial.BillboardStuds,
					spatial.BillboardPixels,
					CanvasPixelsPerStud(viewProjection, spatial.Origin, camera.UpVector(), canvasHeight),
					camera.LookVector() * -1.0f
				);
				origin = quad.Origin;
				axisX = quad.AxisX;
				axisY = quad.AxisY;
				normal = quad.Normal;
			}

			const void *wantedPipeline = captureDepth
											 ? HdrCapturePipeline
											 : (spatial.AlwaysOnTop ? spatialTopPipeline : spatialPipeline);
			if (batch.Shader.IsValid()) {
				const auto shader = ShaderVariants.find(ShaderKey(batch.Shader, ContentOwner));
				if (shader != ShaderVariants.end()) {
					const auto &variant = shader->second;
					wantedPipeline = captureDepth ? variant.HdrCapture
									 : target == WorldColourTarget::Hdr
										 ? (spatial.AlwaysOnTop ? variant.HdrSpatialTop : variant.HdrSpatial)
										 : (spatial.AlwaysOnTop ? variant.SpatialTop : variant.Spatial);
				}
			}
			if (boundPipeline != wantedPipeline) {
				SDL_BindGPUGraphicsPipeline(
					pass, static_cast<SDL_GPUGraphicsPipeline *>(const_cast<void *>(wantedPipeline))
				);
				boundPipeline = const_cast<void *>(wantedPipeline);
			}

			const core::Vector3 light =
				sun.MagnitudeSquared() > 0.0f ? sun.Unit() * -1.0f : core::Vector3::YAxis;
			const float direct = std::max(normal.Dot(light), 0.0f);
			const float bounce = std::max(normal.Y, 0.0f) * 0.15f;
			const float influence = std::clamp(spatial.LightInfluence, 0.0f, 1.0f);
			const float fullbright = spatial.Brightness * (1.0f - influence);
			const float scene = influence * (direct + bounce);

			struct SpatialUniforms {
				glm::mat4 ViewProjection;
				glm::vec4 Origin;
				glm::vec4 AxisX;
				glm::vec4 AxisY;
				glm::vec4 Tint;
				glm::vec4 Canvas;
				glm::vec4 Transform;
			};

			const SpatialUniforms uniforms{
				viewProjection,
				glm::vec4{origin.X, origin.Y, origin.Z, 1.0f},
				glm::vec4{axisX.X, axisX.Y, axisX.Z, 0.0f},
				glm::vec4{axisY.X, axisY.Y, axisY.Z, 0.0f},
				glm::vec4{
					fullbright + influence * ambient.R + scene,
					fullbright + influence * ambient.G + scene,
					fullbright + influence * ambient.B + scene,
					1.0f,
				},
				glm::vec4{spatial.Size.X, spatial.Size.Y, 0.0f, 0.0f},
				glm::vec4{transformOrigin.X, transformOrigin.Y, transformScale.X, transformScale.Y},
			};
			SDL_PushGPUVertexUniformData(command, 0, &uniforms, sizeof(uniforms));

			SDL_GPUTexture *texture = atlas;
			if (batch.ShapedPage != UINT16_MAX && batch.ShapedPage < ShapedAtlasTextures.size()) {
				texture = static_cast<SDL_GPUTexture *>(ShapedAtlasTextures[batch.ShapedPage]);
			}
			if (batch.Image.IsValid() || batch.Viewport != ecs::NULL_ENTITY) {
				const auto found = std::find_if(
					ResolvedImages.begin(), ResolvedImages.end(), [&](const ResolvedImage &entry) {
						return batch.Viewport != ecs::NULL_ENTITY
								   ? entry.Viewport == batch.Viewport
								   : entry.Viewport == ecs::NULL_ENTITY && entry.Name == batch.Image;
					}
				);
				if (found != ResolvedImages.end() && found->Value.Texture != nullptr) {
					texture = static_cast<SDL_GPUTexture *>(found->Value.Texture);
				}
			}

			if (texture == nullptr) {
				continue;
			}
			auto *sampler = static_cast<SDL_GPUSampler *>(
				batch.Resample == gui::ResampleMode::Pixelated && PixelSampler != nullptr ? PixelSampler
																						  : Sampler
			);

			if (texture != bound || sampler != boundSampler) {
				const SDL_GPUTextureSamplerBinding binding{texture, sampler};
				SDL_BindGPUFragmentSamplers(pass, 0, &binding, 1);
				bound = texture;
				boundSampler = sampler;
			}

			const FragmentUniforms fragmentUniforms = FragmentUniformsFor(batch);
			SDL_PushGPUFragmentUniformData(command, 0, &fragmentUniforms, sizeof(fragmentUniforms));
			SDL_DrawGPUIndexedPrimitives(pass, batch.IndexCount, 1, batch.FirstIndex, 0, 0);
			drawn++;
		}

		return drawn;
	}
}
