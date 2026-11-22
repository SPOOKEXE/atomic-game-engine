#pragma once

#include "AtomicGpuQueues.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace engine::render {
	class FrozenGpuResources {
	  public:
		FrozenGpuResources(SDL_GPUDevice *device, const AtomicGpuQueuesV1 &api);
		~FrozenGpuResources();
		FrozenGpuResources(const FrozenGpuResources &) = delete;
		FrozenGpuResources &operator=(const FrozenGpuResources &) = delete;
		void *Freeze(uint32_t kind, void *handle);

	  private:
		SDL_GPUDevice *Device;
		const AtomicGpuQueuesV1 &Api;
		std::vector<AtomicGpuResource> Original;
		std::vector<AtomicGpuResource> Frozen;
	};

	struct GpuDrawPacket {
		enum class Kind : uint8_t {
			Pipeline,
			VertexBuffers,
			IndexBuffer,
			VertexStorage,
			FragmentSamplers,
			VertexUniform,
			FragmentUniform,
			Viewport,
			Scissor,
			Indexed,
			Indirect
		};
		struct Operation {
			Kind Type;
			std::array<uint32_t, 6> Arguments{};
			void *Handle = nullptr;
		};

		FrozenGpuResources Resources;
		std::array<SDL_GPUColorTargetInfo, 8> Targets{};
		SDL_GPUDepthStencilTargetInfo Depth{};
		std::vector<Operation> Operations;
		std::vector<SDL_GPUBufferBinding> VertexBindings;
		std::vector<SDL_GPUBuffer *> StorageBindings;
		std::vector<SDL_GPUTextureSamplerBinding> SamplerBindings;
		std::vector<std::byte> Uniforms;
		std::vector<SDL_GPUViewport> Viewports;
		std::vector<SDL_Rect> Scissors;

		GpuDrawPacket(SDL_GPUDevice *device, const AtomicGpuQueuesV1 &api) : Resources(device, api) {}
		bool Record(SDL_GPUCommandBuffer *command) const;
		void Append(Operation operation);
	};

	class DrawEncoder {
	  public:
		DrawEncoder(SDL_GPUCommandBuffer *command, SDL_GPURenderPass *pass, GpuDrawPacket *packet = nullptr)
			: Command(command), Pass(pass), Packet(packet) {}
		void Pipeline(SDL_GPUGraphicsPipeline *pipeline);
		void VertexBuffers(uint32_t first, const SDL_GPUBufferBinding *bindings, uint32_t count);
		void IndexBuffer(const SDL_GPUBufferBinding &binding, SDL_GPUIndexElementSize size);
		void VertexStorage(uint32_t first, SDL_GPUBuffer *const *buffers, uint32_t count);
		void FragmentSamplers(uint32_t first, const SDL_GPUTextureSamplerBinding *bindings, uint32_t count);
		void VertexUniform(uint32_t slot, const void *bytes, uint32_t size);
		void FragmentUniform(uint32_t slot, const void *bytes, uint32_t size);
		void Viewport(const SDL_GPUViewport &viewport);
		void Scissor(const SDL_Rect &scissor);
		void Indexed(uint32_t indices, uint32_t instances, uint32_t first, int32_t offset, uint32_t instance);
		void Indirect(SDL_GPUBuffer *arguments, uint32_t offset, uint32_t count);

	  private:
		void Uniform(bool fragment, uint32_t slot, const void *bytes, uint32_t size);
		SDL_GPUCommandBuffer *Command;
		SDL_GPURenderPass *Pass;
		GpuDrawPacket *Packet;
	};
}
