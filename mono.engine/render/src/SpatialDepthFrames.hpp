#pragma once

#include "SpatialDepthReadback.hpp"

#include <array>
#include <cstdint>

namespace engine::render {
	struct SpatialDepthBudget {
		uint64_t Used = 0, Limit = 128ull * 1024 * 1024;
		bool Busy = false;
	};
	// One viewport's bounded depth history. The owner publishes only after the
	// matching color frame's fence signals and aborts abandoned recordings.
	// Reset on renderer, world or viewport replacement. Calls stay on the renderer owner thread.
	class SpatialDepthFrames {
	  public:
		static constexpr std::size_t CAPACITY = 3;
		SpatialDepthFrames(SpatialDepthBudget &budget, SpatialDepthReadback &readback)
			: Budget(budget), Readback(readback) {}
		SpatialDepthFrames(const SpatialDepthFrames &) = delete;
		SpatialDepthFrames &operator=(const SpatialDepthFrames &) = delete;
		bool AcceptsDomain(SpatialDepthFrameKey frame) const;
		SpatialDepthFrameStatus Status(SpatialDepthFrameKey frame) const;
		bool SetInfo(SpatialDepthFrameKey frame, const SpatialDepthFrameInfo &info);
		std::optional<SpatialDepthFrameInfo> Info(SpatialDepthFrameKey frame) const;
		bool Reserve(SDL_GPUDevice *device, SpatialDepthFrameKey frame, uint32_t width, uint32_t height);
		bool Record(SDL_GPUCommandBuffer *command, SpatialDepthFrameKey frame, SDL_GPUTexture *source);
		bool Publish(SpatialDepthFrameKey frame);
		// Call only after the command is canceled or its submission fence signals.
		bool Abort(SDL_GPUDevice *device, SpatialDepthFrameKey frame);
		// Makes a reserved frame unavailable without releasing its texture.
		bool Abandon(SpatialDepthFrameKey frame);
		// Call after all commands referencing reserved frames have retired.
		bool AbortAll(SDL_GPUDevice *device);
		// Atomically validate the retained frame and submit its pixel copy.
		uint64_t Queue(SDL_GPUDevice *device, SpatialDepthFrameKey frame, uint32_t x, uint32_t y);
		SpatialDepthReadbackResult Take(SDL_GPUDevice *device, SpatialDepthFrameKey frame, uint64_t token);
		bool Cancel(SpatialDepthFrameKey frame, uint64_t token);
		// Returns false when a reentrant shared readback is busy; retry later.
		bool CancelAll();
		void Reap(SDL_GPUDevice *device);
		// Fails while a recorded frame or readback is in flight.
		bool Shutdown(SDL_GPUDevice *device);

	  private:
		struct Slot {
			SDL_GPUTexture *Depth = nullptr;
			SpatialDepthFrameKey Key;
			SpatialDepthFrameInfo FrameInfo;
			uint32_t Width = 0, Height = 0, Pins = 0;
			bool Reserved = false;
			bool Recorded = false;
			bool Ready = false;
			bool Abandoned = false;
		};
		struct Pin {
			uint64_t Token = 0;
			uint32_t Index = 0;
			bool Cancelled = false;
		};
		std::array<Slot, CAPACITY> Slots{};
		std::array<Pin, SpatialDepthReadback::CAPACITY> Pins{};
		SDL_GPUDevice *Device = nullptr;
		SpatialDepthBudget &Budget;
		SpatialDepthReadback &Readback;
		SpatialDepthFrameKey Domain;
		SpatialDepthFrameKey Published;
		bool Busy = false;
	};
}
