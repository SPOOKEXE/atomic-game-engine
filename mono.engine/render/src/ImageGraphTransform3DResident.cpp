#include "ImageGraphTransform3DResident.hpp"

#include "ImageGraphTransform3DFormats.hpp"
#include "RendererState.hpp"

#include <engine/render/Renderer.hpp>

#include <limits>
#include <new>

namespace engine::render {
	void Renderer::Impl::ReleaseTransform3D(GraphResourceCache::Transform3DSlot &slot) {
		imagegraph::ReleaseTransformImage3DLive(Device, slot.Resources);
		imagegraph::ReleaseSourceCamera3D(Device, slot.CameraResources);
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
			const uint64_t scratch = slot.SdfRequest ? imagegraph::SourceSdfScratchBytes(*slot.SdfRequest)
									 : slot.CameraRequest
										 ? imagegraph::SourceCamera3DScratchBytes(*slot.CameraRequest)
										 : imagegraph::TransformImage3DLiveScratchBytes(slot.Request);
			if (scratch > imagegraph::MAXIMUM_TRANSFORM_IMAGE_3D_SCRATCH_BYTES ||
				GraphResources.Transform3DScratchBytes >
					imagegraph::MAXIMUM_TRANSFORM_IMAGE_3D_SCRATCH_BYTES - scratch) {
				// Admission fails before any command references this request, so its old
				// table entry remains available while the new request is discarded.
				ReleaseTransform3D(slot);
				continue;
			}
			const bool succeeded =
				slot.SdfRequest
					? imagegraph::RecordSourceSdf(Device, command, *slot.SdfRequest, slot.CameraResources)
				: slot.CameraRequest
					? imagegraph::RecordSourceCamera3D(
						  Device, command, *slot.CameraRequest, slot.CameraResources
					  )
					: imagegraph::RecordTransformImage3DLive(Device, command, slot.Request, slot.Resources);
			if (!succeeded) {
				if (!slot.Resources.CommandReferenced && !slot.CameraResources.CommandReferenced) {
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
			(request.Output != imagegraph::TransformImage3DOutput::Rendered &&
			 request.Output != imagegraph::TransformImage3DOutput::Depth) ||
			imagegraph::ValidateTransformImage3D(request.Request) != imagegraph::TransformImage3DStatus::Ok)
			return imagegraph::TransformImage3DQueueResult::Invalid;
		if (State->Device != nullptr && !imagegraph::detail::SupportsTransformImage3DFormats(
											State->Device,
											request.Request.Front.Format,
											request.Request.Back.Format,
											!request.Request.Back.Pixels.empty(),
											request.Request.ColorSpace
										))
			return imagegraph::TransformImage3DQueueResult::Invalid;
		const uint64_t frontBytes = request.Request.Front.Pixels.size();
		const uint64_t backBytes = request.Request.Back.Pixels.size();
		if (backBytes > std::numeric_limits<uint64_t>::max() - frontBytes)
			return imagegraph::TransformImage3DQueueResult::Invalid;
		const uint64_t bytes = frontBytes + backBytes;
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
			if (slot.Owner != request.Owner) continue;
			if (slot.CameraRequest) {
				for (const auto &binding : slot.CameraBindings)
					if (!binding.Cancelled && binding.Name == request.Name &&
						binding.Generation >= request.Generation)
						return imagegraph::TransformImage3DQueueResult::Duplicate;
				// Other camera aliases continue using their shared pass when one name is rebound.
				continue;
			}
			if (slot.Name != request.Name) continue;
			if (slot.Generation >= request.Generation)
				return imagegraph::TransformImage3DQueueResult::Duplicate;
			if (slot.Phase == Impl::GraphResourceCache::Transform3DPhase::Queued)
				queuedReplacement = &slot;
			else
				stale[staleCount++] = &slot;
		}
		for (const auto &published : State->GraphResources.PublishedTransform3DOutputs)
			if (published.Owner == request.Owner && published.Name == request.Name &&
				published.Generation >= request.Generation)
				return imagegraph::TransformImage3DQueueResult::Duplicate;
		auto cancelCameraAlias = [&] {
			for (auto &slot : State->GraphResources.Transform3D)
				if (slot.CameraRequest && slot.Owner == request.Owner)
					for (auto &binding : slot.CameraBindings)
						if (binding.Name == request.Name) binding.Cancelled = true;
		};
		if (queuedReplacement != nullptr && hasBudget(queuedReplacement->SourceBytes)) {
			auto &slot = *queuedReplacement;
			State->GraphResources.Transform3DSourceBytes -= slot.SourceBytes;
			slot = {};
			slot.Phase = Impl::GraphResourceCache::Transform3DPhase::Queued;
			slot.Owner = request.Owner;
			slot.Name = request.Name;
			slot.Generation = request.Generation;
			slot.Output = request.Output;
			slot.Width = request.Request.Front.Width;
			slot.Height = request.Request.Front.Height;
			slot.Request = std::move(request.Request);
			slot.SourceBytes = bytes;
			State->GraphResources.Transform3DSourceBytes += bytes;
			for (size_t index = 0; index < staleCount; index++)
				stale[index]->Cancelled = true;
			cancelCameraAlias();
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
			slot.Output = request.Output;
			slot.Width = request.Request.Front.Width;
			slot.Height = request.Request.Front.Height;
			slot.Request = std::move(request.Request);
			slot.SourceBytes = bytes;
			State->GraphResources.Transform3DSourceBytes += bytes;
			cancelCameraAlias();
			return imagegraph::TransformImage3DQueueResult::Queued;
		}
	}

	imagegraph::TransformImage3DQueueResult
	Renderer::QueueSourceCamera3D(imagegraph::SourceCamera3DLiveRequest request) {
		RequireOwningThread("QueueSourceCamera3D");
		using Result = imagegraph::TransformImage3DQueueResult;
		if (!State || !request.Owner.IsValid() || !request.Name.IsValid() || request.Generation == 0 ||
			imagegraph::ValidateSourceCamera3D(request.Request) != imagegraph::SourceCamera3DStatus::Ok)
			return Result::Invalid;
		const auto output = request.Request.Output;
		request.Request.Output = imagegraph::SourceCamera3DOutput::Rendered;
		const uint64_t bytes = imagegraph::SourceCamera3DSourceBytes(request.Request);
		if (bytes > imagegraph::MAXIMUM_TRANSFORM_IMAGE_3D_OUTPUT_BYTES) return Result::Full;
		using Slot = Impl::GraphResourceCache::Transform3DSlot;
		using Phase = Impl::GraphResourceCache::Transform3DPhase;
		Slot *replacement = nullptr, *free = nullptr, *merged = nullptr;
		for (auto &slot : State->GraphResources.Transform3D) {
			if (slot.Phase == Phase::Free) {
				free = &slot;
				continue;
			}
			if (slot.Owner != request.Owner) continue;
			if (slot.CameraRequest) {
				for (const auto &binding : slot.CameraBindings)
					if (!binding.Cancelled && binding.Name == request.Name &&
						binding.Generation >= request.Generation)
						return Result::Duplicate;
				const bool availableOutput = std::none_of(
					slot.CameraBindings.begin(), slot.CameraBindings.end(), [&](const auto &binding) {
						return !binding.Cancelled && binding.Output == output && binding.Name != request.Name;
					}
				);
				if (slot.Phase == Phase::Queued && !slot.Cancelled && availableOutput &&
					*slot.CameraRequest == request.Request)
					merged = &slot;
				if (slot.Phase == Phase::Queued && slot.CameraBindings.size() == 1 &&
					slot.CameraBindings[0].Name == request.Name)
					replacement = &slot;
			} else if (slot.Name == request.Name) {
				if (slot.Generation >= request.Generation) return Result::Duplicate;
				if (slot.Phase == Phase::Queued) replacement = &slot;
			}
		}
		for (const auto &published : State->GraphResources.PublishedTransform3DOutputs)
			if (published.Owner == request.Owner && published.Name == request.Name &&
				published.Generation >= request.Generation)
				return Result::Duplicate;
		auto *destination = merged ? merged : replacement ? replacement : free;
		if (!destination) return Result::Full;
		if (!merged) {
			const uint64_t held =
				State->GraphResources.Transform3DSourceBytes - (replacement ? replacement->SourceBytes : 0);
			if (held > imagegraph::MAXIMUM_TRANSFORM_IMAGE_3D_OUTPUT_BYTES - bytes) return Result::Full;
		}
		// Reserve before mutating the ledger so allocation failure leaves accepted work intact.
		decltype(destination->CameraBindings) freshBindings;
		try {
			if (merged)
				destination->CameraBindings.reserve(7);
			else
				freshBindings.reserve(7);
		} catch (const std::bad_alloc &) {
			return Result::Full;
		}
		if (merged) {
			std::erase_if(destination->CameraBindings, [&](const auto &binding) {
				return binding.Cancelled || binding.Name == request.Name;
			});
			if (destination->CameraBindings.size() >= 7) return Result::Full;
			destination->CameraBindings.push_back({request.Name, request.Generation, output, false});
		} else {
			State->GraphResources.Transform3DSourceBytes -= replacement ? replacement->SourceBytes : 0;
			*destination = {};
			destination->CameraBindings = std::move(freshBindings);
			destination->Phase = Phase::Queued;
			destination->Owner = request.Owner;
			destination->Name = request.Name;
			destination->Generation = request.Generation;
			destination->Width = request.Request.Width;
			destination->Height = request.Request.Height;
			destination->SourceBytes = bytes;
			destination->CameraRequest = std::move(request.Request);
			destination->CameraBindings.push_back({request.Name, request.Generation, output, false});
			State->GraphResources.Transform3DSourceBytes += bytes;
		}
		for (auto &slot : State->GraphResources.Transform3D) {
			if (&slot == destination || slot.Phase == Phase::Free || slot.Owner != request.Owner) continue;
			if (slot.CameraRequest) {
				for (auto &binding : slot.CameraBindings)
					if (binding.Name == request.Name) binding.Cancelled = true;
			} else if (slot.Name == request.Name)
				slot.Cancelled = true;
		}
		return merged || replacement ? Result::Replaced : Result::Queued;
	}

	imagegraph::TransformImage3DQueueResult
	Renderer::QueueSourceSdf(imagegraph::SourceSdfLiveRequest request) {
		RequireOwningThread("QueueSourceSdf");
		using Result = imagegraph::TransformImage3DQueueResult;
		using Slot = Impl::GraphResourceCache::Transform3DSlot;
		using Phase = Impl::GraphResourceCache::Transform3DPhase;
		if (!State || !request.Owner.IsValid() || !request.Name.IsValid() || !request.Generation ||
			imagegraph::ValidateSourceSdfRequest(request.Request) != imagegraph::SourceSdfStatus::Ok)
			return Result::Invalid;
		const uint64_t bytes = imagegraph::SourceSdfSourceBytes(request.Request),
					   scratch = imagegraph::SourceSdfScratchBytes(request.Request);
		if (bytes > imagegraph::MAXIMUM_TRANSFORM_IMAGE_3D_OUTPUT_BYTES ||
			scratch > imagegraph::MAXIMUM_TRANSFORM_IMAGE_3D_SCRATCH_BYTES)
			return Result::Full;
		Slot *replacement = nullptr, *free = nullptr;
		for (auto &slot : State->GraphResources.Transform3D) {
			if (slot.Phase == Phase::Free) {
				free = &slot;
				continue;
			}
			if (slot.Owner != request.Owner) continue;
			if (slot.CameraRequest) {
				for (const auto &binding : slot.CameraBindings)
					if (!binding.Cancelled && binding.Name == request.Name &&
						binding.Generation >= request.Generation)
						return Result::Duplicate;
				if (slot.Phase == Phase::Queued && slot.CameraBindings.size() == 1 &&
					slot.CameraBindings[0].Name == request.Name)
					replacement = &slot;
			} else if (slot.Name == request.Name) {
				if (slot.Generation >= request.Generation) return Result::Duplicate;
				if (slot.Phase == Phase::Queued) replacement = &slot;
			}
		}
		for (const auto &published : State->GraphResources.PublishedTransform3DOutputs)
			if (published.Owner == request.Owner && published.Name == request.Name &&
				published.Generation >= request.Generation)
				return Result::Duplicate;
		auto *destination = replacement ? replacement : free;
		if (!destination) return Result::Full;
		const uint64_t held =
			State->GraphResources.Transform3DSourceBytes - (replacement ? replacement->SourceBytes : 0);
		if (held > imagegraph::MAXIMUM_TRANSFORM_IMAGE_3D_OUTPUT_BYTES - bytes) return Result::Full;
		if (replacement) State->ReleaseTransform3D(*replacement);
		destination->Phase = Phase::Queued;
		destination->Owner = request.Owner;
		destination->Name = request.Name;
		destination->Generation = request.Generation;
		destination->Width = request.Request.Width;
		destination->Height = request.Request.Height;
		destination->SourceBytes = bytes;
		destination->SdfRequest = std::move(request.Request);
		State->GraphResources.Transform3DSourceBytes += bytes;
		for (auto &slot : State->GraphResources.Transform3D) {
			if (&slot == destination || slot.Phase == Phase::Free || slot.Owner != request.Owner) continue;
			if (slot.CameraRequest) {
				for (auto &binding : slot.CameraBindings)
					if (binding.Name == request.Name) binding.Cancelled = true;
			} else if (slot.Name == request.Name)
				slot.Cancelled = true;
		}
		return replacement ? Result::Replaced : Result::Queued;
	}
	bool Renderer::CancelTransformImage3D(core::Name owner, core::Name name, uint64_t generation) {
		RequireOwningThread("CancelTransformImage3D");
		if (!State) return false;
		for (auto &slot : State->GraphResources.Transform3D) {
			if (slot.Owner != owner) continue;
			if (slot.CameraRequest) {
				auto binding =
					std::find_if(slot.CameraBindings.begin(), slot.CameraBindings.end(), [&](const auto &b) {
						return b.Name == name && b.Generation == generation && !b.Cancelled;
					});
				if (binding == slot.CameraBindings.end()) continue;
				binding->Cancelled = true;
				if (std::all_of(slot.CameraBindings.begin(), slot.CameraBindings.end(), [](const auto &b) {
						return b.Cancelled;
					})) {
					if (slot.Phase == Impl::GraphResourceCache::Transform3DPhase::Queued)
						State->ReleaseTransform3D(slot);
					else
						slot.Cancelled = true;
				}
				return true;
			}
			if (slot.Name != name || slot.Generation != generation) continue;
			if (slot.Phase == Impl::GraphResourceCache::Transform3DPhase::Queued)
				State->ReleaseTransform3D(slot);
			else
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
			snapshot.Output = slot.Output;
			snapshot.SourceBytes = slot.SourceBytes;
			snapshot.ScratchBytes = slot.ScratchBytes;
			snapshot.Cancelled = slot.Cancelled;
			snapshot.CameraOutputs = std::count_if(
				slot.CameraBindings.begin(), slot.CameraBindings.end(), [](const auto &binding) {
					return !binding.Cancelled;
				}
			);
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

	bool test_support::TransformImage3DResidentTestAccess::PublishedFormat(
		const Renderer &renderer, core::Name owner, core::Name name, assets::TextureFormat &format
	) {
		return renderer.State && renderer.State->Textures.FormatOf(name, format, owner);
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
