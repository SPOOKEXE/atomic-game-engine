#pragma once
#include "SourceCamera3D.hpp"

#include <engine/render/ComposerSurface.hpp>
namespace engine::render::hlsl {
	bool RecordComposerSurface(
		SDL_GPUDevice *,
		SDL_GPUCommandBuffer *,
		const SurfaceRequest &,
		imagegraph::SourceCamera3DResources &,
		SDL_GPUTransferBuffer *&displayDownload,
		bool captureReadback = false
	);
	bool RecordComposerDisplayUpload(
		SDL_GPUDevice *,
		SDL_GPUCommandBuffer *,
		uint32_t width,
		uint32_t height,
		std::span<const uint8_t>,
		imagegraph::SourceCamera3DResources &
	);
}
