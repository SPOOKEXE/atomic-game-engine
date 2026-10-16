#pragma once

#include "AtomicGpuQueues.h"

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace engine::render {
	struct GpuDrawPacket;

	// Owner-only proof. A null fence never implies that partial queue work retired.
	struct GpuRetirementProof {
		bool Complete = false;
	};
	struct GpuTaskResult {
		SDL_GPUFence *Fence = nullptr;
		std::shared_ptr<const GpuRetirementProof> Retirement;
	};

	class GpuTaskCompletion {
	  public:
		GpuTaskCompletion() = default;
		template <typename Function>
			requires std::is_nothrow_invocable_v<Function &, GpuTaskResult>
		GpuTaskCompletion(Function callback) : Callback(std::move(callback)) {}
		explicit operator bool() const {
			return static_cast<bool>(Callback);
		}
		void operator()(GpuTaskResult result) const noexcept {
			Callback(std::move(result));
		}

	  private:
		std::function<void(GpuTaskResult)> Callback;
	};

	class GpuQueueBatch {
	  public:
		static constexpr uint32_t INVALID = UINT32_MAX;
		static constexpr uint32_t MAX_TASKS = 64;
		static constexpr const char *DEVICE_PROPERTY = "Atomic.render.queue_batch";
		struct Statistics {
			uint64_t Sequence = 0;
			uint32_t Recorded = 0;
			uint32_t WorkerRecorded = 0;
			uint32_t Submitted = 0;
			uint32_t HandoffAttempts = 0;
			bool PresentationAdopted = false;
			uint32_t PresentationHandoffs = 0;
			std::string_view PresentationFallbackReason{};
			uint32_t Edges = 0;
			double ProducerMicroseconds = 0;
			double JoinMicroseconds = 0;
			double ParallelOverlapMicroseconds = 0;
			std::array<double, 3> RecorderMicroseconds{};
			std::array<uint32_t, 3> RecorderTasks{};
			bool Parallel = false;
			bool Complete = false;
		};
		enum class PresentationBegin { Fallback, Active, Failed };

		struct DeviceStatistics {
			uint64_t Sequence = 0;
			std::array<double, ATOMIC_GPU_QUEUE_COUNT> Microseconds{};
			std::array<uint32_t, ATOMIC_GPU_QUEUE_COUNT> Samples{};
			uint32_t Dropped = 0;
		};

		GpuQueueBatch();
		~GpuQueueBatch();
		GpuQueueBatch(const GpuQueueBatch &) = delete;
		GpuQueueBatch &operator=(const GpuQueueBatch &) = delete;
		static bool Requested();
		bool Initialise(SDL_GPUDevice *device);
		void Shutdown();
		bool Begin(SDL_GPUCommandBuffer **mainCommand);
		bool Begin(SDL_GPUCommandBuffer **mainCommand, SDL_GPUCommandBuffer *presentation);
		PresentationBegin
		BeginPresentation(SDL_GPUCommandBuffer **mainCommand, SDL_GPUCommandBuffer *presentation);
		bool PreparePresentationAcquire();
		bool PreparePresentation(SDL_GPUCommandBuffer **viewCommand);
		void BindView(SDL_GPUCommandBuffer **command);
		void UnbindView(SDL_GPUCommandBuffer **command);
		bool Active() const;
		bool HasCapacity(uint32_t extra);
		void Refuse();
		void ReportPresentationFallback(std::string_view staticReason);
		void RefuseSubmissionAfterForTests(uint32_t successfulSubmits);
		// Lets SDL submit one command successfully, then refuses its controller result.
		void RefuseFirstHandoffResultForTests();
		void RefuseDrainForTests();
		// Explicit failure recovery or teardown only; normal completion never waits here.
		bool RetireFailed();
		void RetainUntilDeviceDestruction(std::shared_ptr<void> resource, uint64_t bytes);
		uint64_t RetainedDeviceBytes() const;
		void ReleaseForDeviceDestruction();
		void EndScope();
		const AtomicGpuQueuesV1 *Api() const;
		uint32_t Prefix();
		bool Raster(std::shared_ptr<GpuDrawPacket> packet);
		uint32_t
		Add(uint32_t queue,
			std::string_view label,
			std::function<bool(SDL_GPUCommandBuffer *)> record,
			std::span<const uint32_t> dependencies,
			GpuTaskCompletion completion = {});
		void JoinAtEnd(uint32_t task);
		bool SubmitEarlier(SDL_GPUCommandBuffer *finalCommand);
		bool Complete(bool submitted, bool finalHandedOff = true);
		void Abort();
		const Statistics &LastStatistics() const;
		const DeviceStatistics &CompletedDeviceStatistics() const;

	  private:
		friend SDL_GPUCommandBuffer *GpuCommandIdentity(SDL_GPUCommandBuffer *command);
		struct State;
		std::unique_ptr<State> Held;
	};
}
