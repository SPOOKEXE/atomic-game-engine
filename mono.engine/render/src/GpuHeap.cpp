#include "GpuHeap.hpp"

#include <algorithm>
#include <cstdint>
#include <mutex>
#include <unordered_map>

namespace engine::render::gpu {
	namespace {
		enum class ResourceKind : uint8_t {
			Buffer,
			TransferBuffer,
			Texture,
		};

		struct ResourceKey {
			const void *Handle = nullptr;
			ResourceKind Kind = ResourceKind::Buffer;

			bool operator==(const ResourceKey &) const = default;
		};

		struct ResourceKeyHash {
			size_t operator()(const ResourceKey &key) const {
				return std::hash<const void *>{}(key.Handle) ^ (static_cast<size_t>(key.Kind) << 1u);
			}
		};

		struct Allocation {
			SDL_GPUDevice *Device = nullptr;
			uint64_t Bytes = 0;
		};

		struct Registry {
			std::mutex Mutex;
			std::unordered_map<ResourceKey, Allocation, ResourceKeyHash> Allocations;
			std::unordered_map<SDL_GPUDevice *, GpuMemoryStatistics> Devices;
		};

		Registry &Resources() {
			static Registry resources;
			return resources;
		}

		uint32_t Samples(SDL_GPUSampleCount count) {
			switch (count) {
			case SDL_GPU_SAMPLECOUNT_2:
				return 2;
			case SDL_GPU_SAMPLECOUNT_4:
				return 4;
			case SDL_GPU_SAMPLECOUNT_8:
				return 8;
			default:
				return 1;
			}
		}

		uint64_t TextureByteSize(const SDL_GPUTextureCreateInfo &info) {
			uint64_t bytes = 0;
			uint32_t width = std::max(info.width, 1u);
			uint32_t height = std::max(info.height, 1u);
			uint32_t depth = std::max(info.layer_count_or_depth, 1u);
			const uint32_t levels = std::max(info.num_levels, 1u);

			for (uint32_t level = 0; level < levels; level++) {
				bytes += SDL_CalculateGPUTextureFormatSize(info.format, width, height, depth);
				width = std::max(width >> 1u, 1u);
				height = std::max(height >> 1u, 1u);
				if (info.type == SDL_GPU_TEXTURETYPE_3D) {
					depth = std::max(depth >> 1u, 1u);
				}
			}

			return bytes * Samples(info.sample_count);
		}

		void Add(SDL_GPUDevice *device, const void *handle, ResourceKind kind, uint64_t bytes) {
			if (device == nullptr || handle == nullptr) {
				return;
			}

			Registry &resources = Resources();
			std::lock_guard lock(resources.Mutex);
			const bool inserted =
				resources.Allocations.emplace(ResourceKey{handle, kind}, Allocation{device, bytes}).second;
			if (!inserted) {
				return;
			}
			GpuMemoryStatistics &totals = resources.Devices[device];
			switch (kind) {
			case ResourceKind::Buffer:
				totals.BufferBytes += bytes;
				totals.Buffers++;
				break;
			case ResourceKind::TransferBuffer:
				totals.TransferBufferBytes += bytes;
				totals.TransferBuffers++;
				break;
			case ResourceKind::Texture:
				totals.TextureBytes += bytes;
				totals.Textures++;
				break;
			}
			totals.LiveBytes += bytes;
			totals.PeakBytes = std::max(totals.PeakBytes, totals.LiveBytes);
		}

		void Remove(SDL_GPUDevice *device, const void *handle, ResourceKind kind) {
			if (device == nullptr || handle == nullptr) {
				return;
			}

			Registry &resources = Resources();
			std::lock_guard lock(resources.Mutex);
			const auto allocation = resources.Allocations.find(ResourceKey{handle, kind});
			if (allocation == resources.Allocations.end()) {
				return;
			}

			const uint64_t bytes = allocation->second.Bytes;
			const auto foundDevice = resources.Devices.find(allocation->second.Device);
			if (foundDevice != resources.Devices.end()) {
				GpuMemoryStatistics &totals = foundDevice->second;
				totals.LiveBytes -= std::min(totals.LiveBytes, bytes);
				switch (kind) {
				case ResourceKind::Buffer:
					totals.BufferBytes -= std::min(totals.BufferBytes, bytes);
					totals.Buffers -= std::min(totals.Buffers, uint64_t{1});
					break;
				case ResourceKind::TransferBuffer:
					totals.TransferBufferBytes -= std::min(totals.TransferBufferBytes, bytes);
					totals.TransferBuffers -= std::min(totals.TransferBuffers, uint64_t{1});
					break;
				case ResourceKind::Texture:
					totals.TextureBytes -= std::min(totals.TextureBytes, bytes);
					totals.Textures -= std::min(totals.Textures, uint64_t{1});
					break;
				}
			}
			resources.Allocations.erase(allocation);
		}
	}

	SDL_GPUBuffer *CreateBuffer(SDL_GPUDevice *device, const SDL_GPUBufferCreateInfo *info) {
		SDL_GPUBuffer *const buffer = SDL_CreateGPUBuffer(device, info);
		if (buffer != nullptr && info != nullptr) {
			Add(device, buffer, ResourceKind::Buffer, info->size);
		}
		return buffer;
	}

	void ReleaseBuffer(SDL_GPUDevice *device, SDL_GPUBuffer *buffer) {
		Remove(device, buffer, ResourceKind::Buffer);
		SDL_ReleaseGPUBuffer(device, buffer);
	}

	SDL_GPUTransferBuffer *
	CreateTransferBuffer(SDL_GPUDevice *device, const SDL_GPUTransferBufferCreateInfo *info) {
		SDL_GPUTransferBuffer *const buffer = SDL_CreateGPUTransferBuffer(device, info);
		if (buffer != nullptr && info != nullptr) {
			Add(device, buffer, ResourceKind::TransferBuffer, info->size);
		}
		return buffer;
	}

	void ReleaseTransferBuffer(SDL_GPUDevice *device, SDL_GPUTransferBuffer *buffer) {
		Remove(device, buffer, ResourceKind::TransferBuffer);
		SDL_ReleaseGPUTransferBuffer(device, buffer);
	}

	SDL_GPUTexture *CreateTexture(SDL_GPUDevice *device, const SDL_GPUTextureCreateInfo *info) {
		SDL_GPUTexture *const texture = SDL_CreateGPUTexture(device, info);
		if (texture != nullptr && info != nullptr) {
			Add(device, texture, ResourceKind::Texture, TextureByteSize(*info));
		}
		return texture;
	}

	void ReleaseTexture(SDL_GPUDevice *device, SDL_GPUTexture *texture) {
		Remove(device, texture, ResourceKind::Texture);
		SDL_ReleaseGPUTexture(device, texture);
	}

	GpuMemoryStatistics MemoryStatistics(SDL_GPUDevice *device) {
		Registry &resources = Resources();
		std::lock_guard lock(resources.Mutex);
		const auto found = resources.Devices.find(device);
		return found != resources.Devices.end() ? found->second : GpuMemoryStatistics{};
	}

	void ForgetDevice(SDL_GPUDevice *device) {
		Registry &resources = Resources();
		std::lock_guard lock(resources.Mutex);
		for (auto allocation = resources.Allocations.begin(); allocation != resources.Allocations.end();) {
			if (allocation->second.Device == device) {
				allocation = resources.Allocations.erase(allocation);
			} else {
				allocation++;
			}
		}
		resources.Devices.erase(device);
	}
}
