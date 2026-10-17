#include "ImageGraphGpu.hpp"

namespace engine::render {
	bool RecordImageGraphNode(
		SDL_GPUCommandBuffer *command,
		SDL_GPUComputePipeline *pipeline,
		SDL_GPUSampler *sampler,
		SDL_GPUTexture *first,
		SDL_GPUTexture *second,
		SDL_GPUTexture *target,
		const ImageGraphGpuUniforms &uniforms
	) {
		SDL_GPUStorageTextureReadWriteBinding destination{};
		destination.texture = target;
		// The unified device queue orders earlier readers before this overwrite.
		// Cycling here would allocate new backing storage on every changed parameter.
		destination.cycle = false;
		SDL_GPUComputePass *pass = SDL_BeginGPUComputePass(command, &destination, 1, nullptr, 0);
		if (!pass) return false;
		SDL_BindGPUComputePipeline(pass, pipeline);
		const std::array bindings{
			SDL_GPUTextureSamplerBinding{first, sampler}, SDL_GPUTextureSamplerBinding{second, sampler}
		};
		SDL_BindGPUComputeSamplers(pass, 0, bindings.data(), static_cast<uint32_t>(bindings.size()));
		SDL_PushGPUComputeUniformData(command, 0, &uniforms, sizeof(uniforms));
		SDL_DispatchGPUCompute(pass, (uniforms.Extents[0] + 7u) / 8u, (uniforms.Extents[1] + 7u) / 8u, 1);
		SDL_EndGPUComputePass(pass);
		return true;
	}

	bool RecordImageGraphPublication(
		SDL_GPUCommandBuffer *command,
		SDL_GPUGraphicsPipeline *pipeline,
		SDL_GPUSampler *sampler,
		SDL_GPUTexture *source,
		SDL_GPUTexture *target,
		imagegraph::OutputSpace space
	) {
		SDL_GPUColorTargetInfo attachment{};
		attachment.texture = target;
		attachment.load_op = SDL_GPU_LOADOP_DONT_CARE;
		attachment.store_op = SDL_GPU_STOREOP_STORE;
		SDL_GPURenderPass *pass = SDL_BeginGPURenderPass(command, &attachment, 1, nullptr);
		if (!pass) return false;
		SDL_BindGPUGraphicsPipeline(pass, pipeline);
		const SDL_GPUTextureSamplerBinding binding{source, sampler};
		SDL_BindGPUFragmentSamplers(pass, 0, &binding, 1);
		const std::array<uint32_t, 4> controls{space == imagegraph::OutputSpace::SRGB ? 1u : 0u, 0, 0, 0};
		SDL_PushGPUFragmentUniformData(command, 0, controls.data(), sizeof(controls));
		SDL_DrawGPUPrimitives(pass, 3, 1, 0, 0);
		SDL_EndGPURenderPass(pass);
		return true;
	}
}
