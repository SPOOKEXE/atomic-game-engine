#include "SpatialDepthReadback.hpp"

#include "GpuHeap.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace engine::render {
	namespace {
		struct ReadbackGuard {
			bool &Busy;
			explicit ReadbackGuard(bool &busy) : Busy(busy) {
				Busy = true;
			}
			~ReadbackGuard() {
				Busy = false;
			}
		};
	}
	uint64_t SpatialDepthReadback::Queue(
		SDL_GPUDevice *device,
		SDL_GPUTexture *depth,
		SpatialDepthFrameKey frame,
		uint32_t width,
		uint32_t height,
		uint32_t x,
		uint32_t y
	) {
		if (Busy || !device || !depth || !frame.Renderer || !frame.World || !frame.Frame || x >= width ||
			y >= height || !Next)
			return 0;
		ReadbackGuard guard(Busy);
		if (Device && Device != device) return 0;
		Device = device;
		ReapCancelledUnsafe(device);
		auto found = std::find_if(Slots.begin(), Slots.end(), [](const Slot &slot) { return !slot.Fence; });
		if (found == Slots.end()) return 0;
		Slot &slot = *found;
		if (!slot.Transfer) {
			SDL_GPUTransferBufferCreateInfo info{};
			info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD;
			info.size = 256;
			slot.Transfer = gpu::CreateTransferBuffer(device, &info);
			if (!slot.Transfer) return 0;
		}
		auto *command = SDL_AcquireGPUCommandBuffer(device);
		if (!command) return 0;
		auto *pass = SDL_BeginGPUCopyPass(command);
		if (!pass) {
			SDL_CancelGPUCommandBuffer(command);
			return 0;
		}
		SDL_GPUTextureRegion region{};
		region.texture = depth;
		region.x = x;
		region.y = y;
		region.w = region.h = region.d = 1;
		SDL_GPUTextureTransferInfo target{};
		target.transfer_buffer = slot.Transfer;
		target.pixels_per_row = 64; // 256-byte row pitch for D32.
		target.rows_per_layer = 1;
		SDL_DownloadFromGPUTexture(pass, &region, &target);
		SDL_EndGPUCopyPass(pass);
		slot.Fence = SDL_SubmitGPUCommandBufferAndAcquireFence(command);
		if (!slot.Fence) return 0;
		slot.Frame = frame;
		slot.Cancelled = false;
		slot.Token = Next++;
		return slot.Token;
	}

	SpatialDepthReadbackResult SpatialDepthReadback::Take(SDL_GPUDevice *device, uint64_t token) {
		if (!device || !token || (Device && Device != device)) return {};
		if (Busy) return {SpatialDepthReadbackStatus::Pending, {}, 0.0f};
		ReadbackGuard guard(Busy);
		ReapCancelledUnsafe(device);
		auto found = std::find_if(Slots.begin(), Slots.end(), [token](const Slot &slot) {
			return slot.Token == token;
		});
		if (found == Slots.end() || found->Cancelled) return {};
		Slot &slot = *found;
		if (!SDL_QueryGPUFence(device, slot.Fence)) return {SpatialDepthReadbackStatus::Pending, slot.Frame};
		SDL_GPUFence *fence = slot.Fence;
		if (!SDL_WaitForGPUFences(device, true, &fence, 1))
			return {SpatialDepthReadbackStatus::Pending, slot.Frame};
		SpatialDepthReadbackResult result{
			slot.Cancelled ? SpatialDepthReadbackStatus::Cancelled : SpatialDepthReadbackStatus::Failed,
			slot.Frame
		};
		if (!slot.Cancelled) {
			const auto *mapped = SDL_MapGPUTransferBuffer(device, slot.Transfer, false);
			if (mapped) {
				std::memcpy(&result.Depth, mapped, sizeof result.Depth);
				SDL_UnmapGPUTransferBuffer(device, slot.Transfer);
				if (std::isfinite(result.Depth) && result.Depth >= 0 && result.Depth <= 1)
					result.Status = SpatialDepthReadbackStatus::Ready;
			}
		}
		SDL_ReleaseGPUFence(device, slot.Fence);
		slot.Fence = nullptr;
		slot.Token = 0;
		slot.Cancelled = false;
		return result;
	}
	bool SpatialDepthReadback::Cancel(uint64_t token) {
		if (Busy || !token) return false;
		ReadbackGuard guard(Busy);
		auto found = std::find_if(Slots.begin(), Slots.end(), [token](const Slot &slot) {
			return slot.Token == token;
		});
		if (found == Slots.end() || found->Cancelled) return false;
		found->Cancelled = true;
		return true;
	}
	void SpatialDepthReadback::ReapCancelled(SDL_GPUDevice *device) {
		if (Busy || !device || (Device && Device != device)) return;
		ReadbackGuard guard(Busy);
		ReapCancelledUnsafe(device);
	}
	void SpatialDepthReadback::ReapCancelledUnsafe(SDL_GPUDevice *device) {
		for (auto &slot : Slots) {
			if (!slot.Cancelled || !slot.Fence || !SDL_QueryGPUFence(device, slot.Fence)) continue;
			SDL_GPUFence *fence = slot.Fence;
			if (!SDL_WaitForGPUFences(device, true, &fence, 1)) continue;
			SDL_ReleaseGPUFence(device, slot.Fence);
			slot.Fence = nullptr;
			slot.Frame = {};
			slot.Token = 0;
			slot.Cancelled = false;
		}
	}
	bool SpatialDepthReadback::InFlight(uint64_t token) const {
		if (!token) return false;
		if (Busy) return true;
		return std::any_of(Slots.begin(), Slots.end(), [token](const Slot &slot) {
			return slot.Token == token && slot.Fence;
		});
	}
	bool SpatialDepthReadback::Shutdown(SDL_GPUDevice *device) {
		if (Busy || !device || (Device && Device != device)) return false;
		ReadbackGuard guard(Busy);
		ReapCancelledUnsafe(device);
		if (std::any_of(Slots.begin(), Slots.end(), [](const Slot &slot) { return slot.Fence != nullptr; }))
			return false;
		for (auto &slot : Slots) {
			if (slot.Transfer) gpu::ReleaseTransferBuffer(device, slot.Transfer);
			slot = {};
		}
		Device = nullptr;
		return true;
	}
}
