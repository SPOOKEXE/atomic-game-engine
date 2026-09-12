// GPU resource lifetime and dispatch for authored mesh LOD selection.

#include "GpuHeap.hpp"
#include "RendererState.hpp"

#include <engine/core/Log.hpp>
#include <engine/core/Profiling.hpp>

#include <SDL3/SDL_gpu.h>
#include <glm/vec4.hpp>

#include <algorithm>

namespace engine::render {

	bool Renderer::Impl::EnsureLodResources(uint32_t selections, uint32_t instances, uint32_t arguments) {
		const auto grown = [](uint32_t have, uint32_t need) {
			uint32_t result = have == 0 ? 64u : have;
			while (result < need) {
				result *= 2u;
			}
			return result;
		};
		const auto buffer = [&](SDL_GPUBuffer *&value,
								uint32_t &capacity,
								uint32_t need,
								uint32_t stride,
								SDL_GPUBufferUsageFlags usage,
								const char *label) {
			if (value != nullptr && capacity >= need) {
				return true;
			}
			const uint32_t next = grown(capacity, need);
			if (value != nullptr) {
				gpu::ReleaseBuffer(Device, value);
			}
			SDL_GPUBufferCreateInfo info{};
			info.usage = usage;
			info.size = next * stride;
			value = gpu::CreateBuffer(Device, &info);
			if (value == nullptr) {
				ENGINE_ERROR("lod {} buffer of {} entries: {}", label, next, SDL_GetError());
				capacity = 0;
				return false;
			}
			capacity = next;
			return true;
		};

		bool ready = buffer(
			Lod.Selections,
			Lod.SelectionCapacity,
			selections,
			sizeof(GpuLodSelection),
			SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_READ,
			"selection"
		);
		ready = buffer(
					Lod.Clusters,
					Lod.ClusterCapacity,
					static_cast<uint32_t>(LodFrame.Clusters.size()),
					sizeof(GpuLodCluster),
					SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_READ,
					"cluster"
				) &&
				ready;
		const uint32_t oldInstanceCapacity = Lod.InstanceCapacity;
		ready = buffer(
					Lod.Instances,
					Lod.InstanceCapacity,
					instances,
					sizeof(GpuInstance),
					SDL_GPU_BUFFERUSAGE_GRAPHICS_STORAGE_READ,
					"instance"
				) &&
				ready;
		if (Lod.InstanceCapacity != oldInstanceCapacity || Lod.Indices == nullptr ||
			Lod.SkinOffsets == nullptr) {
			for (SDL_GPUBuffer **value : {&Lod.Indices, &Lod.SkinOffsets}) {
				if (*value != nullptr) {
					gpu::ReleaseBuffer(Device, *value);
				}
				SDL_GPUBufferCreateInfo info{};
				info.usage = SDL_GPU_BUFFERUSAGE_GRAPHICS_STORAGE_READ;
				info.size = Lod.InstanceCapacity * sizeof(uint32_t);
				*value = gpu::CreateBuffer(Device, &info);
				ready = *value != nullptr && ready;
			}
			if (Lod.Indices == nullptr || Lod.SkinOffsets == nullptr) {
				ENGINE_ERROR("lod index buffers of {} entries: {}", Lod.InstanceCapacity, SDL_GetError());
			}
		}
		ready = buffer(
					Lod.Arguments,
					Lod.ArgumentCapacity,
					arguments,
					sizeof(SDL_GPUIndexedIndirectDrawCommand),
					SDL_GPU_BUFFERUSAGE_INDIRECT | SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_WRITE,
					"argument"
				) &&
				ready;

		const LodTransferLayout layout = TransferLayoutOf(LodFrame);
		if (Lod.Transfer == nullptr || Lod.TransferCapacity < layout.Bytes) {
			uint32_t bytes = grown(Lod.TransferCapacity, layout.Bytes);
			if (Lod.Transfer != nullptr) {
				gpu::ReleaseTransferBuffer(Device, Lod.Transfer);
			}
			SDL_GPUTransferBufferCreateInfo info{};
			info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
			info.size = bytes;
			Lod.Transfer = gpu::CreateTransferBuffer(Device, &info);
			if (Lod.Transfer == nullptr) {
				ENGINE_ERROR("lod transfer buffer of {} bytes: {}", bytes, SDL_GetError());
				Lod.TransferCapacity = 0;
				return false;
			}
			Lod.TransferCapacity = bytes;
		}
		return ready;
	}

	bool Renderer::Impl::DispatchLodSelection(
		SDL_GPUCommandBuffer *command, const glm::mat4 &viewProjection, uint32_t width, uint32_t height
	) {
		Lod.Ready = false;
		if (LodFrame.Selections.empty()) {
			return true;
		}
		if (Lod.Select == nullptr || command == nullptr) {
			return false;
		}

		ENGINE_PROFILE_CAT("select authored lod", core::ProfileCategory::Render);
		SDL_GPUStorageBufferReadWriteBinding output{};
		output.buffer = Lod.Arguments;
		SDL_GPUComputePass *pass = SDL_BeginGPUComputePass(command, nullptr, 0, &output, 1);
		if (pass == nullptr) {
			ENGINE_ERROR("lod selection: SDL_BeginGPUComputePass: {}", SDL_GetError());
			return false;
		}
		SDL_BindGPUComputePipeline(pass, Lod.Select);
		SDL_GPUBuffer *const reads[] = {Lod.Selections, Lod.Clusters};
		SDL_BindGPUComputeStorageBuffers(pass, 0, reads, 2);
		struct Uniforms {
			glm::mat4 ViewProjection;
			glm::uvec4 Counts;
			glm::vec4 Viewport;
		} uniforms{
			viewProjection,
			{static_cast<uint32_t>(LodFrame.Selections.size()),
			 static_cast<uint32_t>(LodFrame.Clusters.size()),
			 0u,
			 0u},
			{static_cast<float>(width), static_cast<float>(height), 0.0f, 0.0f},
		};
		SDL_PushGPUComputeUniformData(command, 0, &uniforms, sizeof(uniforms));
		SDL_DispatchGPUCompute(pass, (uniforms.Counts.y + 63u) / 64u, 1, 1);
		SDL_EndGPUComputePass(pass);
		Lod.Ready = true;
		return true;
	}

	void Renderer::Impl::ReleaseLod() {
		for (SDL_GPUBuffer **buffer :
			 {&Lod.Selections,
			  &Lod.Clusters,
			  &Lod.Instances,
			  &Lod.Indices,
			  &Lod.SkinOffsets,
			  &Lod.Arguments}) {
			if (*buffer != nullptr) {
				gpu::ReleaseBuffer(Device, *buffer);
				*buffer = nullptr;
			}
		}
		if (Lod.Transfer != nullptr) {
			gpu::ReleaseTransferBuffer(Device, Lod.Transfer);
			Lod.Transfer = nullptr;
		}
		if (Lod.Select != nullptr) {
			SDL_ReleaseGPUComputePipeline(Device, Lod.Select);
			Lod.Select = nullptr;
		}
		Lod = {};
		LodFrame.Clear();
	}
}
