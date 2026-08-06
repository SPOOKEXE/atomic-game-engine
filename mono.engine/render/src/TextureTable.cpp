#include <engine/core/Log.hpp>
#include <engine/render/TextureTable.hpp>

#include <SDL3/SDL_gpu.h>

#include <cstring>
#include <vector>

namespace engine::render {

	TextureTable::~TextureTable() {
		Shutdown();
	}

	bool TextureTable::Initialise(SDL_GPUDevice *device) {
		Device = device;
		if (Device == nullptr) {
			return false;
		}

		SDL_GPUSamplerCreateInfo sampler{};
		sampler.min_filter = SDL_GPU_FILTER_LINEAR;
		sampler.mag_filter = SDL_GPU_FILTER_LINEAR;
		sampler.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_LINEAR;

		// Imported model coordinates may exceed one; repeat avoids edge smearing.
		sampler.address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_REPEAT;
		sampler.address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_REPEAT;
		sampler.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_REPEAT;

		SharedSampler = SDL_CreateGPUSampler(Device, &sampler);
		if (SharedSampler == nullptr) {
			ENGINE_ERROR("texture table: sampler: {}", SDL_GetError());
			return false;
		}
		return true;
	}

	void TextureTable::Shutdown() {
		if (Device != nullptr) {
			for (const auto &[name, texture] : Textures) {
				SDL_ReleaseGPUTexture(Device, texture);
			}
			if (SharedSampler != nullptr) {
				SDL_ReleaseGPUSampler(Device, SharedSampler);
			}
		}

		Textures.clear();
		SharedSampler = nullptr;
		UploadedBytes = 0;
		Device = nullptr;
	}

	bool TextureTable::Add(const core::Name &name, const assets::TextureData &image) {
		if (Device == nullptr || !name.IsValid() || !image.IsValid()) {
			return false;
		}

		const size_t bytes = image.Pixels.size();
		if (UploadedBytes + bytes > MAXIMUM_BYTES) {
			ENGINE_WARN("texture table: full, refusing {}", name.Text());
			return false;
		}

		// Expand R8 assets so every texture uses the pipeline's RGBA format.
		std::vector<std::byte> widened;
		const std::byte *pixels = image.Pixels.data();
		if (image.Format == assets::TextureFormat::R8) {
			widened.resize(static_cast<size_t>(image.Width) * image.Height * 4);
			for (size_t index = 0; index * 4 < widened.size(); index++) {
				widened[index * 4] = image.Pixels[index];
				widened[index * 4 + 1] = image.Pixels[index];
				widened[index * 4 + 2] = image.Pixels[index];
				widened[index * 4 + 3] = std::byte{255};
			}
			pixels = widened.data();
		}

		const size_t uploadBytes = static_cast<size_t>(image.Width) * image.Height * 4;

		SDL_GPUTextureCreateInfo info{};
		info.type = SDL_GPU_TEXTURETYPE_2D;
		info.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
		info.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
		info.width = image.Width;
		info.height = image.Height;
		info.layer_count_or_depth = 1;
		info.num_levels = 1;
		info.sample_count = SDL_GPU_SAMPLECOUNT_1;

		SDL_GPUTexture *texture = SDL_CreateGPUTexture(Device, &info);
		if (texture == nullptr) {
			ENGINE_ERROR("texture table: {}: {}", name.Text(), SDL_GetError());
			return false;
		}

		SDL_GPUTransferBufferCreateInfo transferInfo{};
		transferInfo.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
		transferInfo.size = static_cast<uint32_t>(uploadBytes);

		SDL_GPUTransferBuffer *transfer = SDL_CreateGPUTransferBuffer(Device, &transferInfo);
		if (transfer == nullptr) {
			ENGINE_ERROR("texture table: transfer buffer: {}", SDL_GetError());
			SDL_ReleaseGPUTexture(Device, texture);
			return false;
		}

		void *mapped = SDL_MapGPUTransferBuffer(Device, transfer, false);
		std::memcpy(mapped, pixels, uploadBytes);
		SDL_UnmapGPUTransferBuffer(Device, transfer);

		SDL_GPUCommandBuffer *command = SDL_AcquireGPUCommandBuffer(Device);
		SDL_GPUCopyPass *copy = SDL_BeginGPUCopyPass(command);

		SDL_GPUTextureTransferInfo source{};
		source.transfer_buffer = transfer;
		source.pixels_per_row = image.Width;
		source.rows_per_layer = image.Height;

		SDL_GPUTextureRegion region{};
		region.texture = texture;
		region.w = image.Width;
		region.h = image.Height;
		region.d = 1;

		SDL_UploadToGPUTexture(copy, &source, &region, false);
		SDL_EndGPUCopyPass(copy);
		SDL_SubmitGPUCommandBuffer(command);
		SDL_ReleaseGPUTransferBuffer(Device, transfer);

		// Release the old texture only after the replacement upload succeeds.
		const auto existing = Textures.find(name.Id());
		if (existing != Textures.end()) {
			SDL_ReleaseGPUTexture(Device, existing->second);
			existing->second = texture;
		} else {
			Textures.emplace(name.Id(), texture);
		}

		UploadedBytes += uploadBytes;
		return true;
	}

	SDL_GPUTexture *TextureTable::Find(const core::Name &name) const {
		if (!name.IsValid()) {
			return nullptr;
		}
		const auto found = Textures.find(name.Id());
		return found == Textures.end() ? nullptr : found->second;
	}
}
