#include "SpatialDepthFrames.hpp"

#include "GpuHeap.hpp"

#include <algorithm>

namespace engine::render {
	namespace {
		uint64_t Bytes(uint32_t width, uint32_t height) {
			return uint64_t(width) * height * 4;
		}
		struct Guard {
			bool &Busy;
			explicit Guard(bool &busy) : Busy(busy) {
				Busy = true;
			}
			~Guard() {
				Busy = false;
			}
		};
	}
	bool SpatialDepthFrames::AcceptsDomain(SpatialDepthFrameKey frame) const {
		return !Domain.Renderer || (Domain.Renderer == frame.Renderer && Domain.World == frame.World &&
									Domain.Viewport == frame.Viewport);
	}
	SpatialDepthFrameStatus SpatialDepthFrames::Status(SpatialDepthFrameKey frame) const {
		if (Busy) return SpatialDepthFrameStatus::Pending;
		const auto found =
			std::find_if(Slots.begin(), Slots.end(), [&](const Slot &slot) { return slot.Key == frame; });
		if (found == Slots.end()) return SpatialDepthFrameStatus::Unknown;
		if (found->Reserved)
			return found->Abandoned ? SpatialDepthFrameStatus::Unknown : SpatialDepthFrameStatus::Pending;
		return found->Ready ? SpatialDepthFrameStatus::Ready : SpatialDepthFrameStatus::Unknown;
	}
	bool SpatialDepthFrames::SetInfo(SpatialDepthFrameKey frame, const SpatialDepthFrameInfo &info) {
		if (Busy || info.Key != frame || !info.SnapshotSequence) return false;
		Guard guard(Busy);
		auto found = std::find_if(Slots.begin(), Slots.end(), [&](const Slot &slot) {
			return slot.Reserved && slot.Key == frame;
		});
		if (found == Slots.end() || info.Width != found->Width || info.Height != found->Height) return false;
		found->FrameInfo = info;
		return true;
	}
	std::optional<SpatialDepthFrameInfo> SpatialDepthFrames::Info(SpatialDepthFrameKey frame) const {
		if (Busy) return {};
		const auto found = std::find_if(Slots.begin(), Slots.end(), [&](const Slot &slot) {
			return slot.Ready && slot.Key == frame;
		});
		if (found == Slots.end() || found->FrameInfo.Key != frame) return {};
		return found->FrameInfo;
	}
	bool SpatialDepthFrames::Reserve(
		SDL_GPUDevice *device, SpatialDepthFrameKey frame, uint32_t width, uint32_t height
	) {
		if (Busy || Budget.Busy || !device || !frame.Renderer || !frame.World || !frame.Frame || !width ||
			!height)
			return false;
		Guard guard(Busy);
		Guard budgetGuard(Budget.Busy);
		if (Device && Device != device) return false;
		Device = device;
		if (uint64_t(width) > (Budget.Limit / 4) / height) return false;
		if (!AcceptsDomain(frame)) return false;
		if (!Domain.Renderer) {
			Domain = frame;
			Domain.Frame = 0;
		}
		if (std::any_of(Slots.begin(), Slots.end(), [&](const Slot &slot) { return slot.Key == frame; }))
			return false;
		auto found = std::find_if(Slots.begin(), Slots.end(), [&](const Slot &slot) {
			return !slot.Reserved && !slot.Pins && !(slot.Ready && slot.Key == Published);
		});
		if (found == Slots.end()) return false;
		Slot &slot = *found;
		// Invalidate the previous identity before recording can reuse its image.
		slot.Ready = false;
		slot.Key = {};
		slot.FrameInfo = {};
		if (slot.Depth && (slot.Width != width || slot.Height != height)) {
			gpu::ReleaseTexture(device, slot.Depth);
			Budget.Used -= Bytes(slot.Width, slot.Height);
			slot.Depth = nullptr;
		}
		if (!slot.Depth) {
			const uint64_t bytes = Bytes(width, height);
			if (bytes > Budget.Limit || Budget.Used > Budget.Limit - bytes) return false;
			SDL_GPUTextureCreateInfo info{};
			info.type = SDL_GPU_TEXTURETYPE_2D;
			info.format = SDL_GPU_TEXTUREFORMAT_D32_FLOAT;
			info.usage = SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET;
			info.width = width;
			info.height = height;
			info.layer_count_or_depth = info.num_levels = 1;
			slot.Depth = gpu::CreateTexture(device, &info);
			if (!slot.Depth) {
				slot.Width = slot.Height = 0;
				return false;
			}
			Budget.Used += bytes;
			slot.Width = width;
			slot.Height = height;
		}
		slot.Key = frame;
		slot.Reserved = true;
		slot.Recorded = false;
		slot.Abandoned = false;
		return true;
	}
	bool SpatialDepthFrames::Record(
		SDL_GPUCommandBuffer *command, SpatialDepthFrameKey frame, SDL_GPUTexture *source
	) {
		if (Busy || !command || !source) return false;
		Guard guard(Busy);
		auto found = std::find_if(Slots.begin(), Slots.end(), [&](const Slot &slot) {
			return slot.Reserved && slot.Key == frame;
		});
		if (found == Slots.end() || found->Recorded || source == found->Depth) return false;
		auto *pass = SDL_BeginGPUCopyPass(command);
		if (!pass) return false;
		SDL_GPUTextureLocation from{}, to{};
		from.texture = source;
		to.texture = found->Depth;
		SDL_CopyGPUTextureToTexture(pass, &from, &to, found->Width, found->Height, 1, false);
		SDL_EndGPUCopyPass(pass);
		found->Recorded = true;
		return true;
	}
	bool SpatialDepthFrames::Publish(SpatialDepthFrameKey frame) {
		if (Busy) return false;
		Guard guard(Busy);
		auto found = std::find_if(Slots.begin(), Slots.end(), [&](const Slot &slot) {
			return slot.Reserved && slot.Key == frame;
		});
		if (found == Slots.end() || !found->Recorded || found->Abandoned || found->FrameInfo.Key != frame)
			return false;
		found->Reserved = false;
		found->Ready = !Published.Frame || frame.Frame > Published.Frame;
		if (found->Ready) Published = frame;
		return true;
	}
	bool SpatialDepthFrames::Abort(SDL_GPUDevice *device, SpatialDepthFrameKey frame) {
		if (Busy || Budget.Busy || !device || (Device && Device != device)) return false;
		Guard guard(Busy);
		Guard budgetGuard(Budget.Busy);
		auto found = std::find_if(Slots.begin(), Slots.end(), [&](const Slot &slot) {
			return slot.Reserved && slot.Key == frame;
		});
		if (found == Slots.end()) return false;
		if (found->Depth) {
			gpu::ReleaseTexture(device, found->Depth);
			Budget.Used -= Bytes(found->Width, found->Height);
		}
		*found = {};
		return true;
	}
	bool SpatialDepthFrames::Abandon(SpatialDepthFrameKey frame) {
		const auto found = std::find_if(Slots.begin(), Slots.end(), [&](const Slot &slot) {
			return slot.Reserved && slot.Key == frame;
		});
		if (found == Slots.end()) return false;
		found->Abandoned = true;
		return true;
	}
	bool SpatialDepthFrames::AbortAll(SDL_GPUDevice *device) {
		if (Busy || Budget.Busy || !device || (Device && Device != device)) return false;
		Guard guard(Busy);
		Guard budgetGuard(Budget.Busy);
		for (auto &slot : Slots) {
			if (!slot.Reserved) continue;
			if (slot.Depth) {
				gpu::ReleaseTexture(device, slot.Depth);
				Budget.Used -= Bytes(slot.Width, slot.Height);
			}
			slot = {};
		}
		return true;
	}
	uint64_t
	SpatialDepthFrames::Queue(SDL_GPUDevice *device, SpatialDepthFrameKey frame, uint32_t x, uint32_t y) {
		if (Busy || !device || (Device && Device != device)) return 0;
		Guard guard(Busy);
		auto found = std::find_if(Slots.begin(), Slots.end(), [&](const Slot &slot) {
			return slot.Ready && slot.Key == frame;
		});
		auto pin = std::find_if(Pins.begin(), Pins.end(), [](const Pin &entry) { return !entry.Token; });
		if (found == Slots.end() || pin == Pins.end()) return 0;
		const uint64_t token = Readback.Queue(device, found->Depth, frame, found->Width, found->Height, x, y);
		if (!token) return 0;
		++found->Pins;
		*pin = {token, static_cast<uint32_t>(found - Slots.begin()), false};
		return token;
	}
	SpatialDepthReadbackResult
	SpatialDepthFrames::Take(SDL_GPUDevice *device, SpatialDepthFrameKey frame, uint64_t token) {
		if (!device || !token || (Device && Device != device)) return {};
		if (Busy) return {SpatialDepthReadbackStatus::Pending, {}, 0.0f};
		Guard guard(Busy);
		auto found =
			std::find_if(Pins.begin(), Pins.end(), [token](const Pin &pin) { return pin.Token == token; });
		if (found == Pins.end() || found->Cancelled || Slots[found->Index].Key != frame) return {};
		auto result = Readback.Take(device, token);
		if (result.Status != SpatialDepthReadbackStatus::Pending) {
			--Slots[found->Index].Pins;
			*found = {};
		}
		return result;
	}
	bool SpatialDepthFrames::Cancel(SpatialDepthFrameKey frame, uint64_t token) {
		if (Busy || !token) return false;
		Guard guard(Busy);
		auto found =
			std::find_if(Pins.begin(), Pins.end(), [token](const Pin &pin) { return pin.Token == token; });
		if (found == Pins.end() || found->Cancelled || Slots[found->Index].Key != frame ||
			!Readback.Cancel(token))
			return false;
		found->Cancelled = true;
		return true;
	}
	bool SpatialDepthFrames::CancelAll() {
		if (Busy) return false;
		Guard guard(Busy);
		bool complete = true;
		for (auto &pin : Pins)
			if (pin.Token && !pin.Cancelled) {
				if (Readback.Cancel(pin.Token))
					pin.Cancelled = true;
				else
					complete = false;
			}
		return complete;
	}
	void SpatialDepthFrames::Reap(SDL_GPUDevice *device) {
		if (Busy || !device || (Device && Device != device)) return;
		Guard guard(Busy);
		Readback.ReapCancelled(device);
		for (auto &pin : Pins)
			if (pin.Token && pin.Cancelled && !Readback.InFlight(pin.Token)) {
				--Slots[pin.Index].Pins;
				pin = {};
			}
	}
	bool SpatialDepthFrames::Shutdown(SDL_GPUDevice *device) {
		if (Busy || Budget.Busy || !device || (Device && Device != device)) return false;
		Guard guard(Busy);
		Guard budgetGuard(Budget.Busy);
		if (std::any_of(Slots.begin(), Slots.end(), [](const Slot &slot) {
				return slot.Reserved || slot.Pins;
			}))
			return false;
		for (auto &slot : Slots) {
			if (slot.Depth) {
				gpu::ReleaseTexture(device, slot.Depth);
				Budget.Used -= Bytes(slot.Width, slot.Height);
			}
			slot = {};
		}
		Published = {};
		Domain = {};
		Device = nullptr;
		return true;
	}
}
