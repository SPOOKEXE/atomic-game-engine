#pragma once

#include <engine/render/Renderer.hpp>

#include <SDL3/SDL_gpu.h>

#include <array>
#include <cstddef>
#include <cstdint>

namespace engine::render {

	// Renderer-owned. The device must be idle before Shutdown.
	class SpatialDepthReadback {
	  public:
		static constexpr std::size_t CAPACITY = 8;
		SpatialDepthReadback() = default;
		SpatialDepthReadback(const SpatialDepthReadback &) = delete;
		SpatialDepthReadback &operator=(const SpatialDepthReadback &) = delete;
		SpatialDepthReadback(SpatialDepthReadback &&) = delete;
		SpatialDepthReadback &operator=(SpatialDepthReadback &&) = delete;
		uint64_t Queue(
			SDL_GPUDevice *device,
			SDL_GPUTexture *depth,
			SpatialDepthFrameKey frame,
			uint32_t width,
			uint32_t height,
			uint32_t x,
			uint32_t y
		);
		SpatialDepthReadbackResult Take(SDL_GPUDevice *device, uint64_t token);
		// Cancellation consumes the token. The slot is reaped after its fence signals.
		bool Cancel(uint64_t token);
		void ReapCancelled(SDL_GPUDevice *device);
		bool InFlight(uint64_t token) const;
		bool Shutdown(SDL_GPUDevice *device);

	  private:
		struct Slot {
			SDL_GPUTransferBuffer *Transfer = nullptr;
			SDL_GPUFence *Fence = nullptr;
			SpatialDepthFrameKey Frame;
			uint64_t Token = 0;
			bool Cancelled = false;
		};
		std::array<Slot, CAPACITY> Slots{};
		void ReapCancelledUnsafe(SDL_GPUDevice *device);
		SDL_GPUDevice *Device = nullptr;
		uint64_t Next = 1;
		bool Busy = false;
	};
}
