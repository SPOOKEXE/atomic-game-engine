#pragma once

// Explicit diagnostic snapshots, copied before a later graph node can alias the image.
#include <SDL3/SDL_gpu.h>

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace engine::render {
	struct RenderStageProbe {
		struct Snapshot {
			SDL_GPUTransferBuffer *Transfer = nullptr;
			std::filesystem::path Stem;
			std::string Label;
			uint32_t Width = 0, Height = 0, Pitch = 0, PixelBytes = 0;
			SDL_GPUTextureFormat Format = SDL_GPU_TEXTUREFORMAT_INVALID;
		};
		std::filesystem::path Directory;
		uint64_t First = 0, Last = 8, Sequence = 0;
		size_t PendingBytes = 0;
		uint64_t ViewSlot = UINT64_MAX;
		std::vector<Snapshot> Pending;

		static std::string Quote(std::string_view value);
		void Configure();
		bool Enabled(uint64_t frame, uint64_t view = UINT64_MAX) const;
		void Record(
			SDL_GPUDevice *device,
			SDL_GPUCommandBuffer *command,
			uint64_t frame,
			const std::string &metadata,
			SDL_GPUTexture *texture,
			uint32_t width,
			uint32_t height,
			SDL_GPUTextureFormat format,
			std::string_view label = {}
		);
		void Flush(SDL_GPUDevice *device);
		void Clear(SDL_GPUDevice *device);
	};
}
