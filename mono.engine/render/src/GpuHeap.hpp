#pragma once

#include <engine/render/Renderer.hpp>

#include <SDL3/SDL_gpu.h>

namespace engine::render::gpu {
	SDL_GPUBuffer *CreateBuffer(SDL_GPUDevice *device, const SDL_GPUBufferCreateInfo *info);
	void ReleaseBuffer(SDL_GPUDevice *device, SDL_GPUBuffer *buffer);

	SDL_GPUTransferBuffer *
	CreateTransferBuffer(SDL_GPUDevice *device, const SDL_GPUTransferBufferCreateInfo *info);
	void ReleaseTransferBuffer(SDL_GPUDevice *device, SDL_GPUTransferBuffer *buffer);

	SDL_GPUTexture *CreateTexture(SDL_GPUDevice *device, const SDL_GPUTextureCreateInfo *info);
	void ReleaseTexture(SDL_GPUDevice *device, SDL_GPUTexture *texture);

	GpuMemoryStatistics MemoryStatistics(SDL_GPUDevice *device);
	void ForgetDevice(SDL_GPUDevice *device);
}
