#include <engine/core/Log.hpp>
#include <engine/core/Paths.hpp>
#include <engine/render/InterfacePass.hpp>

#include <SDL3/SDL_gpu.h>

#include <algorithm>
#include <cstring>
#include <fstream>
#include <vector>

namespace engine::render {

	namespace {
		std::vector<uint8_t> ReadShader(std::string_view name) {
			const auto path = core::Paths::Shaders("render") / (std::string(name) + ".spv");
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
			const std::vector<uint8_t> code = ReadShader(name);
			if (code.empty()) {
				return nullptr;
			}

			SDL_GPUShaderCreateInfo info{};
			info.code = code.data();
			info.code_size = code.size();
			info.entrypoint = "main";
			info.format = SDL_GPU_SHADERFORMAT_SPIRV;
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

		auto *gpu = static_cast<SDL_GPUDevice *>(device);
		if (gpu == nullptr) {
			return false;
		}

		// **The atlas first, because a pipeline with nothing to sample is a
		// pipeline that draws nothing.** A failure here is not fatal: the
		// interface still draws its rectangles and images, and the missing text
		// is visible as missing — `GlyphAtlas::Build` says why.
		Glyphs.Build(pixelSize);

		SDL_GPUShader *vertex = Load(gpu, "interface.vert", SDL_GPU_SHADERSTAGE_VERTEX, 0, 1);
		SDL_GPUShader *fragment = Load(gpu, "interface.frag", SDL_GPU_SHADERSTAGE_FRAGMENT, 1, 0);
		if (vertex == nullptr || fragment == nullptr) {
			if (vertex != nullptr) {
				SDL_ReleaseGPUShader(gpu, vertex);
			}
			if (fragment != nullptr) {
				SDL_ReleaseGPUShader(gpu, fragment);
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
		// behind a wall — and a quad wound the other way is invisible under
		// culling, which looks exactly like an element that failed to lay out.
		info.target_info.has_depth_stencil_target = false;
		info.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;

		Pipeline = SDL_CreateGPUGraphicsPipeline(gpu, &info);

		SDL_ReleaseGPUShader(gpu, vertex);
		SDL_ReleaseGPUShader(gpu, fragment);

		if (Pipeline == nullptr) {
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
		// border rather than wrapping to the far side of the sheet — which
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

			// **One channel, because the atlas is coverage and not colour.**
			// Four would be three bytes of 255 per texel for a sheet that is
			// megabytes.
			texture.format = SDL_GPU_TEXTUREFORMAT_R8_UNORM;
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

		TransferBuffer = nullptr;
		IndexBuffer = nullptr;
		VertexBuffer = nullptr;
		AtlasTexture = nullptr;
		Sampler = nullptr;
		Pipeline = nullptr;
		Device = nullptr;

		VertexCapacity = 0;
		IndexCapacity = 0;
		TransferCapacity = 0;
		AtlasUploaded = false;
	}

	void InterfacePass::Submit(const gui::DrawList &list, const core::Vector2 &canvas) {
		Pending = list;
		Canvas = canvas;
	}

	bool InterfacePass::UploadAtlas(void *commandBuffer) {
		if (AtlasUploaded || AtlasTexture == nullptr || !Glyphs.Ready()) {
			return AtlasUploaded;
		}

		auto *gpu = static_cast<SDL_GPUDevice *>(Device);
		const std::vector<uint8_t> &coverage = Glyphs.Coverage();

		SDL_GPUTransferBufferCreateInfo info{};
		info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
		info.size = static_cast<uint32_t>(coverage.size());

		SDL_GPUTransferBuffer *staging = SDL_CreateGPUTransferBuffer(gpu, &info);
		if (staging == nullptr) {
			return false;
		}

		if (void *mapped = SDL_MapGPUTransferBuffer(gpu, staging, false)) {
			std::memcpy(mapped, coverage.data(), coverage.size());
			SDL_UnmapGPUTransferBuffer(gpu, staging);
		}

		SDL_GPUCopyPass *copy = SDL_BeginGPUCopyPass(static_cast<SDL_GPUCommandBuffer *>(commandBuffer));

		SDL_GPUTextureTransferInfo source{};
		source.transfer_buffer = staging;
		source.offset = 0;

		SDL_GPUTextureRegion region{};
		region.texture = static_cast<SDL_GPUTexture *>(AtlasTexture);
		region.w = Glyphs.Width();
		region.h = Glyphs.Height();
		region.d = 1;

		SDL_UploadToGPUTexture(copy, &source, &region, false);
		SDL_EndGPUCopyPass(copy);

		// **Released immediately rather than kept.** The atlas is uploaded once
		// for the life of the pass, so a staging buffer held for a second upload
		// that never comes is a megabyte of nothing. SDL defers the free until
		// the copy has retired.
		SDL_ReleaseGPUTransferBuffer(gpu, staging);

		AtlasUploaded = true;
		return true;
	}

	bool InterfacePass::Prepare(void *commandBuffer) {
		if (Pipeline == nullptr || commandBuffer == nullptr) {
			return false;
		}

		auto *gpu = static_cast<SDL_GPUDevice *>(Device);

		UploadAtlas(commandBuffer);

		Mesh.Build(Pending, Glyphs);
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
		// again — and a buffer reallocation mid-frame is a stall the panel gets
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
		// positions — a tearing that only appears under load.
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

		SDL_BindGPUGraphicsPipeline(pass, static_cast<SDL_GPUGraphicsPipeline *>(Pipeline));

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
			SDL_GPUTexture *texture = atlas;

			// The identity, so a rectangle and a glyph — which sample the atlas
			// and are most of a frame — pay one uniform push and no arithmetic.
			FlipbookCell cell;

			if (batch.Image.IsValid() && Images) {
				if (auto *resolved = static_cast<SDL_GPUTexture *>(Images(batch.Image, cell))) {
					texture = resolved;
				} else {
					// **The cell goes back to the identity with the handle.** A
					// resolver may have filled it before deciding it had no
					// texture, and sampling the atlas's white texel through a
					// quarter-scale transform is a rectangle drawn at the wrong
					// size for a reason nothing on screen explains.
					cell = FlipbookCell{};
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

			// **Pushed per batch rather than per pass**, because the sheet is a
			// property of the texture and a frame holds several. It is sixteen
			// bytes against a bind that already happened.
			const float flipbook[4] = {cell.Scale, cell.OffsetU, cell.OffsetV, 0.0f};
			SDL_PushGPUFragmentUniformData(
				static_cast<SDL_GPUCommandBuffer *>(commandBuffer), 0, flipbook, sizeof(flipbook)
			);

			// The scissor, in pixels, clamped to the target — a negative origin
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
}
