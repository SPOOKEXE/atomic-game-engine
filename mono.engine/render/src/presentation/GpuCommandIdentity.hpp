#pragma once

struct SDL_GPUCommandBuffer;

namespace engine::render {
	SDL_GPUCommandBuffer *GpuCommandIdentity(SDL_GPUCommandBuffer *command);
	inline const void *GpuCommandIdentity(const void *command) {
		return GpuCommandIdentity(static_cast<SDL_GPUCommandBuffer *>(const_cast<void *>(command)));
	}
}
