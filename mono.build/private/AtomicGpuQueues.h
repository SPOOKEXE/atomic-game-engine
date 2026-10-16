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
#define ATOMIC_GPU_QUEUES_MAX_RESOURCES 128u

typedef enum AtomicGpuQueueClass {
	ATOMIC_GPU_QUEUE_GRAPHICS = 0,
	ATOMIC_GPU_QUEUE_COMPUTE = 1,
	ATOMIC_GPU_QUEUE_TRANSFER = 2,
	ATOMIC_GPU_QUEUE_COUNT = 3
} AtomicGpuQueueClass;

typedef enum AtomicGpuResourceKind {
	ATOMIC_GPU_RESOURCE_BUFFER = 0,
	ATOMIC_GPU_RESOURCE_TEXTURE = 1,
	ATOMIC_GPU_RESOURCE_SAMPLER = 2,
	ATOMIC_GPU_RESOURCE_GRAPHICS_PIPELINE = 3,
	ATOMIC_GPU_RESOURCE_COMPUTE_PIPELINE = 4
} AtomicGpuResourceKind;

typedef struct AtomicGpuResource {
	uint32_t Kind;
	void *Handle;
} AtomicGpuResource;

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
	 * acquired buffer was submitted or cancelled and every frozen lease released. */
	bool(SDLCALL *BeginRecordingBatch)(SDL_GPUDevice *device);
	bool(SDLCALL *EndRecordingBatch)(SDL_GPUDevice *device);

	/* Owner-only maintenance outside recording. A no-op without pending work;
	 * otherwise waits for all queues and performs SDL's pending defragmentation. */
	bool(SDLCALL *MaintainIdle)(SDL_GPUDevice *device);

	/* Acquire, record, configure, submit or cancel on one thread. Workers bind frozen
	 * resource handles and never cycle their backing. */
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

	/* Owner freezes current backing generations inside a recording batch. Frozen
	 * handles bind through SDL normally; workers must never cycle or release them.
	 * At most MAX_RESOURCES per call. Allocation failure clears every output.
	 * Owner releases only returned handles after workers finish, before ending the
	 * batch. Release clears the outputs. Submitted SDL commands retain their own
	 * backing references until their fences finish. */
	bool(SDLCALL *FreezeResources)(
		SDL_GPUDevice *device, uint32_t count, const AtomicGpuResource *sources, AtomicGpuResource *outputs
	);
	bool(SDLCALL *ReleaseFrozenResources)(
		SDL_GPUDevice *device, uint32_t count, AtomicGpuResource *resources
	);

	/* Owner registers at most eight cleanup callbacks outside recording batches.
	 * Userdata stays alive until SDL_DestroyGPUDevice returns. Each callback runs
	 * once before native device destruction and must not submit work. A true
	 * resourcesRetired permits native child destruction; false requires abandoning
	 * borrowed handles without Vulkan calls after an unconfirmed queue drain. */
	bool(SDLCALL *DeferDeviceCleanup)(
		SDL_GPUDevice *device, void(SDLCALL *cleanup)(void *userdata, bool resourcesRetired), void *userdata
	);

	/* Optional size-tagged tail for preserving a frame's acquired presentation command.
	 * Present only when StructSize covers this field. The command remains caller-owned;
	 * its swapchain barrier, semaphores and present data are not transferred. */
	bool(SDLCALL *BeginRecordingBatchWithPresentation)(
		SDL_GPUDevice *device, SDL_GPUCommandBuffer *presentationCommand
	);
} AtomicGpuQueuesV1;

#endif
