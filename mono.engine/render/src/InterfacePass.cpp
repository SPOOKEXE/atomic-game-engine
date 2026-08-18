#include "ShaderBinary.hpp"

#include <engine/core/Log.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/render/InterfacePass.hpp>
#include <engine/resources/Shaders.hpp>

#include <SDL3/SDL_gpu.h>

#include <algorithm>
#include <cstring>
#include <fstream>
#include <vector>

namespace engine::render {

	namespace {
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

		SDL_GPUShader *vertex = Load(gpu, "interface.vert", SDL_GPU_SHADERSTAGE_VERTEX, 0, 1);
		SDL_GPUShader *spatialVertex = Load(gpu, "interface_spatial.vert", SDL_GPU_SHADERSTAGE_VERTEX, 0, 1);
		SDL_GPUShader *fragment = Load(gpu, "interface.frag", SDL_GPU_SHADERSTAGE_FRAGMENT, 1, 0);
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

		SDL_ReleaseGPUShader(gpu, vertex);
		SDL_ReleaseGPUShader(gpu, spatialVertex);
		SDL_ReleaseGPUShader(gpu, fragment);

		if (Pipeline == nullptr || SpatialPipeline == nullptr || SpatialTopPipeline == nullptr) {
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

			AtlasTexture = SDL_CreateGPUTexture(gpu, &texture);
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
			SDL_ReleaseGPUTransferBuffer(gpu, static_cast<SDL_GPUTransferBuffer *>(AtlasTransferBuffer));
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
			SDL_ReleaseGPUTransferBuffer(gpu, static_cast<SDL_GPUTransferBuffer *>(TransferBuffer));
		}
		if (AtlasTransferBuffer != nullptr) {
			SDL_ReleaseGPUTransferBuffer(gpu, static_cast<SDL_GPUTransferBuffer *>(AtlasTransferBuffer));
		}
		if (IndexBuffer != nullptr) {
			SDL_ReleaseGPUBuffer(gpu, static_cast<SDL_GPUBuffer *>(IndexBuffer));
		}
		if (VertexBuffer != nullptr) {
			SDL_ReleaseGPUBuffer(gpu, static_cast<SDL_GPUBuffer *>(VertexBuffer));
		}
		if (AtlasTexture != nullptr) {
			SDL_ReleaseGPUTexture(gpu, static_cast<SDL_GPUTexture *>(AtlasTexture));
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
		for (const auto &[id, pipeline] : ShaderVariants) {
			SDL_ReleaseGPUGraphicsPipeline(gpu, static_cast<SDL_GPUGraphicsPipeline *>(pipeline));
		}
		ShaderVariants.clear();

		TransferBuffer = nullptr;
		IndexBuffer = nullptr;
		VertexBuffer = nullptr;
		AtlasTexture = nullptr;
		AtlasTransferBuffer = nullptr;
		Sampler = nullptr;
		Pipeline = nullptr;
		SpatialPipeline = nullptr;
		SpatialTopPipeline = nullptr;
		Device = nullptr;
		SwapchainFormat = 0;

		VertexCapacity = 0;
		IndexCapacity = 0;
		TransferCapacity = 0;
		AtlasUploaded = false;
	}

	void InterfacePass::Submit(const gui::DrawList &list, const core::Vector2 &canvas, ecs::Store &store) {
		Pending = list;
		Canvas = canvas;
		SpatialCollectors.clear();
		store.Each<const gui::SpatialCanvas>([&](ecs::Entity collector, const gui::SpatialCanvas &spatial) {
			SpatialCollectors.push_back(SpatialCollector{collector, spatial});
		});
	}

	bool InterfacePass::AddShaderVariant(const core::Name &name, std::span<const uint32_t> spirv) {
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
				ENGINE_ERROR("interface shader '{}' cannot be translated to MSL: {}", name.Text(), result.Error);
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

		// **One sampler and no uniform buffer - `interface.frag`'s own
		// shape**, matching exactly what `Initialise` declares for it below,
		// and not `opaque.frag`'s nine and three. This is the contract a
		// `ShaderScript` meant for an `ImageLabel` is written against; see
		// this method's own header. The clip rectangle `Record` pushes
		// reaches the fragment stage through SDL's push-constant path rather
		// than a bound buffer, which is why it does not appear here.
		fragmentInfo.num_samplers = 1;
		fragmentInfo.num_uniform_buffers = 0;

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

		SDL_GPUGraphicsPipeline *pipeline = SDL_CreateGPUGraphicsPipeline(gpu, &info);

		SDL_ReleaseGPUShader(gpu, vertex);
		SDL_ReleaseGPUShader(gpu, fragment);

		if (pipeline == nullptr) {
			ENGINE_ERROR("interface shader '{}' pipeline: {}", name.Text(), SDL_GetError());
			return false;
		}

		// Replacing is the ordinary case: an author editing a `ShaderScript`
		// bumps its revision every keystroke that lands.
		DropShaderVariant(name);
		ShaderVariants[name.Id()] = pipeline;
		return true;
	}

	bool InterfacePass::DropShaderVariant(const core::Name &name) {
		const auto found = ShaderVariants.find(name.Id());
		if (found == ShaderVariants.end()) {
			return false;
		}
		if (Device != nullptr) {
			SDL_ReleaseGPUGraphicsPipeline(
				static_cast<SDL_GPUDevice *>(Device), static_cast<SDL_GPUGraphicsPipeline *>(found->second)
			);
		}
		ShaderVariants.erase(found);
		return true;
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

		SDL_GPUTransferBuffer *staging = SDL_CreateGPUTransferBuffer(gpu, &info);
		if (staging == nullptr) {
			return false;
		}
		AtlasTransferBuffer = staging;

		void *mapped = SDL_MapGPUTransferBuffer(gpu, staging, false);
		if (mapped == nullptr) {
			SDL_ReleaseGPUTransferBuffer(gpu, staging);
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

	bool InterfacePass::Prepare(void *commandBuffer) {
		if (Pipeline == nullptr || commandBuffer == nullptr) {
			return false;
		}

		auto *gpu = static_cast<SDL_GPUDevice *>(Device);

		UploadAtlas(commandBuffer);

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
			}
		);
		Recorded = Mesh.Batches().size();

		const auto vertices = static_cast<uint32_t>(Mesh.Vertices().size());
		const auto indices = static_cast<uint32_t>(Mesh.Indices().size());
		if (vertices == 0 || indices == 0) {
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
				SDL_ReleaseGPUBuffer(gpu, static_cast<SDL_GPUBuffer *>(VertexBuffer));
			}
			SDL_GPUBufferCreateInfo info{};
			info.usage = SDL_GPU_BUFFERUSAGE_VERTEX;
			info.size = vertexBytes;
			VertexBuffer = SDL_CreateGPUBuffer(gpu, &info);
			VertexCapacity = VertexBuffer != nullptr ? vertexBytes : 0;
		}

		if (IndexBuffer == nullptr || IndexCapacity < indexBytes) {
			if (IndexBuffer != nullptr) {
				SDL_ReleaseGPUBuffer(gpu, static_cast<SDL_GPUBuffer *>(IndexBuffer));
			}
			SDL_GPUBufferCreateInfo info{};
			info.usage = SDL_GPU_BUFFERUSAGE_INDEX;
			info.size = indexBytes;
			IndexBuffer = SDL_CreateGPUBuffer(gpu, &info);
			IndexCapacity = IndexBuffer != nullptr ? indexBytes : 0;
		}

		if (VertexBuffer == nullptr || IndexBuffer == nullptr) {
			return false;
		}

		const uint32_t total = vertexBytes + indexBytes;
		if (TransferBuffer == nullptr || TransferCapacity < total) {
			if (TransferBuffer != nullptr) {
				SDL_ReleaseGPUTransferBuffer(gpu, static_cast<SDL_GPUTransferBuffer *>(TransferBuffer));
			}
			SDL_GPUTransferBufferCreateInfo info{};
			info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
			info.size = total;
			TransferBuffer = SDL_CreateGPUTransferBuffer(gpu, &info);
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
		return true;
	}

	void InterfacePass::Record(void *commandBuffer, void *renderPass) {
		auto *command = static_cast<SDL_GPUCommandBuffer *>(commandBuffer);
		auto *pass = static_cast<SDL_GPURenderPass *>(renderPass);
		if (command == nullptr || pass == nullptr || Pipeline == nullptr) {
			return;
		}

		auto *defaultPipeline = static_cast<SDL_GPUGraphicsPipeline *>(Pipeline);
		SDL_BindGPUGraphicsPipeline(pass, defaultPipeline);
		SDL_GPUGraphicsPipeline *boundPipeline = defaultPipeline;

		// The canvas, for the vertex shader's one divide. Pushed rather than
		// held in a buffer because it is two floats and changes with the window.
		const float canvas[2] = {
			Canvas.X > 0.0f ? Canvas.X : 1.0f,
			Canvas.Y > 0.0f ? Canvas.Y : 1.0f,
		};
		SDL_PushGPUVertexUniformData(command, 0, canvas, sizeof(canvas));

		SDL_GPUBufferBinding vertex{};
		vertex.buffer = static_cast<SDL_GPUBuffer *>(VertexBuffer);
		SDL_BindGPUVertexBuffers(pass, 0, &vertex, 1);

		SDL_GPUBufferBinding index{};
		index.buffer = static_cast<SDL_GPUBuffer *>(IndexBuffer);
		SDL_BindGPUIndexBuffer(pass, &index, SDL_GPU_INDEXELEMENTSIZE_16BIT);

		auto *atlas = static_cast<SDL_GPUTexture *>(AtlasTexture);
		auto *sampler = static_cast<SDL_GPUSampler *>(Sampler);

		// **Rebound per batch only when it changes.** A bind is cheap and a
		// redundant one is not free; tracking the last one is four lines against
		// a bind per quad.
		SDL_GPUTexture *bound = nullptr;

		for (const InterfaceBatch &batch : Mesh.Batches()) {
			const bool spatial = std::any_of(
				SpatialCollectors.begin(), SpatialCollectors.end(), [&](const SpatialCollector &entry) {
					return entry.Collector == batch.Collector;
				}
			);
			if (spatial) {
				continue;
			}

			// **Bound per batch only when it changes**, `bound`'s own reason
			// applied to a pipeline instead of a texture. A batch naming no
			// shader - the ordinary case - or one whose variant has not
			// built yet draws with the pass's own shader rather than
			// nothing: the same "keep the last frame's" the codebase reaches
			// for whenever a resolve can fail.
			SDL_GPUGraphicsPipeline *wanted = defaultPipeline;
			if (batch.Shader.IsValid()) {
				const auto found = ShaderVariants.find(batch.Shader.Id());
				if (found != ShaderVariants.end()) {
					wanted = static_cast<SDL_GPUGraphicsPipeline *>(found->second);
				}
			}
			if (wanted != boundPipeline) {
				SDL_BindGPUGraphicsPipeline(pass, wanted);
				boundPipeline = wanted;
			}

			SDL_GPUTexture *texture = atlas;

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

			if (texture != bound) {
				SDL_GPUTextureSamplerBinding binding{};
				binding.texture = texture;
				binding.sampler = sampler;
				SDL_BindGPUFragmentSamplers(pass, 0, &binding, 1);
				bound = texture;
			}

			const float clip[4] = {
				batch.Clip.Min.X,
				batch.Clip.Min.Y,
				batch.Clip.Max.X,
				batch.Clip.Max.Y,
			};
			SDL_PushGPUFragmentUniformData(
				static_cast<SDL_GPUCommandBuffer *>(commandBuffer), 0, clip, sizeof(clip)
			);

			// The scissor, in pixels, clamped to the target - a negative origin
			// or a width past the edge is a validation error on some backends
			// and silently ignored on others, which is the worst pair.
			SDL_Rect scissor{};
			scissor.x = static_cast<int>(std::max(0.0f, batch.Clip.Min.X));
			scissor.y = static_cast<int>(std::max(0.0f, batch.Clip.Min.Y));
			scissor.w = static_cast<int>(std::max(0.0f, batch.Clip.Max.X - batch.Clip.Min.X));
			scissor.h = static_cast<int>(std::max(0.0f, batch.Clip.Max.Y - batch.Clip.Min.Y));
			if (scissor.w <= 0 || scissor.h <= 0) {
				continue;
			}
			SDL_SetGPUScissor(pass, &scissor);

			SDL_DrawGPUIndexedPrimitives(pass, batch.IndexCount, 1, batch.FirstIndex, 0, 0);
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
		bool alwaysOnTop
	) {
		auto *command = static_cast<SDL_GPUCommandBuffer *>(commandBuffer);
		auto *pass = static_cast<SDL_GPURenderPass *>(renderPass);
		if (command == nullptr || pass == nullptr || SpatialPipeline == nullptr ||
			SpatialTopPipeline == nullptr || SpatialCollectors.empty()) {
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
		auto *sampler = static_cast<SDL_GPUSampler *>(Sampler);
		SDL_GPUTexture *bound = nullptr;
		void *boundPipeline = nullptr;
		uint32_t drawn = 0;

		for (const InterfaceBatch &batch : Mesh.Batches()) {
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
				if (normal.Dot(toCamera) <= 0.0f) {
					continue;
				}
			} else {
				const core::Vector3 right = camera.RightVector();
				const core::Vector3 up = camera.UpVector();
				const glm::vec4 projectedAnchor =
					viewProjection * glm::vec4{spatial.Origin.X, spatial.Origin.Y, spatial.Origin.Z, 1.0f};
				const core::Vector3 oneUp = spatial.Origin + up;
				const glm::vec4 projectedUp = viewProjection * glm::vec4{oneUp.X, oneUp.Y, oneUp.Z, 1.0f};
				const float pixelsPerStud =
					std::abs(
						projectedUp.y / std::max(std::abs(projectedUp.w), 1.0e-5f) -
						projectedAnchor.y / std::max(std::abs(projectedAnchor.w), 1.0e-5f)
					) *
					static_cast<float>(height) * 0.5f;
				const float usablePixelsPerStud = std::max(pixelsPerStud, 1.0e-5f);
				const core::Vector2 worldSize{
					spatial.BillboardStuds.X + spatial.BillboardPixels.X / usablePixelsPerStud,
					spatial.BillboardStuds.Y + spatial.BillboardPixels.Y / usablePixelsPerStud,
				};
				axisX = right * worldSize.X;
				axisY = up * -worldSize.Y;
				origin = spatial.Origin - axisX * 0.5f - axisY * 0.5f;
				normal = distance > 0.0f ? toCamera / distance : camera.LookVector() * -1.0f;
			}

			const void *wantedPipeline = spatial.AlwaysOnTop ? SpatialTopPipeline : SpatialPipeline;
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
			};
			SDL_PushGPUVertexUniformData(command, 0, &uniforms, sizeof(uniforms));

			SDL_GPUTexture *texture = atlas;
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
			if (texture != bound) {
				const SDL_GPUTextureSamplerBinding binding{texture, sampler};
				SDL_BindGPUFragmentSamplers(pass, 0, &binding, 1);
				bound = texture;
			}

			const float clip[4]{
				batch.Clip.Min.X,
				batch.Clip.Min.Y,
				batch.Clip.Max.X,
				batch.Clip.Max.Y,
			};
			SDL_PushGPUFragmentUniformData(command, 0, clip, sizeof(clip));
			SDL_DrawGPUIndexedPrimitives(pass, batch.IndexCount, 1, batch.FirstIndex, 0, 0);
			drawn++;
		}

		return drawn;
	}
}
