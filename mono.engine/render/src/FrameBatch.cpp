#include "FrameBatch.hpp"

#include "FramePreparation.hpp"
#include "RendererState.hpp"
#include "RendererTestHooks.hpp"
#include "VulkanTimestamps.hpp"

#include <engine/core/Log.hpp>
#include <engine/graph/ExecutionPlan.hpp>
#include <engine/render/DataFactoryHookBind.hpp>

#include <SDL3/SDL_gpu.h>

#include <algorithm>
#include <atomic>
#include <limits>
#include <string>
#include <vector>

namespace engine::render {

	namespace {
		constexpr size_t NO_FORCED_FAILURE_GROUP = std::numeric_limits<size_t>::max();
		std::atomic_size_t ForcedFailureBeforeGroupForTests = NO_FORCED_FAILURE_GROUP;

		bool FailBeforeGroupForTests(size_t group) {
#if ENGINE_ASSERTS_ENABLED
			return ForcedFailureBeforeGroupForTests.load(std::memory_order_relaxed) == group;
#else
			(void)group;
			return false;
#endif
		}
	}

	void test_support::SetFrameBatchFailureBeforeGroupForTests(std::optional<size_t> group) {
#if ENGINE_ASSERTS_ENABLED
		ForcedFailureBeforeGroupForTests.store(
			group.value_or(NO_FORCED_FAILURE_GROUP), std::memory_order_relaxed
		);
#else
		(void)group;
#endif
	}

	FrameBatchResult FrameBatch::Run(
		std::span<const View> views,
		OverlayImage &overlay,
		FrameOverlayHook *gameInterfaceHook,
		bool present,
		FrameOverlayHook *hostOverlayHook
	) {
		FrameResult frame;
		if (Render.State == nullptr) {
			return {.Frame = frame, .Outcome = FrameBatchOutcome::SkippedBeforeAcquisition};
		}
		if (Render.State->Device == nullptr || views.empty() || Render.State->BatchActive) {
			Render.State->VisibilityWorking.Invalidate();
			Render.State->VisibilityCompleted = {};
			return {.Frame = frame, .Outcome = FrameBatchOutcome::SkippedBeforeAcquisition};
		}
		Render.State->VisibilityCompleted = {};
		Render.State->VisibilityWorking.Invalidate();
		Render.State->PollSceneFrames();

		std::vector<FrameViewIdentity> identities;
		identities.reserve(views.size());
		for (const View &view : views) {
			identities.push_back({view.World, Render.State->PipelineFor(view.Pipeline)});
		}
		const std::vector<FrameViewGroup> groups = GroupFrameViews(identities, present);

		std::vector<size_t> order;
		order.reserve(views.size());
		for (const FrameViewGroup &group : groups) {
			order.insert(order.end(), group.Views.begin(), group.Views.end());
		}
		for (size_t position = 0; position + 1 < order.size(); position++) {
			const SceneTarget *target = views[order[position]].Target;
			if (target == nullptr || !target->IsValid()) {
				ENGINE_ERROR("render batch view {} has no offscreen target", order[position]);
				return {.Frame = frame, .Outcome = FrameBatchOutcome::SkippedBeforeAcquisition};
			}
		}

		SDL_GPUCommandBuffer *command = nullptr;
		SDL_GPUTexture *swapchain = nullptr;
		uint32_t width = 0;
		uint32_t height = 0;
		if (present) {
			if (!Render.State->BeginFrame()) {
				return {.Frame = frame, .Outcome = FrameBatchOutcome::SkippedBeforeAcquisition};
			}
			Render.State->TakeFrame(command, swapchain, width, height);
		} else {
			command = SDL_AcquireGPUCommandBuffer(Render.State->Device);
			if (command == nullptr) {
				ENGINE_ERROR("SDL_AcquireGPUCommandBuffer (view batch): {}", SDL_GetError());
				return {.Frame = frame, .Outcome = FrameBatchOutcome::SkippedBeforeAcquisition};
			}
		}

		const SceneTarget *finalTarget = views[order.back()].Target;
		if (swapchain == nullptr && (finalTarget == nullptr || !finalTarget->IsValid())) {
			// A headless Studio has no swapchain and its viewport has no extent
			// until the first interface layout. That frame has nowhere to draw by
			// design. A windowed caller reaching the same state lost its target.
			if (!Render.State->Headless()) {
				ENGINE_ERROR("render batch final view has neither a swapchain nor an offscreen target");
			}
			const bool submitted = SDL_SubmitGPUCommandBuffer(command);
			return {
				.Frame = frame,
				.Outcome = submitted ? FrameBatchOutcome::Submitted : FrameBatchOutcome::Aborted,
			};
		}

		const scene::WorldLighting previousLighting = Render.CurrentLighting();
		++Render.State->RenderGeneration;
		Render.State->BatchActive = true;
		Render.State->DiscardPendingGraphHistoryWrites();
		Render.State->DataCaptureSources.clear();
		Render.State->MeshResidencyRecorded = false;
		Render.State->PreparedScopes.Clear();
		Render.State->BatchFailed = false;
		Render.State->BatchCommand = command;
		Render.State->BatchSwapchain = swapchain;
		Render.State->BatchWidth = width;
		Render.State->BatchHeight = height;
		Render.State->BatchTimingSlot = VulkanTimestamps::NO_SLOT;
		Render.State->BatchCaptureTimingRequested = std::any_of(
			Render.State->GraphResources.Images.begin(),
			Render.State->GraphResources.Images.end(),
			[&](const Renderer::Impl::ResourceImageSlot &image) {
				if (image.Phase != Renderer::Impl::ResourceImagePhase::Queued || image.Cancelled ||
					image.Image.DataCaptureTimingId == 0) {
					return false;
				}
				return std::any_of(views.begin(), views.end(), [&](const View &view) {
					const Renderer::Impl::InstalledPipeline *const pipeline =
						Render.State->PipelineFor(view.Pipeline);
					const ResourceImageRequest &request = image.Image.Request;
					return pipeline != nullptr && pipeline->Name == request.Pipeline &&
						   view.Slot == request.ViewSlot &&
						   (request.ExpectedSnapshotId.empty() ||
							request.ExpectedSnapshotId == view.SnapshotId);
				});
			}
		);

		std::vector<ViewMutationIdentity> restorations;
		size_t position = 0;
		for (size_t groupIndex = 0; groupIndex < groups.size() && !Render.State->BatchFailed; groupIndex++) {
			if (FailBeforeGroupForTests(groupIndex)) {
				// This is deliberately after acquisition and before a group transition,
				// where every batch-owned cleanup obligation is live.
				Render.State->BatchFailed = true;
				Render.State->VisibilityWorking.Invalidate();
				Render.State->VisibilityCompleted = {};
				break;
			}
			const FrameViewGroup &group = groups[groupIndex];
			for (size_t member = 0; member < group.Views.size(); member++, position++) {
				const size_t viewIndex = group.Views[member];
				const View &source = views[viewIndex];
				const auto pipelineIdentity = Render.ResolvePipelineIdentity(source.Pipeline);
				const ViewMutationIdentity identity{
					.WorldName = std::string(source.WorldName.Text()),
					.SnapshotId = source.SnapshotId,
					.Pipeline = pipelineIdentity ? pipelineIdentity->Name : core::Name{},
					.PipelineRevision = pipelineIdentity ? pipelineIdentity->Revision : 0,
					.ViewSlot = source.Slot,
				};
				const View *active = &source;
				View mutated;
				if (Render.HookBind->HasViewMutation(identity)) {
					mutated = source;
					if (Render.HookBind->ConsumeViewMutation(identity, mutated)) active = &mutated;
				} else if (Render.HookBind->HasViewMutationRestore(identity)) {
					mutated = source;
					mutated.Damage.Scene = true;
					mutated.Damage.Viewport = true;
					mutated.Damage.Environment = true;
					mutated.Damage.Portals = true;
					active = &mutated;
					restorations.push_back(identity);
				}
				const View &view = *active;
				Render.State->ActiveDataCaptureSource = {
					view.SnapshotId, view.WorldName, view.CameraFrame, view.Camera
				};
				Render.SetLighting(view.OverrideLighting ? view.Lighting : previousLighting);

				Render.State->BatchFirst = position == 0;
				Render.State->BatchFinal = position + 1 == order.size();
				Render.State->BatchShared = member == 0;
				Render.State->BatchViewIndex = viewIndex;
				Render.State->BatchWorldIndex = groupIndex;

				frame.Accumulate(Render.RenderView(
					view.CameraFrame,
					view.Camera,
					view.Instances,
					overlay,
					view.Surfaces,
					gameInterfaceHook,
					Render.State->BatchFinal ? hostOverlayHook : nullptr,
					view.Target,
					view.Slot,
					view,
					view.Particles,
					view.RibbonVertices,
					view.RibbonRuns,
					view.Lights,
					view.Foreign,
					view.Portals,
					Render.State->BatchFinal && present,
					view.Pipeline,
					view.World
				));
				if (Render.State->BatchFailed) {
					break;
				}
			}
		}
		const bool completed = frame.Submitted && !Render.State->BatchFailed;
		if (completed)
			for (const ViewMutationIdentity &identity : restorations)
				Render.HookBind->CompleteViewMutationRestore(identity);

		std::vector<const Renderer::Impl::InstalledPipeline *> plannedPipelines;
		plannedPipelines.reserve(groups.size());
		for (const FrameViewGroup &group : groups) {
			const auto *named =
				static_cast<const Renderer::Impl::InstalledPipeline *>(group.Identity.Pipeline);
			if (named == nullptr || std::find(plannedPipelines.begin(), plannedPipelines.end(), named) !=
										plannedPipelines.end()) {
				continue;
			}
			plannedPipelines.push_back(named);
			uint32_t planWidth = width;
			uint32_t planHeight = height;
			std::vector<uint64_t> worlds;
			worlds.reserve(views.size());
			for (const View &view : views) {
				if (Render.State->PipelineFor(view.Pipeline) != named) {
					continue;
				}
				worlds.push_back(view.World);
				if (view.Target != nullptr && view.Target->IsValid()) {
					planWidth = std::max(planWidth, view.Target->Width);
					planHeight = std::max(planHeight, view.Target->Height);
				}
			}
			planWidth = std::max(planWidth, 1u);
			planHeight = std::max(planHeight, 1u);

			graph::FrameExecutionPlan plan;
			core::Name offender;
			if (graph::PlanFrame(
					named->Graph, named->Schedule, worlds, planWidth, planHeight, plan, offender
				) == graph::ExecutionPlanStatus::Ok) {
				frame.ScheduledReadBytes += plan.ReadBytes;
				frame.ScheduledWriteBytes += plan.WriteBytes;
				frame.QueueTransferBytes += plan.QueueTransferBytes;
				frame.ConcurrentWaves += static_cast<uint32_t>(
					std::count_if(plan.Waves.begin(), plan.Waves.end(), [](const graph::PlannedWave &wave) {
						return wave.ConcurrentQueues;
					})
				);
				frame.TrafficCommandBuffers += static_cast<uint32_t>(named->Buffers.size());
			}
		}

		Render.SetLighting(previousLighting);
		FrameBatchOutcome outcome = completed ? FrameBatchOutcome::Submitted : FrameBatchOutcome::Aborted;
		if (Render.State->BatchCommand != nullptr) {
			// The partial command still submits to release its device ownership.
			// Its images retain their fences but cannot certify a completed graph.
			for (Renderer::Impl::ResourceImageSlot &image : Render.State->GraphResources.Images) {
				if (image.Phase == Renderer::Impl::ResourceImagePhase::Recorded) {
					image.Image.Status = ResourceImageStatus::Failed;
				} else if (image.Phase == Renderer::Impl::ResourceImagePhase::Queued &&
						   std::any_of(views.begin(), views.end(), [&](const View &view) {
							   const auto *pipeline = Render.State->PipelineFor(view.Pipeline);
							   return pipeline && pipeline->Name == image.Image.Request.Pipeline &&
									  view.Slot == image.Image.Request.ViewSlot;
						   })) {
					// A failed earlier node may prevent capture from recording at all.
					// No device work owns this slot, so its failure is ready immediately.
					image.Image.Status = ResourceImageStatus::Failed;
					image.Phase = Renderer::Impl::ResourceImagePhase::Ready;
				}
			}
			Render.State->Timestamps.Abandon(Render.State->BatchTimingSlot);
			if (Render.State->BatchTimingSlot < VulkanTimestamps::SLOTS) {
				Render.State->PendingMarks[Render.State->BatchTimingSlot].clear();
				Render.State->AbandonCaptureTimings(Render.State->BatchTimingSlot);
			}
			const bool submitted = Render.State->SubmitSceneCommand(Render.State->BatchCommand);
			frame.Submitted = submitted;
			outcome = submitted ? FrameBatchOutcome::SubmittedAfterViewFailure : FrameBatchOutcome::Aborted;
			if (!submitted) {
				Render.State->StageProbe.Clear(Render.State->Device);
				ENGINE_ERROR("SDL_SubmitGPUCommandBuffer (failed view batch): {}", SDL_GetError());
				Render.State->DiscardPendingGraphHistoryWrites(Render.State->BatchCommand);
			} else {
				// A partial batch releases its command buffer but never certifies history.
				Render.State->DiscardPendingGraphHistoryWrites(Render.State->BatchCommand);
			}
			Render.State->ClearSubmittedGraphHistoryWrites();
			Render.State->CompleteResidentUploads(submitted);
			Render.State->BatchCommand = nullptr;

			// A failed batch never reached the final view's submit, so any
			// downloads an earlier view recorded still hold their buffer.
			Render.State->DropDownloads();
		}
		Render.State->StageProbe.Flush(Render.State->Device);
		Render.State->BatchActive = false;
		Render.State->BatchFirst = false;
		Render.State->BatchFinal = false;
		Render.State->BatchShared = false;
		Render.State->PreparedScopes.Clear();
		Render.State->BatchFailed = false;
		Render.State->BatchSwapchain = nullptr;
		Render.State->BatchWidth = 0;
		Render.State->BatchHeight = 0;
		Render.State->BatchTimingSlot = VulkanTimestamps::NO_SLOT;
		Render.State->BatchCaptureTimingRequested = false;
		return {.Frame = frame, .Outcome = outcome};
	}
}
