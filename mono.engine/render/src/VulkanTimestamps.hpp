#pragma once

// Nonblocking per-pass GPU timestamps for SDL's Vulkan backend.
//
// SDL 3.4.14 has no public GPU query API or native command-buffer handle. This
// adapter mirrors only the pinned backend prefixes needed to reach Vulkan. A
// submodule update must revalidate the mirror. Every failed probe disables GPU
// timing instead of returning invented measurements.

#include <cstdint>

struct SDL_GPUCommandBuffer;
struct SDL_GPUDevice;

namespace engine::render {

	class VulkanTimestamps {
	  public:
		static constexpr uint32_t MARKS = 128;
		static constexpr uint32_t SLOTS = 4;
		static constexpr uint32_t NO_SLOT = SLOTS;

		~VulkanTimestamps();
		void Shutdown();

		bool Probe(SDL_GPUDevice *device);

		bool Ready() const {
			return Pools[0] != nullptr;
		}

		// Collects one completed slot without waiting and makes it reusable.
		bool Collect(uint32_t slot, double *times, uint32_t &count);

		// Starts marks in a free slot. `avoid` names a slot whose reset is already
		// recorded in a later command buffer and therefore cannot carry earlier
		// work. Returns NO_SLOT when every usable slot is in flight.
		uint32_t Begin(SDL_GPUCommandBuffer *command, uint32_t avoid = NO_SLOT);
		uint32_t Mark(SDL_GPUCommandBuffer *command);
		void Submitted(uint32_t slot);
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
		uint32_t Active = NO_SLOT;
		uint32_t Used = 0;

		void *DestroyQueryPool = nullptr;
		void *CmdResetQueryPool = nullptr;
		void *CmdWriteTimestamp = nullptr;
		void *GetQueryPoolResults = nullptr;
	};
}
