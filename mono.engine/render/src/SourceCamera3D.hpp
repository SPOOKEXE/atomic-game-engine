#pragma once
#include <engine/render/SourceCamera3D.hpp>

#include <SDL3/SDL_gpu.h>

#include <vector>

namespace engine::render::imagegraph {
	struct SourceCamera3DResources {
		SDL_GPUTexture *Output = nullptr;
		std::array<SDL_GPUTexture *, 7> Outputs{};
		std::vector<SDL_GPUTexture *> Textures;
		std::vector<SDL_GPUBuffer *> Buffers;
		std::vector<SDL_GPUTransferBuffer *> Transfers;
		std::vector<SDL_GPUShader *> Shaders;
		std::vector<SDL_GPUGraphicsPipeline *> Pipelines;
		std::vector<SDL_GPUSampler *> Samplers;
		bool CommandReferenced = false;
	};
	uint64_t SourceCamera3DSourceBytes(const SourceCamera3DRequest &request);
	uint64_t SourceCamera3DScratchBytes(const SourceCamera3DRequest &request);
	bool RecordSourceCamera3D(
		SDL_GPUDevice *device,
		SDL_GPUCommandBuffer *command,
		const SourceCamera3DRequest &request,
		SourceCamera3DResources &resources
	);
	void ReleaseSourceCamera3D(SDL_GPUDevice *device, SourceCamera3DResources &resources);
}
