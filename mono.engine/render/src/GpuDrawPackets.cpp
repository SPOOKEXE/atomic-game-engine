#include "GpuDrawPackets.hpp"

#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace engine::render {
	namespace {
		void CheckBindingRange(uint32_t first, uint32_t count, uint32_t limit, size_t payload) {
			if (first > limit || count > limit - first || payload + count > 262144)
				throw std::runtime_error("GPU packet binding budget exceeded");
		}
	}
	FrozenGpuResources::FrozenGpuResources(SDL_GPUDevice *device, const AtomicGpuQueuesV1 &api)
		: Device(device), Api(api) {
		Original.reserve(128);
		Frozen.reserve(128);
	}
	FrozenGpuResources::~FrozenGpuResources() {
		for (size_t first = 0; first < Frozen.size(); first += ATOMIC_GPU_QUEUES_MAX_RESOURCES) {
			const auto count = static_cast<uint32_t>(
				std::min<size_t>(ATOMIC_GPU_QUEUES_MAX_RESOURCES, Frozen.size() - first)
			);
			(void)Api.ReleaseFrozenResources(Device, count, Frozen.data() + first);
		}
	}
	void *FrozenGpuResources::Freeze(uint32_t kind, void *handle) {
		if (handle == nullptr) return nullptr;
		for (size_t index = 0; index < Original.size(); ++index)
			if (Original[index].Kind == kind && Original[index].Handle == handle) return Frozen[index].Handle;
		if (Original.size() == 4096) throw std::runtime_error("GPU packet resource budget exceeded");
		if (Original.size() == Original.capacity())
			Original.reserve(std::min<size_t>(4096, Original.capacity() * 2));
		if (Frozen.size() == Frozen.capacity()) Frozen.reserve(std::min<size_t>(4096, Frozen.capacity() * 2));
		const AtomicGpuResource source{kind, handle};
		AtomicGpuResource frozen{};
		if (!Api.FreezeResources(Device, 1, &source, &frozen)) throw std::runtime_error(SDL_GetError());
		Original.push_back(source);
		Frozen.push_back(frozen);
		return frozen.Handle;
	}
	void GpuDrawPacket::Append(Operation operation) {
		if (Operations.size() == 262144) throw std::runtime_error("GPU packet operation budget exceeded");
		Operations.push_back(operation);
	}
	bool GpuDrawPacket::Record(SDL_GPUCommandBuffer *command) const {
		auto *pass = SDL_BeginGPURenderPass(command, Targets.data(), 8, &Depth);
		if (pass == nullptr) return false;
		for (const auto &operation : Operations) {
			const auto &a = operation.Arguments;
			switch (operation.Type) {
			case Kind::Pipeline:
				SDL_BindGPUGraphicsPipeline(pass, static_cast<SDL_GPUGraphicsPipeline *>(operation.Handle));
				break;
			case Kind::VertexBuffers:
				SDL_BindGPUVertexBuffers(pass, a[0], VertexBindings.data() + a[1], a[2]);
				break;
			case Kind::IndexBuffer: {
				const SDL_GPUBufferBinding binding{static_cast<SDL_GPUBuffer *>(operation.Handle), a[0]};
				SDL_BindGPUIndexBuffer(pass, &binding, static_cast<SDL_GPUIndexElementSize>(a[1]));
				break;
			}
			case Kind::VertexStorage:
				SDL_BindGPUVertexStorageBuffers(pass, a[0], StorageBindings.data() + a[1], a[2]);
				break;
			case Kind::FragmentSamplers:
				SDL_BindGPUFragmentSamplers(pass, a[0], SamplerBindings.data() + a[1], a[2]);
				break;
			case Kind::VertexUniform:
				SDL_PushGPUVertexUniformData(command, a[0], Uniforms.data() + a[1], a[2]);
				break;
			case Kind::FragmentUniform:
				SDL_PushGPUFragmentUniformData(command, a[0], Uniforms.data() + a[1], a[2]);
				break;
			case Kind::Viewport:
				SDL_SetGPUViewport(pass, &Viewports[a[0]]);
				break;
			case Kind::Scissor:
				SDL_SetGPUScissor(pass, &Scissors[a[0]]);
				break;
			case Kind::Indexed:
				SDL_DrawGPUIndexedPrimitives(pass, a[0], a[1], a[2], static_cast<int32_t>(a[3]), a[4]);
				break;
			case Kind::Indirect:
				SDL_DrawGPUIndexedPrimitivesIndirect(
					pass, static_cast<SDL_GPUBuffer *>(operation.Handle), a[0], a[1]
				);
				break;
			}
		}
		SDL_EndGPURenderPass(pass);
		return true;
	}
	void DrawEncoder::Pipeline(SDL_GPUGraphicsPipeline *pipeline) {
		if (!Packet) return SDL_BindGPUGraphicsPipeline(Pass, pipeline);
		Packet->Append(
			{GpuDrawPacket::Kind::Pipeline,
			 {},
			 Packet->Resources.Freeze(ATOMIC_GPU_RESOURCE_GRAPHICS_PIPELINE, pipeline)}
		);
	}
	void DrawEncoder::VertexBuffers(uint32_t first, const SDL_GPUBufferBinding *bindings, uint32_t count) {
		if (!Packet) return SDL_BindGPUVertexBuffers(Pass, first, bindings, count);
		CheckBindingRange(first, count, 16, Packet->VertexBindings.size());
		const auto offset = static_cast<uint32_t>(Packet->VertexBindings.size());
		for (uint32_t index = 0; index < count; ++index) {
			auto binding = bindings[index];
			binding.buffer = static_cast<SDL_GPUBuffer *>(
				Packet->Resources.Freeze(ATOMIC_GPU_RESOURCE_BUFFER, binding.buffer)
			);
			Packet->VertexBindings.push_back(binding);
		}
		Packet->Append({GpuDrawPacket::Kind::VertexBuffers, {first, offset, count}});
	}
	void DrawEncoder::IndexBuffer(const SDL_GPUBufferBinding &binding, SDL_GPUIndexElementSize size) {
		if (!Packet) return SDL_BindGPUIndexBuffer(Pass, &binding, size);
		Packet->Append(
			{GpuDrawPacket::Kind::IndexBuffer,
			 {binding.offset, static_cast<uint32_t>(size)},
			 Packet->Resources.Freeze(ATOMIC_GPU_RESOURCE_BUFFER, binding.buffer)}
		);
	}
	void DrawEncoder::VertexStorage(uint32_t first, SDL_GPUBuffer *const *buffers, uint32_t count) {
		if (!Packet) return SDL_BindGPUVertexStorageBuffers(Pass, first, buffers, count);
		CheckBindingRange(first, count, 8, Packet->StorageBindings.size());
		const auto offset = static_cast<uint32_t>(Packet->StorageBindings.size());
		for (uint32_t index = 0; index < count; ++index)
			Packet->StorageBindings.push_back(
				static_cast<SDL_GPUBuffer *>(
					Packet->Resources.Freeze(ATOMIC_GPU_RESOURCE_BUFFER, buffers[index])
				)
			);
		Packet->Append({GpuDrawPacket::Kind::VertexStorage, {first, offset, count}});
	}
	void DrawEncoder::FragmentSamplers(
		uint32_t first, const SDL_GPUTextureSamplerBinding *bindings, uint32_t count
	) {
		if (!Packet) return SDL_BindGPUFragmentSamplers(Pass, first, bindings, count);
		CheckBindingRange(first, count, 16, Packet->SamplerBindings.size());
		const auto offset = static_cast<uint32_t>(Packet->SamplerBindings.size());
		for (uint32_t index = 0; index < count; ++index) {
			auto binding = bindings[index];
			binding.texture = static_cast<SDL_GPUTexture *>(
				Packet->Resources.Freeze(ATOMIC_GPU_RESOURCE_TEXTURE, binding.texture)
			);
			binding.sampler = static_cast<SDL_GPUSampler *>(
				Packet->Resources.Freeze(ATOMIC_GPU_RESOURCE_SAMPLER, binding.sampler)
			);
			Packet->SamplerBindings.push_back(binding);
		}
		Packet->Append({GpuDrawPacket::Kind::FragmentSamplers, {first, offset, count}});
	}
	void DrawEncoder::Uniform(bool fragment, uint32_t slot, const void *bytes, uint32_t size) {
		if (!Packet) {
			if (fragment)
				SDL_PushGPUFragmentUniformData(Command, slot, bytes, size);
			else
				SDL_PushGPUVertexUniformData(Command, slot, bytes, size);
			return;
		}
		if (slot >= 4 || size > 65536 || Packet->Uniforms.size() + size > 16u * 1024u * 1024u)
			throw std::runtime_error("GPU packet uniform budget exceeded");
		const auto offset = static_cast<uint32_t>(Packet->Uniforms.size());
		const auto *source = static_cast<const std::byte *>(bytes);
		Packet->Uniforms.insert(Packet->Uniforms.end(), source, source + size);
		Packet->Append(
			{fragment ? GpuDrawPacket::Kind::FragmentUniform : GpuDrawPacket::Kind::VertexUniform,
			 {slot, offset, size}}
		);
	}
	void DrawEncoder::VertexUniform(uint32_t slot, const void *bytes, uint32_t size) {
		Uniform(false, slot, bytes, size);
	}
	void DrawEncoder::FragmentUniform(uint32_t slot, const void *bytes, uint32_t size) {
		Uniform(true, slot, bytes, size);
	}
	void DrawEncoder::Viewport(const SDL_GPUViewport &viewport) {
		if (!Packet) return SDL_SetGPUViewport(Pass, &viewport);
		Packet->Append({GpuDrawPacket::Kind::Viewport, {static_cast<uint32_t>(Packet->Viewports.size())}});
		Packet->Viewports.push_back(viewport);
	}
	void DrawEncoder::Scissor(const SDL_Rect &scissor) {
		if (!Packet) return SDL_SetGPUScissor(Pass, &scissor);
		Packet->Append({GpuDrawPacket::Kind::Scissor, {static_cast<uint32_t>(Packet->Scissors.size())}});
		Packet->Scissors.push_back(scissor);
	}
	void DrawEncoder::Indexed(
		uint32_t indices, uint32_t instances, uint32_t first, int32_t offset, uint32_t instance
	) {
		if (!Packet) return SDL_DrawGPUIndexedPrimitives(Pass, indices, instances, first, offset, instance);
		Packet->Append(
			{GpuDrawPacket::Kind::Indexed,
			 {indices, instances, first, static_cast<uint32_t>(offset), instance}}
		);
	}
	void DrawEncoder::Indirect(SDL_GPUBuffer *arguments, uint32_t offset, uint32_t count) {
		if (!Packet) return SDL_DrawGPUIndexedPrimitivesIndirect(Pass, arguments, offset, count);
		Packet->Append(
			{GpuDrawPacket::Kind::Indirect,
			 {offset, count},
			 Packet->Resources.Freeze(ATOMIC_GPU_RESOURCE_BUFFER, arguments)}
		);
	}
}
