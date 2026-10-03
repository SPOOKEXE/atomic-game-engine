#include "ComposerNodes.hpp"

#include "GpuHeap.hpp"
#include "ImageGraphGpuHelpers.hpp"

#include <engine/core/Metrics.hpp>
#include <engine/render/GraphRunner.hpp>

#include <algorithm>
#include <array>
#include <cstring>
#include <new>

namespace engine::render::hlsl {
	namespace {
		struct SurfaceVertex {
			float X, Y, Z, U, V;
		};
		constexpr std::array<SurfaceVertex, 4> VERTICES{
			{{0, 0, 0, 0, 0}, {1, 0, 0, 1, 0}, {0, 1, 0, 0, 1}, {1, 1, 0, 1, 1}}
		};
		SDL_GPUShader *CreateCookedStage(
			SDL_GPUDevice *device,
			const CookedPair &pair,
			bool fragment,
			bool metal,
			imagegraph::SourceCamera3DResources &resources,
			uint32_t samplers
		) {
			SDL_GPUShaderCreateInfo info{};
			const auto &spirv = fragment ? pair.SpirV.Fragment.SpirV : pair.SpirV.Vertex.SpirV;
			const auto &msl = fragment ? pair.FragmentMsl : pair.VertexMsl;
			info.code = metal ? reinterpret_cast<const Uint8 *>(msl.data())
							  : reinterpret_cast<const Uint8 *>(spirv.data());
			info.code_size = metal ? msl.size() : spirv.size() * sizeof(uint32_t);
			info.entrypoint = metal ? "main0" : "main";
			info.format = metal ? SDL_GPU_SHADERFORMAT_MSL : SDL_GPU_SHADERFORMAT_SPIRV;
			info.stage = fragment ? SDL_GPU_SHADERSTAGE_FRAGMENT : SDL_GPU_SHADERSTAGE_VERTEX;
			info.num_uniform_buffers = fragment ? uint32_t(pair.SpirV.UniformBytes != 0) : 1;
			info.num_samplers = fragment ? samplers : 0;
			auto *shader = SDL_CreateGPUShader(device, &info);
			if (shader) resources.Shaders.push_back(shader);
			return shader;
		}
	} // namespace
	bool RecordComposerSurface(
		SDL_GPUDevice *device,
		SDL_GPUCommandBuffer *command,
		const SurfaceRequest &request,
		imagegraph::SourceCamera3DResources &resources,
		SDL_GPUTransferBuffer *&displayDownload,
		bool captureReadback
	) try {
		if (!device || !command || ValidateSurfaceRequest(request)) return false;
		SurfaceJob job;
		if (BuildSurfaceJob(request, job, captureReadback)) return false;
		const auto formats = SDL_GetGPUShaderFormats(device);
		const bool metal = !(formats & SDL_GPU_SHADERFORMAT_SPIRV);
		if (metal && (!(formats & SDL_GPU_SHADERFORMAT_MSL) || request.Pair.VertexMsl.empty() ||
					  request.Pair.FragmentMsl.empty()))
			return false;
		std::vector<SamplerBinding> mappings;
		if (SamplerBindings(request.Pair.SpirV, metal ? "msl" : "spirv", mappings)) return false;
		uint32_t count = 0;
		for (const auto &binding : mappings)
			count = std::max(count, binding.BackendSlot + 1);
		// SPIR-V inactive declared slots remain supplied; Metal binds its compact
		// active table.
		if (!metal) count = request.Pair.SpirV.SamplerCount;
		resources.Textures.reserve(request.Textures.size() + 2);
		resources.Transfers.reserve(request.Textures.size() + 3);
		resources.Buffers.reserve(1);
		resources.Shaders.reserve(2);
		resources.Pipelines.reserve(1);
		resources.Samplers.reserve(1);
		std::array<SDL_GPUTexture *, MAXIMUM_SAMPLERS> textures{};
		SDL_GPUBuffer *vertices = nullptr;
		NodeTable table;
		table.Set(core::Name("composer-upload"), [&](const graph::RunContext &context) {
			if (context.Writes.size() != request.Textures.size()) return false;
			for (size_t slot = 0; slot < request.Textures.size(); ++slot) {
				textures[slot] =
					imagegraph::gpu_helpers::Upload(device, command, &request.Textures[slot], {}, resources);
				if (!textures[slot]) return false;
			}
			SDL_GPUBufferCreateInfo buffer{};
			buffer.usage = SDL_GPU_BUFFERUSAGE_VERTEX;
			buffer.size = sizeof(VERTICES);
			vertices = gpu::CreateBuffer(device, &buffer);
			if (!vertices) return false;
			resources.Buffers.push_back(vertices);
			auto *transfer = imagegraph::gpu_helpers::Transfer(
				device, sizeof(VERTICES), SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD, resources
			);
			if (!transfer) return false;
			void *mapped = SDL_MapGPUTransferBuffer(device, transfer, false);
			if (!mapped) return false;
			std::memcpy(mapped, VERTICES.data(), sizeof(VERTICES));
			SDL_UnmapGPUTransferBuffer(device, transfer);
			auto *copy = SDL_BeginGPUCopyPass(command);
			if (!copy) return false;
			SDL_GPUTransferBufferLocation source{};
			source.transfer_buffer = transfer;
			SDL_GPUBufferRegion target{};
			target.buffer = vertices;
			target.size = sizeof(VERTICES);
			SDL_UploadToGPUBuffer(copy, &source, &target, false);
			SDL_EndGPUCopyPass(copy);
			resources.CommandReferenced = true;
			core::Metrics::Count("render.composer.vertex_upload_bytes", sizeof(VERTICES));
			return true;
		});
		table.Set(core::Name("composer-surface"), [&](const graph::RunContext &context) {
			if (context.Reads.size() != request.Textures.size() || context.Writes.size() != 1) return false;
			auto *vertex = CreateCookedStage(device, request.Pair, false, metal, resources, 0);
			auto *fragment = CreateCookedStage(device, request.Pair, true, metal, resources, count);
			if (!vertex || !fragment) return false;
			const auto &base = request.Textures[0];
			resources.Output = imagegraph::gpu_helpers::Texture(
				device, base.Width, base.Height, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM, resources
			);
			auto *sampler = imagegraph::gpu_helpers::Sampler(device, request.LinearSampling, resources);
			if (!resources.Output || !sampler) return false;
			SDL_GPUVertexBufferDescription stream{};
			stream.slot = 0;
			stream.pitch = sizeof(SurfaceVertex);
			stream.input_rate = SDL_GPU_VERTEXINPUTRATE_VERTEX;
			std::array<SDL_GPUVertexAttribute, 2> attributes{};
			attributes[0] = {
				.location = 0, .buffer_slot = 0, .format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3, .offset = 0
			};
			attributes[1] = {
				.location = 2,
				.buffer_slot = 0,
				.format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2,
				.offset = offsetof(SurfaceVertex, U)
			};
			SDL_GPUColorTargetDescription target{};
			target.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
			SDL_GPUGraphicsPipelineCreateInfo info{};
			info.vertex_shader = vertex;
			info.fragment_shader = fragment;
			info.vertex_input_state = {&stream, 1, attributes.data(), uint32_t(attributes.size())};
			info.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLESTRIP;
			info.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
			info.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
			info.multisample_state.sample_count = SDL_GPU_SAMPLECOUNT_1;
			info.target_info.color_target_descriptions = &target;
			info.target_info.num_color_targets = 1;
			auto *pipeline = SDL_CreateGPUGraphicsPipeline(device, &info);
			if (!pipeline) return false;
			resources.Pipelines.push_back(pipeline);
			std::array<SDL_GPUTextureSamplerBinding, MAXIMUM_SAMPLERS> bindings{};
			if (!metal)
				for (uint32_t slot = 0; slot < count; ++slot)
					bindings[slot] = {textures[slot], sampler};
			else
				for (const auto &mapping : mappings)
					bindings[mapping.BackendSlot] = {textures[mapping.SourceSlot], sampler};
			SDL_GPUColorTargetInfo attachment{};
			attachment.texture = resources.Output;
			attachment.load_op = SDL_GPU_LOADOP_CLEAR;
			attachment.store_op = SDL_GPU_STOREOP_STORE;
			auto *pass = SDL_BeginGPURenderPass(command, &attachment, 1, nullptr);
			if (!pass) return false;
			resources.CommandReferenced = true;
			SDL_BindGPUGraphicsPipeline(pass, pipeline);
			SDL_GPUBufferBinding binding{vertices, 0};
			SDL_BindGPUVertexBuffers(pass, 0, &binding, 1);
			if (count) SDL_BindGPUFragmentSamplers(pass, 0, bindings.data(), count);
			const auto matrices = VertexMatrices(request.Pair, base.Width, base.Height);
			SDL_PushGPUVertexUniformData(command, 0, matrices.data(), sizeof(matrices));
			if (!request.Uniforms.empty())
				SDL_PushGPUFragmentUniformData(command, 0, request.Uniforms.data(), request.Uniforms.size());
			SDL_DrawGPUPrimitives(pass, 4, 1, 0, 0);
			SDL_EndGPURenderPass(pass);
			core::Metrics::Count("render.composer.surface_jobs", 1);
			return true;
		});
		table.Set(core::Name("composer-display-download"), [&](const graph::RunContext &context) {
			if (context.Reads.size() != 1 || context.Writes.size() != 1 || !resources.Output) return false;
			const auto &base = request.Textures[0];
			const uint32_t bytes = uint64_t(base.Width) * base.Height * 4;
			displayDownload = imagegraph::gpu_helpers::Transfer(
				device, bytes, SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD, resources
			);
			if (!displayDownload) return false;
			auto *copy = SDL_BeginGPUCopyPass(command);
			if (!copy) return false;
			SDL_GPUTextureRegion source{};
			source.texture = resources.Output;
			source.w = base.Width;
			source.h = base.Height;
			source.d = 1;
			SDL_GPUTextureTransferInfo destination{};
			destination.transfer_buffer = displayDownload;
			destination.pixels_per_row = base.Width;
			destination.rows_per_layer = base.Height;
			SDL_DownloadFromGPUTexture(copy, &source, &destination);
			SDL_EndGPUCopyPass(copy);
			resources.CommandReferenced = true;
			core::Metrics::Count("render.composer.display_download_bytes", bytes);
			core::Metrics::Count("render.composer.display_downloads", 1);
			return true;
		});
		if (!table.Missing(job.Graph).empty()) return false;
		GraphRunner runner(table);
		return job.Graph.Execute(job.Schedule, runner, size_t(0));
	} catch (const std::bad_alloc &) {
		return false;
	}
	bool RecordComposerDisplayUpload(
		SDL_GPUDevice *device,
		SDL_GPUCommandBuffer *command,
		uint32_t width,
		uint32_t height,
		std::span<const uint8_t> pixels,
		imagegraph::SourceCamera3DResources &resources
	) try {
		if (!device || !command) return false;
		SurfaceJob job;
		if (BuildSurfaceDisplayUploadJob(width, height, pixels.size(), job)) return false;
		NodeTable table;
		table.Set(core::Name("composer-display-upload"), [&](const graph::RunContext &context) {
			if (context.Reads.size() != 1 || context.Writes.size() != 1) return false;
			auto *texture = imagegraph::gpu_helpers::Texture(
				device, width, height, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM_SRGB, resources, 1, false, true
			);
			if (!texture) return false;
			auto *upload = imagegraph::gpu_helpers::Transfer(
				device, pixels.size(), SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD, resources
			);
			if (!upload) return false;
			void *mapped = SDL_MapGPUTransferBuffer(device, upload, false);
			if (!mapped) return false;
			std::memcpy(mapped, pixels.data(), pixels.size());
			SDL_UnmapGPUTransferBuffer(device, upload);
			auto *copy = SDL_BeginGPUCopyPass(command);
			if (!copy) return false;
			SDL_GPUTextureTransferInfo source{};
			source.transfer_buffer = upload;
			source.pixels_per_row = width;
			source.rows_per_layer = height;
			SDL_GPUTextureRegion destination{};
			destination.texture = texture;
			destination.w = width;
			destination.h = height;
			destination.d = 1;
			SDL_UploadToGPUTexture(copy, &source, &destination, false);
			SDL_EndGPUCopyPass(copy);
			resources.CommandReferenced = true;
			resources.Output = texture;
			core::Metrics::Count("render.composer.display_upload_bytes", pixels.size());
			core::Metrics::Count("render.composer.display_uploads", 1);
			return true;
		});
		if (!table.Missing(job.Graph).empty()) return false;
		GraphRunner runner(table);
		return job.Graph.Execute(job.Schedule, runner, size_t(0));
	} catch (const std::bad_alloc &) {
		return false;
	}

} // namespace engine::render::hlsl
