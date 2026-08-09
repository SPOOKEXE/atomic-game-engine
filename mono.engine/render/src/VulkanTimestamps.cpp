#include "VulkanTimestamps.hpp"

#include <engine/core/Log.hpp>

#include <SDL3/SDL_gpu.h>
#include <SDL3/SDL_vulkan.h>

#include <cstring>
#include <string_view>
#include <vulkan/vulkan.h>

namespace engine::render {

	namespace {

		// --- SDL 3.2.31's internal layouts, mirrored ------------------------
		//
		// **Only the prefixes, and only as far as the two fields wanted.**
		// Everything below is copied from `mono.vendor/sdl/src/gpu/SDL_sysgpu.h`
		// and `src/gpu/vulkan/SDL_gpu_vulkan.c` at the pinned commit. The tails
		// are cut off deliberately: a shorter mirror is a smaller surface to be
		// wrong about, and nothing here reads past `commandBuffer`.

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

		// The first three fields of `VulkanCommandBuffer`.
		struct MirroredCommandBuffer {
			MirroredCommonHeader Common;
			void *Renderer;
			VkCommandBuffer CommandBuffer;
		};

		// The first five fields of `VulkanRenderer`. The two property structs
		// are by value, which is why this needs the real Vulkan headers rather
		// than opaque pointers — their sizes are the offset to the device.
		struct MirroredRenderer {
			VkInstance Instance;
			VkPhysicalDevice PhysicalDevice;
			VkPhysicalDeviceProperties2KHR PhysicalDeviceProperties;
			VkPhysicalDeviceDriverPropertiesKHR PhysicalDeviceDriverProperties;
			VkDevice LogicalDevice;
		};
	}

	VulkanTimestamps::~VulkanTimestamps() {
		if (Pool != nullptr && Device != nullptr && DestroyQueryPool != nullptr) {
			auto destroy = reinterpret_cast<PFN_vkDestroyQueryPool>(DestroyQueryPool);
			destroy(static_cast<VkDevice>(Device), static_cast<VkQueryPool>(Pool), nullptr);
		}
	}

	bool VulkanTimestamps::Probe(SDL_GPUDevice *device) {
		if (device == nullptr) {
			return false;
		}

		// **The driver name first, because everything after it is a lie on any
		// other backend.** A D3D12 command buffer cast to the Vulkan mirror is
		// not a wrong number, it is a wild pointer.
		const char *driver = SDL_GetGPUDeviceDriver(device);
		if (driver == nullptr || std::string_view(driver) != "vulkan") {
			ENGINE_INFO(
				"gpu timestamps: not measured — the '{}' backend has no path",
				driver != nullptr ? driver : "?"
			);
			return false;
		}

		auto getInstanceProcAddr =
			reinterpret_cast<PFN_vkGetInstanceProcAddr>(SDL_Vulkan_GetVkGetInstanceProcAddr());
		if (getInstanceProcAddr == nullptr) {
			ENGINE_WARN("gpu timestamps: not measured — SDL would not hand out vkGetInstanceProcAddr");
			return false;
		}

		// **Acquired to reach the command buffer, and released immediately.**
		// The renderer pointer inside it is what carries the device, and there
		// is no other way to it: `SDL_gpu.h` exposes no native handle.
		SDL_GPUCommandBuffer *probe = SDL_AcquireGPUCommandBuffer(device);
		if (probe == nullptr) {
			return false;
		}

		auto *mirrored = reinterpret_cast<MirroredCommandBuffer *>(probe);
		auto *renderer = static_cast<MirroredRenderer *>(mirrored->Renderer);

		// **Checked rather than trusted.** If a submodule bump moved these
		// fields, what is at these offsets is not a device and not an instance,
		// and the cheapest way to notice is that the header SDL fills in still
		// says what it should.
		const bool plausible =
			renderer != nullptr && renderer->Instance != VK_NULL_HANDLE &&
			renderer->PhysicalDevice != VK_NULL_HANDLE && renderer->LogicalDevice != VK_NULL_HANDLE &&
			renderer->PhysicalDeviceProperties.sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2 &&
			mirrored->Common.Device == device && mirrored->CommandBuffer != VK_NULL_HANDLE;

		if (!plausible) {
			ENGINE_WARN(
				"gpu timestamps: not measured — SDL's internals are not the shape this was built "
				"against, so the submodule has moved. See D00103"
			);
			SDL_SubmitGPUCommandBuffer(probe);
			return false;
		}

		// Nanoseconds per tick. Zero means the device does not support
		// timestamps at all, which is legal.
		Period = static_cast<double>(renderer->PhysicalDeviceProperties.properties.limits.timestampPeriod);
		if (Period <= 0.0) {
			ENGINE_INFO("gpu timestamps: not measured — this device reports no timestamp period");
			SDL_SubmitGPUCommandBuffer(probe);
			return false;
		}

		Device = renderer->LogicalDevice;

		CreateQueryPool =
			reinterpret_cast<void *>(getInstanceProcAddr(renderer->Instance, "vkCreateQueryPool"));
		DestroyQueryPool =
			reinterpret_cast<void *>(getInstanceProcAddr(renderer->Instance, "vkDestroyQueryPool"));
		CmdResetQueryPool =
			reinterpret_cast<void *>(getInstanceProcAddr(renderer->Instance, "vkCmdResetQueryPool"));
		CmdWriteTimestamp =
			reinterpret_cast<void *>(getInstanceProcAddr(renderer->Instance, "vkCmdWriteTimestamp"));
		GetQueryPoolResults =
			reinterpret_cast<void *>(getInstanceProcAddr(renderer->Instance, "vkGetQueryPoolResults"));

		SDL_SubmitGPUCommandBuffer(probe);

		if (CreateQueryPool == nullptr || DestroyQueryPool == nullptr || CmdResetQueryPool == nullptr ||
			CmdWriteTimestamp == nullptr || GetQueryPoolResults == nullptr) {
			ENGINE_WARN("gpu timestamps: not measured — the timestamp entry points would not load");
			Device = nullptr;
			return false;
		}

		VkQueryPoolCreateInfo info{};
		info.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
		info.queryType = VK_QUERY_TYPE_TIMESTAMP;
		info.queryCount = MARKS;

		VkQueryPool pool = VK_NULL_HANDLE;
		auto create = reinterpret_cast<PFN_vkCreateQueryPool>(CreateQueryPool);
		if (create(static_cast<VkDevice>(Device), &info, nullptr, &pool) != VK_SUCCESS) {
			ENGINE_WARN("gpu timestamps: not measured — the query pool would not be created");
			Device = nullptr;
			return false;
		}

		Pool = pool;
		ENGINE_INFO("gpu timestamps: measuring, {} ns a tick over {} marks", Period, MARKS);
		return true;
	}

	void VulkanTimestamps::Begin(SDL_GPUCommandBuffer *command) {
		if (!Ready() || command == nullptr) {
			return;
		}

		// **Reset on the GPU timeline, not the CPU's.** The pool is read a frame
		// or more later, so resetting it from the host would race the read of
		// the frame still in flight.
		auto reset = reinterpret_cast<PFN_vkCmdResetQueryPool>(CmdResetQueryPool);
		reset(
			reinterpret_cast<MirroredCommandBuffer *>(command)->CommandBuffer,
			static_cast<VkQueryPool>(Pool),
			0,
			MARKS
		);
		Used = 0;
	}

	uint32_t VulkanTimestamps::Mark(SDL_GPUCommandBuffer *command) {
		if (!Ready() || command == nullptr || Used >= MARKS) {
			return MARKS;
		}

		// **Bottom of pipe.** A top-of-pipe mark records when the command was
		// *reached*, which for a pass that is still executing is not when it
		// finished — and the difference between two of those is not the pass's
		// cost.
		auto write = reinterpret_cast<PFN_vkCmdWriteTimestamp>(CmdWriteTimestamp);
		write(
			reinterpret_cast<MirroredCommandBuffer *>(command)->CommandBuffer,
			VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
			static_cast<VkQueryPool>(Pool),
			Used
		);
		return Used++;
	}

	bool VulkanTimestamps::Read(double *into, uint32_t count) {
		if (!Ready() || into == nullptr || count == 0 || count > MARKS) {
			return false;
		}

		uint64_t ticks[MARKS]{};
		auto results = reinterpret_cast<PFN_vkGetQueryPoolResults>(GetQueryPoolResults);

		// **Without `WAIT`, so a frame still running answers "not yet".** The
		// caller asks again next frame; blocking here would serialise the CPU
		// against the GPU to report how fast the GPU is, which is the classic
		// way a profiler becomes the thing it is measuring.
		const VkResult status = results(
			static_cast<VkDevice>(Device),
			static_cast<VkQueryPool>(Pool),
			0,
			count,
			sizeof(ticks),
			ticks,
			sizeof(uint64_t),
			VK_QUERY_RESULT_64_BIT
		);
		if (status != VK_SUCCESS) {
			return false;
		}

		for (uint32_t index = 0; index < count; index++) {
			into[index] = static_cast<double>(ticks[index]) * Period;
		}
		return true;
	}

	double VulkanTimestamps::Between(const double *times, uint32_t from, uint32_t to) {
		if (times == nullptr || from >= MARKS || to >= MARKS || to <= from) {
			return 0.0;
		}
		return times[to] - times[from];
	}
}
