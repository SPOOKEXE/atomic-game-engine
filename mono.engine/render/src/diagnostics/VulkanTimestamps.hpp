#pragma once

// Nonblocking per-pass GPU timestamps for SDL's Vulkan backend.
//
// The private queue descriptor supplies native handles when enabled. Ordinary
// SDL uses pinned backend prefixes for the graphics queue only. A submodule
// update must revalidate that fallback. Failed probes disable GPU timing.

#include <cstdint>

struct SDL_GPUCommandBuffer;
struct SDL_GPUDevice;
struct AtomicGpuQueuesV1;

namespace engine::render {

	class VulkanTimestamps {
	  public:
		// The eight-view recording benchmark needs 202 marks; larger frames still report overflow.
		static constexpr uint32_t MARKS = 256;
		static constexpr uint32_t SLOTS = 4;
		static constexpr uint32_t NO_SLOT = SLOTS;

		~VulkanTimestamps();
		// A failed backend teardown forgets handles without issuing calls on a destroyed device.
		void Shutdown(bool resourcesRetired = true);

		bool Probe(SDL_GPUDevice *device, uint32_t queue = 0);

		bool Ready() const {
			return Pools[0] != nullptr;
		}

		// Collects one completed slot without waiting and makes it reusable. Times
		// are nanoseconds relative to the slot's first timestamp, including wrap.
		bool Collect(uint32_t slot, double *times, uint32_t &count);

		// Starts marks in a free slot. `avoid` names a slot whose reset is already
		// recorded in a later command buffer and therefore cannot carry earlier
		// work. Returns NO_SLOT when every usable slot is in flight.
		uint32_t Begin(SDL_GPUCommandBuffer *command, uint32_t avoid = NO_SLOT);
		uint32_t Mark(SDL_GPUCommandBuffer *command);
		void Submitted(uint32_t slot);
		// Discards recorded marks. Caller ensures no command using this slot remains
		// in GPU flight, either because none was submitted or the device was drained.
		void Abandon(uint32_t slot);

		bool Pending(uint32_t slot) const {
			return slot < SLOTS && InFlight[slot];
		}

		static double Between(const double *times, uint32_t from, uint32_t to);

	  private:
		void *Device = nullptr;
		void *Pools[SLOTS]{};
		bool InFlight[SLOTS]{};
		uint32_t Counts[SLOTS]{};
		double Period = 0.0;
		uint64_t TimestampMask = ~uint64_t{0};
		uint32_t Queue = 0;
		const AtomicGpuQueuesV1 *QueueApi = nullptr;
		uint32_t Active = NO_SLOT;
		uint32_t Used = 0;

		void *DestroyQueryPool = nullptr;
		void *CmdResetQueryPool = nullptr;
		void *CmdWriteTimestamp = nullptr;
		void *GetQueryPoolResults = nullptr;
	};
}
