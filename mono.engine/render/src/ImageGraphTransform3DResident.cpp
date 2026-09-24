#include "ImageGraphTransform3DResident.hpp"

#include "RendererState.hpp"

#include <engine/render/Renderer.hpp>

namespace engine::render {
	void Renderer::Impl::ReleaseTransform3D(GraphResourceCache::Transform3DSlot &slot) {
		imagegraph::ReleaseTransformImage3DLive(Device, slot.Resources);
		GraphResources.Transform3DSourceBytes -=
			std::min(GraphResources.Transform3DSourceBytes, slot.SourceBytes);
		GraphResources.Transform3DScratchBytes -=
			std::min(GraphResources.Transform3DScratchBytes, slot.ScratchBytes);
		slot = {};
	}

	void Renderer::Impl::RecordTransform3D(SDL_GPUCommandBuffer *command) {
		if (Device == nullptr || command == nullptr) return;
		for (GraphResourceCache::Transform3DSlot &slot : GraphResources.Transform3D) {
			if (slot.Phase != GraphResourceCache::Transform3DPhase::Queued) continue;
			const uint64_t scratch = imagegraph::TransformImage3DLiveScratchBytes(slot.Request);
			if (scratch > imagegraph::MAXIMUM_TRANSFORM_IMAGE_3D_SCRATCH_BYTES ||
				GraphResources.Transform3DScratchBytes >
					imagegraph::MAXIMUM_TRANSFORM_IMAGE_3D_SCRATCH_BYTES - scratch) {
				// Admission fails before any command references this request, so its old
				// table entry remains available while the new request is discarded.
				ReleaseTransform3D(slot);
				continue;
			}
			const bool succeeded =
				imagegraph::RecordTransformImage3DLive(Device, command, slot.Request, slot.Resources);
			if (!succeeded) {
				if (!slot.Resources.CommandReferenced) {
					ReleaseTransform3D(slot);
					continue;
				}
				// A copy already references this slot in the shared command buffer. Keep
				// its reservation through the fence, including when a later pass fails.
				slot.ScratchBytes = scratch;
				GraphResources.Transform3DScratchBytes += scratch;
				slot.Succeeded = false;
				slot.Phase = GraphResourceCache::Transform3DPhase::Recorded;
				continue;
			}
			slot.ScratchBytes = scratch;
			GraphResources.Transform3DScratchBytes += scratch;
			slot.Succeeded = true;
			slot.Phase = GraphResourceCache::Transform3DPhase::Recorded;
		}
	}
	imagegraph::TransformImage3DQueueResult
	Renderer::QueueTransformImage3D(imagegraph::TransformImage3DLiveRequest request) {
		RequireOwningThread("QueueTransformImage3D");
		if (!State) return imagegraph::TransformImage3DQueueResult::Invalid;
		if (!request.Owner.IsValid() || !request.Name.IsValid() || request.Generation == 0 ||
			imagegraph::ValidateTransformImage3D(request.Request) != imagegraph::TransformImage3DStatus::Ok)
			return imagegraph::TransformImage3DQueueResult::Invalid;
		const uint64_t bytes = request.Request.Front.Rgba8.size() + request.Request.Back.Rgba8.size();
		auto hasBudget = [&](uint64_t replaced) {
			const uint64_t held = State->GraphResources.Transform3DSourceBytes - replaced;
			return bytes <= imagegraph::MAXIMUM_TRANSFORM_IMAGE_3D_OUTPUT_BYTES &&
				   held <= imagegraph::MAXIMUM_TRANSFORM_IMAGE_3D_OUTPUT_BYTES - bytes;
		};
		auto *queuedReplacement = static_cast<Impl::GraphResourceCache::Transform3DSlot *>(nullptr);
		auto *freeSlot = static_cast<Impl::GraphResourceCache::Transform3DSlot *>(nullptr);
		std::array<
			Impl::GraphResourceCache::Transform3DSlot *,
			Impl::GraphResourceCache::TRANSFORM_3D_CAPACITY>
			stale{};
		size_t staleCount = 0;
		for (auto &slot : State->GraphResources.Transform3D) {
			if (slot.Phase == Impl::GraphResourceCache::Transform3DPhase::Free) {
				freeSlot = &slot;
				continue;
			}
			if (slot.Owner != request.Owner || slot.Name != request.Name) continue;
			if (slot.Generation >= request.Generation)
				return imagegraph::TransformImage3DQueueResult::Duplicate;
			if (slot.Phase == Impl::GraphResourceCache::Transform3DPhase::Queued)
				queuedReplacement = &slot;
			else
				stale[staleCount++] = &slot;
		}
		if (queuedReplacement != nullptr && hasBudget(queuedReplacement->SourceBytes)) {
			auto &slot = *queuedReplacement;
			State->GraphResources.Transform3DSourceBytes -= slot.SourceBytes;
			slot = {};
			slot.Phase = Impl::GraphResourceCache::Transform3DPhase::Queued;
			slot.Owner = request.Owner;
			slot.Name = request.Name;
			slot.Generation = request.Generation;
			slot.Width = request.Request.Front.Width;
			slot.Height = request.Request.Front.Height;
			slot.Request = std::move(request.Request);
			slot.SourceBytes = bytes;
			State->GraphResources.Transform3DSourceBytes += bytes;
			for (size_t index = 0; index < staleCount; index++)
				stale[index]->Cancelled = true;
			return imagegraph::TransformImage3DQueueResult::Replaced;
		}
		if (freeSlot == nullptr || !hasBudget(0)) return imagegraph::TransformImage3DQueueResult::Full;
		for (size_t index = 0; index < staleCount; index++)
			stale[index]->Cancelled = true;
		{
			auto &slot = *freeSlot;
			slot.Phase = Impl::GraphResourceCache::Transform3DPhase::Queued;
			slot.Owner = request.Owner;
			slot.Name = request.Name;
			slot.Generation = request.Generation;
			slot.Width = request.Request.Front.Width;
			slot.Height = request.Request.Front.Height;
			slot.Request = std::move(request.Request);
			slot.SourceBytes = bytes;
			State->GraphResources.Transform3DSourceBytes += bytes;
			return imagegraph::TransformImage3DQueueResult::Queued;
		}
	}
	bool Renderer::CancelTransformImage3D(core::Name owner, core::Name name, uint64_t generation) {
		RequireOwningThread("CancelTransformImage3D");
		if (!State) return false;
		for (auto &slot : State->GraphResources.Transform3D)
			if (slot.Owner == owner && slot.Name == name && slot.Generation == generation) {
				if (slot.Phase == Impl::GraphResourceCache::Transform3DPhase::Queued) {
					State->GraphResources.Transform3DSourceBytes -= slot.SourceBytes;
					slot = {};
				} else
					slot.Cancelled = true;
				return true;
			}
		return false;
	}
	void Renderer::DropTransformImage3DOwner(core::Name owner) {
		RequireOwningThread("DropTransformImage3DOwner");
		if (!State) return;
		std::erase_if(State->GraphResources.PublishedTransform3DOutputs, [owner](const auto &output) {
			return output.Owner == owner;
		});
		for (auto &slot : State->GraphResources.Transform3D)
			if (slot.Owner == owner) {
				if (slot.Phase == Impl::GraphResourceCache::Transform3DPhase::Queued) {
					State->GraphResources.Transform3DSourceBytes -= slot.SourceBytes;
					slot = {};
				} else
					slot.Cancelled = true;
			}
	}
	std::array<test_support::TransformImage3DQueueSlotSnapshot, 4>
	test_support::TransformImage3DResidentTestAccess::Slots(const Renderer &renderer) {
		std::array<TransformImage3DQueueSlotSnapshot, 4> result;
		if (!renderer.State) return result;
		for (size_t index = 0; index < result.size(); index++) {
			const auto &slot = renderer.State->GraphResources.Transform3D[index];
			auto &snapshot = result[index];
			snapshot.Phase = static_cast<TransformImage3DQueuePhase>(slot.Phase);
			snapshot.Owner = slot.Owner;
			snapshot.Name = slot.Name;
			snapshot.Generation = slot.Generation;
			snapshot.SourceBytes = slot.SourceBytes;
			snapshot.ScratchBytes = slot.ScratchBytes;
			snapshot.Cancelled = slot.Cancelled;
			snapshot.Request = slot.Request;
		}
		return result;
	}

	uint64_t test_support::TransformImage3DResidentTestAccess::SourceBytes(const Renderer &renderer) {
		return renderer.State ? renderer.State->GraphResources.Transform3DSourceBytes : 0;
	}

	bool test_support::TransformImage3DResidentTestAccess::SetPhase(
		Renderer &renderer, size_t index, TransformImage3DQueuePhase phase
	) {
		if (!renderer.State || index >= renderer.State->GraphResources.Transform3D.size()) return false;
		renderer.State->GraphResources.Transform3D[index].Phase =
			static_cast<Renderer::Impl::GraphResourceCache::Transform3DPhase>(phase);
		return true;
	}

	bool test_support::TransformImage3DResidentTestAccess::SetCancelled(
		Renderer &renderer, size_t index, bool cancelled
	) {
		if (!renderer.State || index >= renderer.State->GraphResources.Transform3D.size()) return false;
		renderer.State->GraphResources.Transform3D[index].Cancelled = cancelled;
		return true;
	}

	bool test_support::TransformImage3DResidentTestAccess::RecordAndSubmit(Renderer &renderer) {
		if (renderer.State == nullptr || renderer.State->Device == nullptr) return false;
		SDL_GPUCommandBuffer *command = SDL_AcquireGPUCommandBuffer(renderer.State->Device);
		if (command == nullptr) return false;
		renderer.State->RecordTransform3D(command);
		return renderer.State->SubmitSceneCommand(command);
	}

	void test_support::TransformImage3DResidentTestAccess::Poll(Renderer &renderer) {
		if (renderer.State != nullptr) renderer.State->PollSceneFrames();
	}

	void *test_support::TransformImage3DResidentTestAccess::PublishedTexture(
		const Renderer &renderer, core::Name owner, core::Name name
	) {
		return renderer.State ? renderer.State->Textures.Find(name, owner) : nullptr;
	}

	uint64_t test_support::TransformImage3DResidentTestAccess::PublishedGeneration(
		const Renderer &renderer, core::Name owner, core::Name name
	) {
		if (!renderer.State) return 0;
		for (const auto &output : renderer.State->GraphResources.PublishedTransform3DOutputs)
			if (output.Owner == owner && output.Name == name) return output.Generation;
		return 0;
	}
}
