#pragma once

#include <SDL3/SDL_gpu.h>

#include <cstdint>

namespace engine::render {
	// A surface bank reuses transient work images in capture postorder. No
	// temporal samples survive between capture cameras or world revisions.
	struct WeatherCaptureScratch {
		SDL_GPUTexture *Clouds = nullptr;
		SDL_GPUTexture *Depth = nullptr;
		SDL_GPUTexture *Resolved = nullptr;
		SDL_GPUTexture *Composite = nullptr;
		SDL_GPUTexture *HardwareDepth = nullptr;
		SDL_GPUBuffer *Bindings = nullptr;
		uint32_t Width = 0;
		uint32_t Height = 0;
	};
	void ReleaseWeatherCapture(SDL_GPUDevice *device, WeatherCaptureScratch &scratch);
}
