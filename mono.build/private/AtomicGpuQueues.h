#ifndef ATOMIC_GPU_QUEUES_H
#define ATOMIC_GPU_QUEUES_H

/* Private project ABI. Obtain the immutable table from SDL GPU device properties.
 * Its handles are borrowed and expire before SDL_DestroyGPUDevice returns. */
#include <SDL3/SDL_gpu.h>

#include <vulkan/vulkan.h>

#define ATOMIC_GPU_QUEUES_ABI_VERSION 1u
#define ATOMIC_GPU_QUEUES_CREATE_PROPERTY "Atomic.gpu.vulkan.enable_queues"
#define ATOMIC_GPU_QUEUES_DEVICE_PROPERTY "Atomic.gpu.vulkan.queues.v1"
#define ATOMIC_GPU_QUEUES_MAX_SEMAPHORES 128u

typedef enum AtomicGpuQueueClass {
	ATOMIC_GPU_QUEUE_GRAPHICS = 0,
	ATOMIC_GPU_QUEUE_COMPUTE = 1,
	ATOMIC_GPU_QUEUE_TRANSFER = 2,
	ATOMIC_GPU_QUEUE_COUNT = 3
} AtomicGpuQueueClass;

typedef struct AtomicGpuQueuesV1 {
	uint32_t StructSize;
	uint32_t AbiVersion;
	VkInstance Instance;
	VkPhysicalDevice PhysicalDevice;
	VkDevice Device;
	PFN_vkGetDeviceProcAddr GetDeviceProcAddr;
	VkPhysicalDeviceProperties Properties;
	uint32_t Family[ATOMIC_GPU_QUEUE_COUNT];
	uint32_t QueueIndex[ATOMIC_GPU_QUEUE_COUNT];
	/* Physical family capacity. Only QueueIndex is created for each class. */
	uint32_t QueueCount[ATOMIC_GPU_QUEUE_COUNT];
	uint32_t TimestampValidBits[ATOMIC_GPU_QUEUE_COUNT];
	VkQueueFlags QueueFlags[ATOMIC_GPU_QUEUE_COUNT];
	VkQueue Queue[ATOMIC_GPU_QUEUE_COUNT];

	/* Owner brackets all worker recording and submission. End refuses until every
	 * acquired buffer was submitted or cancelled on its acquiring thread. */
	bool(SDLCALL *BeginRecordingBatch)(SDL_GPUDevice *device);
	bool(SDLCALL *EndRecordingBatch)(SDL_GPUDevice *device);

	/* Owner-only maintenance outside recording. A no-op without pending work;
	 * otherwise waits for all queues and performs SDL's pending defragmentation. */
	bool(SDLCALL *MaintainIdle)(SDL_GPUDevice *device);

	/* Acquire, record, configure, submit or cancel on one thread. Resources must
	 * keep the same active generation throughout the batch; workers never cycle. */
	SDL_GPUCommandBuffer *(SDLCALL *Acquire)(SDL_GPUDevice *device, uint32_t queueClass);

	/* Copies at most MAX_SEMAPHORES waits and signals each. Semaphores are binary,
	 * created on Device, and caller-owned until every consuming fence completes.
	 * Calls replace graph dependencies while preserving SDL swapchain semaphores.
	 * Wait stages must be VK_PIPELINE_STAGE_ALL_COMMANDS_BIT in ABI version 1.
	 * Every producer must submit successfully before its consumer is submitted. */
	bool(SDLCALL *SetDependencies)(
		SDL_GPUCommandBuffer *command,
		uint32_t waitCount,
		const VkSemaphore *waits,
		const VkPipelineStageFlags *waitStages,
		uint32_t signalCount,
		const VkSemaphore *signals
	);

	/* Borrowed timestamp/debug handle only. Submit and cancel through SDL so
	 * command/resource references and completion cleanup stay intact. */
	VkCommandBuffer(SDLCALL *NativeCommandBuffer)(SDL_GPUCommandBuffer *command);
} AtomicGpuQueuesV1;

#endif
