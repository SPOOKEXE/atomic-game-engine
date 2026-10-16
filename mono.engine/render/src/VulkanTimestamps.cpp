#include "VulkanTimestamps.hpp"

#include "AtomicGpuQueues.h"

#include <engine/core/Log.hpp>

#include <SDL3/SDL_gpu.h>
#include <SDL3/SDL_vulkan.h>

#include <cstddef>
#include <string_view>
#include <vulkan/vulkan.h>

namespace engine::render {

	namespace {
		constexpr uint32_t SAMPLERS_PER_STAGE = 16;
		constexpr uint32_t STORAGE_TEXTURES_PER_STAGE = 8;
		constexpr uint32_t STORAGE_BUFFERS_PER_STAGE = 8;
		constexpr uint32_t COMPUTE_WRITE_TEXTURES = 8;
		constexpr uint32_t COMPUTE_WRITE_BUFFERS = 8;
		constexpr uint32_t COLOUR_TARGET_BINDINGS = 8;

		struct MirroredPass {
			SDL_GPUCommandBuffer *CommandBuffer;
			bool InProgress;
		};

		struct MirroredComputePass {
			SDL_GPUCommandBuffer *CommandBuffer;
			bool InProgress;
			SDL_GPUComputePipeline *Pipeline;
			bool SamplerBound[SAMPLERS_PER_STAGE];
			bool ReadOnlyStorageTextureBound[STORAGE_TEXTURES_PER_STAGE];
			bool ReadOnlyStorageBufferBound[STORAGE_BUFFERS_PER_STAGE];
			bool ReadWriteStorageTextureBound[COMPUTE_WRITE_TEXTURES];
			bool ReadWriteStorageBufferBound[COMPUTE_WRITE_BUFFERS];
		};

		struct MirroredRenderPass {
			SDL_GPUCommandBuffer *CommandBuffer;
			bool InProgress;
			SDL_GPUTexture *ColourTargets[COLOUR_TARGET_BINDINGS];
			uint32_t ColourTargetCount;
			SDL_GPUTexture *DepthStencilTarget;
			SDL_GPUGraphicsPipeline *Pipeline;
			bool VertexSamplerBound[SAMPLERS_PER_STAGE];
			bool VertexStorageTextureBound[STORAGE_TEXTURES_PER_STAGE];
			bool VertexStorageBufferBound[STORAGE_BUFFERS_PER_STAGE];
			bool FragmentSamplerBound[SAMPLERS_PER_STAGE];
			bool FragmentStorageTextureBound[STORAGE_TEXTURES_PER_STAGE];
			bool FragmentStorageBufferBound[STORAGE_BUFFERS_PER_STAGE];
		};

		struct MirroredCommonHeader {
			SDL_GPUDevice *Device;
			MirroredRenderPass Render;
			MirroredComputePass Compute;
			MirroredPass Copy;
			bool SwapchainAcquired;
			bool Submitted;
			bool IgnoreValidation;
		};

		struct MirroredCommandBuffer {
			MirroredCommonHeader Common;
			void *Renderer;
			VkCommandBuffer CommandBuffer;
		};

		struct MirroredRenderer {
			VkInstance Instance;
			VkPhysicalDevice PhysicalDevice;
			VkPhysicalDeviceProperties2KHR PhysicalDeviceProperties;
			VkPhysicalDeviceDriverPropertiesKHR PhysicalDeviceDriverProperties;
			VkDevice LogicalDevice;
		};

		VkCommandBuffer CommandHandle(SDL_GPUCommandBuffer *command, const AtomicGpuQueuesV1 *api) {
			return api != nullptr ? api->NativeCommandBuffer(command)
								  : reinterpret_cast<MirroredCommandBuffer *>(command)->CommandBuffer;
		}
	}

	VulkanTimestamps::~VulkanTimestamps() {
		Shutdown();
	}

	void VulkanTimestamps::Shutdown(bool resourcesRetired) {
		if (resourcesRetired && Device != nullptr && DestroyQueryPool != nullptr) {
			auto destroy = reinterpret_cast<PFN_vkDestroyQueryPool>(DestroyQueryPool);
			for (void *pool : Pools) {
				if (pool != nullptr) {
					destroy(static_cast<VkDevice>(Device), static_cast<VkQueryPool>(pool), nullptr);
				}
			}
		}
		for (uint32_t slot = 0; slot < SLOTS; ++slot) {
			Pools[slot] = nullptr;
			InFlight[slot] = false;
			Counts[slot] = 0;
		}
		Device = nullptr;
		QueueApi = nullptr;
		TimestampMask = ~uint64_t{0};
		Queue = 0;
		Period = 0.0;
		Active = NO_SLOT;
		Used = 0;
		DestroyQueryPool = nullptr;
		CmdResetQueryPool = nullptr;
		CmdWriteTimestamp = nullptr;
		GetQueryPoolResults = nullptr;
	}

	bool VulkanTimestamps::Probe(SDL_GPUDevice *device, uint32_t queue) {
		if (device == nullptr || Ready()) {
			return Ready() && queue == Queue;
		}
		if (queue >= ATOMIC_GPU_QUEUE_COUNT) {
			return false;
		}
		const char *driver = SDL_GetGPUDeviceDriver(device);
		if (driver == nullptr || std::string_view(driver) != "vulkan") {
			ENGINE_INFO("gpu timestamps unavailable on the '{}' backend", driver != nullptr ? driver : "?");
			return false;
		}

		PFN_vkGetDeviceProcAddr getDeviceProcAddr = nullptr;
		VkDevice logical = VK_NULL_HANDLE;
		const auto *api = static_cast<const AtomicGpuQueuesV1 *>(SDL_GetPointerProperty(
			SDL_GetGPUDeviceProperties(device), ATOMIC_GPU_QUEUES_DEVICE_PROPERTY, nullptr
		));
		QueueApi = nullptr;
		TimestampMask = ~uint64_t{0};
		if (api != nullptr) {
			if (api->AbiVersion != ATOMIC_GPU_QUEUES_ABI_VERSION ||
				api->StructSize <
					offsetof(AtomicGpuQueuesV1, NativeCommandBuffer) + sizeof(api->NativeCommandBuffer) ||
				api->Device == VK_NULL_HANDLE || api->GetDeviceProcAddr == nullptr ||
				api->NativeCommandBuffer == nullptr || api->Queue[queue] == VK_NULL_HANDLE ||
				api->TimestampValidBits[queue] == 0 || api->TimestampValidBits[queue] > 64) {
				return false;
			}
			Period = api->Properties.limits.timestampPeriod;
			if (!(Period > 0.0)) {
				return false;
			}
			const uint32_t bits = api->TimestampValidBits[queue];
			TimestampMask = bits == 64 ? ~uint64_t{0} : (uint64_t{1} << bits) - 1;
			QueueApi = api;
			logical = api->Device;
			getDeviceProcAddr = api->GetDeviceProcAddr;
		} else {
			if (queue != ATOMIC_GPU_QUEUE_GRAPHICS) {
				return false;
			}
			auto getInstanceProcAddr =
				reinterpret_cast<PFN_vkGetInstanceProcAddr>(SDL_Vulkan_GetVkGetInstanceProcAddr());
			if (getInstanceProcAddr == nullptr) {
				ENGINE_WARN("gpu timestamps unavailable: SDL did not expose vkGetInstanceProcAddr");
				return false;
			}

			SDL_GPUCommandBuffer *probe = SDL_AcquireGPUCommandBuffer(device);
			if (probe == nullptr) {
				return false;
			}
			auto *command = reinterpret_cast<MirroredCommandBuffer *>(probe);
			if (command->Common.Device != device || command->Renderer == nullptr ||
				command->CommandBuffer == VK_NULL_HANDLE) {
				ENGINE_WARN("gpu timestamps unavailable: SDL's Vulkan command prefix changed");
				SDL_SubmitGPUCommandBuffer(probe);
				return false;
			}

			auto *renderer = static_cast<MirroredRenderer *>(command->Renderer);
			if (renderer->Instance == VK_NULL_HANDLE || renderer->PhysicalDevice == VK_NULL_HANDLE ||
				renderer->LogicalDevice == VK_NULL_HANDLE ||
				renderer->PhysicalDeviceProperties.sType != VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2) {
				ENGINE_WARN("gpu timestamps unavailable: SDL's Vulkan renderer prefix changed");
				SDL_SubmitGPUCommandBuffer(probe);
				return false;
			}

			Period = renderer->PhysicalDeviceProperties.properties.limits.timestampPeriod;
			if (Period <= 0.0) {
				SDL_SubmitGPUCommandBuffer(probe);
				return false;
			}

			getDeviceProcAddr = reinterpret_cast<PFN_vkGetDeviceProcAddr>(
				getInstanceProcAddr(renderer->Instance, "vkGetDeviceProcAddr")
			);
			if (getDeviceProcAddr == nullptr) {
				SDL_SubmitGPUCommandBuffer(probe);
				return false;
			}
			logical = renderer->LogicalDevice;
			SDL_SubmitGPUCommandBuffer(probe);
		}
		Queue = queue;

		auto create =
			reinterpret_cast<PFN_vkCreateQueryPool>(getDeviceProcAddr(logical, "vkCreateQueryPool"));
		DestroyQueryPool = reinterpret_cast<void *>(getDeviceProcAddr(logical, "vkDestroyQueryPool"));
		CmdResetQueryPool = reinterpret_cast<void *>(getDeviceProcAddr(logical, "vkCmdResetQueryPool"));
		CmdWriteTimestamp = reinterpret_cast<void *>(getDeviceProcAddr(logical, "vkCmdWriteTimestamp"));
		GetQueryPoolResults = reinterpret_cast<void *>(getDeviceProcAddr(logical, "vkGetQueryPoolResults"));
		if (create == nullptr || DestroyQueryPool == nullptr || CmdResetQueryPool == nullptr ||
			CmdWriteTimestamp == nullptr || GetQueryPoolResults == nullptr) {
			return false;
		}

		VkQueryPoolCreateInfo info{};
		info.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
		info.queryType = VK_QUERY_TYPE_TIMESTAMP;
		info.queryCount = MARKS;
		Device = logical;
		for (uint32_t slot = 0; slot < SLOTS; slot++) {
			VkQueryPool pool = VK_NULL_HANDLE;
			if (create(logical, &info, nullptr, &pool) != VK_SUCCESS) {
				Shutdown();
				return false;
			}
			Pools[slot] = pool;
		}
		ENGINE_INFO("gpu timestamps enabled at {:.3f} ns per tick", Period);
		return true;
	}

	bool VulkanTimestamps::Collect(uint32_t slot, double *times, uint32_t &count) {
		if (!Ready() || slot >= SLOTS || !InFlight[slot] || times == nullptr) {
			return false;
		}
		const uint32_t wanted = Counts[slot];
		uint64_t ticks[MARKS]{};
		auto results = reinterpret_cast<PFN_vkGetQueryPoolResults>(GetQueryPoolResults);
		const VkResult status = results(
			static_cast<VkDevice>(Device),
			static_cast<VkQueryPool>(Pools[slot]),
			0,
			wanted,
			sizeof(ticks),
			ticks,
			sizeof(uint64_t),
			VK_QUERY_RESULT_64_BIT
		);
		if (status != VK_SUCCESS) {
			return false;
		}
		const uint64_t first = ticks[0] & TimestampMask;
		for (uint32_t index = 0; index < wanted; index++) {
			const uint64_t elapsed = (ticks[index] - first) & TimestampMask;
			times[index] = static_cast<double>(elapsed) * Period;
		}
		count = wanted;
		InFlight[slot] = false;
		Counts[slot] = 0;
		return true;
	}

	uint32_t VulkanTimestamps::Begin(SDL_GPUCommandBuffer *command, uint32_t avoid) {
		if (!Ready() || command == nullptr || Active != NO_SLOT) {
			return NO_SLOT;
		}
		const VkCommandBuffer native = CommandHandle(command, QueueApi);
		if (native == VK_NULL_HANDLE) {
			return NO_SLOT;
		}
		for (uint32_t slot = 0; slot < SLOTS; slot++) {
			if (InFlight[slot] || slot == avoid) {
				continue;
			}
			auto reset = reinterpret_cast<PFN_vkCmdResetQueryPool>(CmdResetQueryPool);
			reset(native, static_cast<VkQueryPool>(Pools[slot]), 0, MARKS);
			Active = slot;
			Used = 0;
			return slot;
		}
		return NO_SLOT;
	}

	uint32_t VulkanTimestamps::Mark(SDL_GPUCommandBuffer *command) {
		if (Active == NO_SLOT || command == nullptr || Used >= MARKS) {
			return MARKS;
		}
		const VkCommandBuffer native = CommandHandle(command, QueueApi);
		if (native == VK_NULL_HANDLE) {
			return MARKS;
		}
		auto write = reinterpret_cast<PFN_vkCmdWriteTimestamp>(CmdWriteTimestamp);
		write(native, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, static_cast<VkQueryPool>(Pools[Active]), Used);
		return Used++;
	}

	void VulkanTimestamps::Submitted(uint32_t slot) {
		if (slot >= SLOTS || Active != slot) {
			return;
		}
		Counts[slot] = Used;
		InFlight[slot] = Used > 0;
		Active = NO_SLOT;
		Used = 0;
	}

	void VulkanTimestamps::Abandon(uint32_t slot) {
		if (slot >= SLOTS) {
			return;
		}
		InFlight[slot] = false;
		Counts[slot] = 0;
		if (slot == Active) {
			Active = NO_SLOT;
			Used = 0;
		}
	}

	double VulkanTimestamps::Between(const double *times, uint32_t from, uint32_t to) {
		if (times == nullptr || from >= MARKS || to >= MARKS || to <= from) {
			return 0.0;
		}
		return times[to] - times[from];
	}
}
