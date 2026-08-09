#pragma once

// Per-pass GPU time, by reaching into SDL's Vulkan backend.
//
// **`D00103` says this needs an SDL feature that does not exist, and it still
// does.** SDL 3.2.31 has no timestamp query, no query pool and no
// `SDL_GPUQuery`; `SDL_gpu.h` exposes no native handle either, so there is no
// supported route to `vkCmdWriteTimestamp`. What this file does instead is
// mirror the first few fields of two SDL-internal structs and cast the opaque
// pointers it already holds — which is a real cost stated plainly rather than a
// hidden one:
//
// - **It is pinned to one SDL version.** `mono.vendor/sdl` is a submodule at
//   3.2.31 and these layouts are that version's. A submodule bump can silently
//   change them, which is why `Probe` checks what it found rather than trusting
//   it, and why every failure path here is "report nothing" rather than "read
//   whatever is at that address".
// - **It is Vulkan only.** `SDL_GetGPUDeviceDriver` gates it, so a D3D12 or
//   Metal run reports nothing and says so, exactly as before.
// - **It is not a supported use of SDL.** If SDL ships timestamp queries this
//   file is deleted rather than ported.
//
// The alternative was a fork of the submodule, which is a rebase burden forever
// and a repository nobody else can clone from. This is contained to one file
// and one call site.
//
// @tier L12 · client

#include <cstdint>

struct SDL_GPUDevice;
struct SDL_GPUCommandBuffer;

namespace engine::render {

	// GPU timestamps for one device, or nothing when they cannot be had.
	class VulkanTimestamps {
	  public:
		// How many marks one frame may record. Two per pass — a begin and an
		// end — so this is a ceiling on passes, not on draws.
		static constexpr uint32_t MARKS = 128;

		~VulkanTimestamps();

		// Finds the device's Vulkan handles and builds a query pool.
		//
		// **Every check is a reason to give up rather than to continue.** A
		// driver that is not Vulkan, a `vkGetInstanceProcAddr` that will not
		// load, a timestamp period of zero, a queue that does not support
		// timestamps at all — each answers `false` and leaves this dormant.
		//
		// @param device The renderer's device.
		// @return `false` when per-pass GPU time is not available here.
		bool Probe(SDL_GPUDevice *device);

		// Whether `Probe` succeeded.
		bool Ready() const {
			return Pool != nullptr;
		}

		// Starts a frame's marks over.
		//
		// @param command The frame's command buffer.
		void Begin(SDL_GPUCommandBuffer *command);

		// Records one mark.
		//
		// @param command The frame's command buffer.
		// @return The mark's index, or `MARKS` when the frame is full.
		uint32_t Mark(SDL_GPUCommandBuffer *command);

		// Reads back the marks a completed frame recorded.
		//
		// **Not blocking.** A frame whose results are not ready yet answers
		// `false` and is asked again next frame, which is why the caller keeps
		// the mark indices rather than the times.
		//
		// @param into  Filled with `MARKS` nanosecond values.
		// @param count How many marks the frame recorded.
		// @return `false` when the GPU has not finished with them.
		bool Read(double *into, uint32_t count);

		// Nanoseconds between two marks.
		//
		// @param times What `Read` filled in.
		// @param from  The earlier mark.
		// @param to    The later mark.
		// @return The span, or zero when either mark is out of range.
		static double Between(const double *times, uint32_t from, uint32_t to);

	  private:
		void *Device = nullptr; // VkDevice
		void *Pool = nullptr;	// VkQueryPool
		double Period = 0.0;	// Nanoseconds per tick.
		uint32_t Used = 0;

		// Loaded through `SDL_Vulkan_GetVkGetInstanceProcAddr`, which is public
		// SDL and the one supported part of this whole arrangement.
		void *CreateQueryPool = nullptr;
		void *DestroyQueryPool = nullptr;
		void *CmdResetQueryPool = nullptr;
		void *CmdWriteTimestamp = nullptr;
		void *GetQueryPoolResults = nullptr;
	};
}
