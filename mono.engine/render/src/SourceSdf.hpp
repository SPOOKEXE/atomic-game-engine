#pragma once
#include "SourceCamera3D.hpp"

#include <engine/render/SourceSdf.hpp>
namespace engine::render::imagegraph {
	uint64_t SourceSdfSourceBytes(const SourceSdfRequest &request);
	bool CompleteSourceSdfCapture(
		const SourceSdfRequest &,
		std::span<const std::byte> download,
		uint64_t maximumBytes,
		engine::imagegraph::HostNodeCapture &,
		bool packed = false
	);
	uint64_t SourceSdfScratchBytes(const SourceSdfRequest &request);
	bool RecordSourceSdf(
		SDL_GPUDevice *device,
		SDL_GPUCommandBuffer *command,
		const SourceSdfRequest &request,
		SourceCamera3DResources &resources,
		bool captureReadback = false
	);
}
