#include "GpuQueueBatch.hpp"

#include "GpuCommandIdentity.hpp"
#include "GpuDrawPackets.hpp"
#include "VulkanTimestamps.hpp"

#include <engine/core/FrameGraph.hpp>
#include <engine/core/Log.hpp>
#include <engine/core/Profiling.hpp>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <mutex>
#include <string>
#include <thread>
#include <utility>

namespace engine::render {
	namespace {
		thread_local std::array<GpuQueueBatch *, 4> CommandOwners{};
		using Clock = std::chrono::steady_clock;
		double Microseconds(Clock::time_point start) {
			return std::chrono::duration<double, std::micro>(Clock::now() - start).count();
		}
	}
	struct GpuQueueBatch::State {
		struct Timing {
			std::string_view Label;
			uint32_t Recorder = 0, Queue = 0, Slot = VulkanTimestamps::NO_SLOT;
			uint32_t From = VulkanTimestamps::MARKS, To = VulkanTimestamps::MARKS;
		};
		struct QueryBank {
			VulkanTimestamps Queries;
			uint32_t Slot = VulkanTimestamps::NO_SLOT;
		};
		struct Task {
			uint32_t Queue = 0;
			std::string_view Label;
			std::function<bool(SDL_GPUCommandBuffer *)> Record;
			GpuTaskCompletion Completion;
			std::vector<uint32_t> Dependencies;
			std::vector<VkSemaphore> Waits;
			std::vector<VkSemaphore> Signals;
			SDL_GPUCommandBuffer *Command = nullptr;
			SDL_GPUFence *Fence = nullptr;
			int Worker = -1;
			double Microseconds = 0;
			uint64_t Started = 0, Ended = 0;
			Timing GpuTiming;
			bool Ready = false;
			bool Submitted = false;
		};
		struct Flight {
			SDL_GPUFence *Fence = nullptr;
			std::vector<VkSemaphore> Semaphores;
			std::vector<Timing> Timings;
			uint64_t Sequence = 0;
		};
		enum class Request { None, Record, Submit, Cancel, Stop };
		struct Worker {
			std::mutex Mutex;
			std::condition_variable Changed;
			Request Action = Request::None;
			uint32_t Index = 0;
			bool Finished = true;
			std::jthread Thread;
		};
		SDL_GPUDevice *Device = nullptr;
		SDL_GPUDevice *TeardownDevice = nullptr;
		VkDevice NativeDevice = VK_NULL_HANDLE;
		bool DeferredNativeCleanup = false;
		const AtomicGpuQueuesV1 *Api = nullptr;
		const AtomicGpuQueuesV1 *TeardownApi = nullptr;
		std::thread::id Owner = std::this_thread::get_id();
		std::array<Worker, 2> Workers;
		std::vector<Task> Tasks;
		std::vector<uint32_t> FinalDependencies;
		std::vector<VkSemaphore> Semaphores;
		std::vector<Flight> Flights;
		std::vector<SDL_GPUCommandBuffer *> OwnerCommands;
		std::vector<VkSemaphore> FailedSemaphores;
		std::vector<Timing> PendingTimings;
		SDL_GPUCommandBuffer *Identity = nullptr;
		SDL_GPUCommandBuffer **Main = nullptr;
		SDL_GPUCommandBuffer **View = nullptr;
		uint32_t MainDependency = INVALID;
		uint32_t FinalTask = INVALID;
		uint32_t NextWorker = 0;
		uint32_t RefuseAfter = INVALID;
		bool RefuseDrain = false;
		bool RefuseFirstHandoffResult = false;
		std::shared_ptr<GpuRetirementProof> Retirement;
		std::shared_ptr<GpuRetirementProof> FailedRetirement;
		std::vector<std::shared_ptr<void>> RetainedResources;
		uint64_t RetainedBytes = 0;
		bool Active = false;
		bool Parallel = false;
		bool Failed = false;
		bool Poisoned = false;
		Statistics Last;
		DeviceStatistics CompletedDevice;
		std::array<std::array<QueryBank, ATOMIC_GPU_QUEUE_COUNT>, 3> QueryBanks;
		PFN_vkCreateSemaphore CreateSemaphore = nullptr;
		PFN_vkDestroySemaphore DestroySemaphore = nullptr;

		static void SDLCALL FinishNativeCleanup(void *userdata, bool resourcesRetired) {
			auto &state = *static_cast<State *>(userdata);
			for (auto &flight : state.Flights) {
				if (resourcesRetired) SDL_ReleaseGPUFence(state.TeardownDevice, flight.Fence);
				if (resourcesRetired)
					for (auto semaphore : flight.Semaphores)
						state.DestroySemaphore(state.NativeDevice, semaphore, nullptr);
			}
			state.Flights.clear();
			if (resourcesRetired)
				for (auto semaphore : state.FailedSemaphores)
					state.DestroySemaphore(state.NativeDevice, semaphore, nullptr);
			state.FailedSemaphores.clear();
			for (auto &recorder : state.QueryBanks)
				for (auto &bank : recorder)
					bank.Queries.Shutdown(resourcesRetired);
			state.DeferredNativeCleanup = false;
			state.TeardownDevice = nullptr;
			state.NativeDevice = VK_NULL_HANDLE;
			state.TeardownApi = nullptr;
			if (!resourcesRetired)
				ENGINE_ERROR("GPU device teardown could not prove native resource retirement");
		}

		void RequireOwner() const {
			if (std::this_thread::get_id() == Owner) return;
			ENGINE_ERROR("GPU queue batch used outside renderer owner");
			std::abort();
		}
		void Send(size_t worker, Request action, uint32_t index = 0) {
			auto &target = Workers[worker];
			std::lock_guard lock(target.Mutex);
			target.Action = action;
			target.Index = index;
			target.Finished = false;
			target.Changed.notify_one();
		}
		void Await(size_t worker) {
			auto &target = Workers[worker];
			std::unique_lock lock(target.Mutex);
			target.Changed.wait(lock, [&] { return target.Finished; });
		}
		void Record(Task &task) {
			const auto started = Clock::now();
			task.Started = static_cast<uint64_t>(
				std::chrono::duration_cast<std::chrono::nanoseconds>(started.time_since_epoch()).count()
			);
			auto &bank = QueryBanks[task.Worker < 0 ? 2 : task.Worker][task.Queue];
			task.GpuTiming.Label = task.Label == "record gpu raster packet" ? "gpu raster packet"
								   : task.Label == "compute graph upload"	? "gpu compute graph upload"
								   : task.Label == "compute graph dispatch" ? "gpu compute graph dispatch"
								   : task.Label == "compute graph download" ? "gpu compute graph download"
																			: "gpu packet";
			task.GpuTiming.Recorder = task.Worker < 0 ? 2 : task.Worker;
			task.GpuTiming.Queue = task.Queue;
			try {
				task.Command = Api->Acquire(Device, task.Queue);
				if (task.Command) {
					task.GpuTiming.Slot = bank.Slot;
					task.GpuTiming.From = bank.Queries.Mark(task.Command);
					task.Ready = task.Record(task.Command);
					task.GpuTiming.To = bank.Queries.Mark(task.Command);
				}
			} catch (...) {
				task.Ready = false;
			}
			task.Ended = static_cast<uint64_t>(
				std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count()
			);
			task.Microseconds = engine::render::Microseconds(started);
			if (!task.Ready && task.Command) {
				SDL_CancelGPUCommandBuffer(task.Command);
				task.Command = nullptr;
			}
		}
		void Submit(Task &task) {
			std::array<VkPipelineStageFlags, ATOMIC_GPU_QUEUES_MAX_SEMAPHORES> stages;
			stages.fill(VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
			if (!Api->SetDependencies(
					task.Command,
					static_cast<uint32_t>(task.Waits.size()),
					task.Waits.data(),
					stages.data(),
					static_cast<uint32_t>(task.Signals.size()),
					task.Signals.data()
				))
				return;
			auto *command = task.Command;
			task.Command = nullptr;
			++Last.HandoffAttempts;
			const bool refuseObservedResult = std::exchange(RefuseFirstHandoffResult, false);
			if (task.Completion) {
				task.Fence = SDL_SubmitGPUCommandBufferAndAcquireFence(command);
				task.Submitted = task.Fence != nullptr;
			} else
				task.Submitted = SDL_SubmitGPUCommandBuffer(command);
			// This seam keeps the successful native submit alive while testing a lost controller result.
			if (refuseObservedResult && !task.Completion && task.Submitted) {
				task.Submitted = false;
			}
		}

		void Run(size_t worker) {
			auto &target = Workers[worker];
			for (;;) {
				std::unique_lock lock(target.Mutex);
				target.Changed.wait(lock, [&] { return target.Action != Request::None; });
				const auto action = target.Action;
				const auto index = target.Index;
				target.Action = Request::None;
				lock.unlock();
				if (action == Request::Record) {
					for (auto &task : Tasks)
						if (task.Worker == static_cast<int>(worker)) Record(task);
				} else if (action == Request::Submit) {
					Submit(Tasks[index]);
				} else if (action == Request::Cancel) {
					for (auto &task : Tasks)
						if (task.Worker == static_cast<int>(worker) && task.Command) {
							SDL_CancelGPUCommandBuffer(task.Command);
							task.Command = nullptr;
						}
				}
				lock.lock();
				target.Finished = true;
				target.Changed.notify_one();
				if (action == Request::Stop) return;
			}
		}
		void Collect(bool wait) {
			std::erase_if(Flights, [&](Flight &flight) {
				if (!wait && !SDL_QueryGPUFence(Device, flight.Fence)) return false;
				if (!SDL_WaitForGPUFences(Device, true, &flight.Fence, 1)) return false;
				SDL_ReleaseGPUFence(Device, flight.Fence);
				CompletedDevice = {.Sequence = flight.Sequence};
				std::array<std::array<std::array<double, VulkanTimestamps::MARKS>, ATOMIC_GPU_QUEUE_COUNT>, 3>
					times{};
				std::array<std::array<uint32_t, ATOMIC_GPU_QUEUE_COUNT>, 3> counts{};
				for (const auto &timing : flight.Timings) {
					auto &count = counts[timing.Recorder][timing.Queue];
					auto &values = times[timing.Recorder][timing.Queue];
					if (count == 0 && timing.Slot != VulkanTimestamps::NO_SLOT)
						(void)QueryBanks[timing.Recorder][timing.Queue].Queries.Collect(
							timing.Slot, values.data(), count
						);
					if (timing.To >= count || timing.From >= timing.To) {
						++CompletedDevice.Dropped;
						continue;
					}
					const auto microseconds =
						VulkanTimestamps::Between(values.data(), timing.From, timing.To) / 1000.0;
					CompletedDevice.Microseconds[timing.Queue] += microseconds;
					++CompletedDevice.Samples[timing.Queue];
					core::FrameGraph::Report(
						timing.Label, core::ProfileCategory::Gpu, static_cast<float>(microseconds / 1000.0)
					);
				}
				for (auto semaphore : flight.Semaphores)
					DestroySemaphore(Api->Device, semaphore, nullptr);
				return true;
			});
		}
	};

	GpuQueueBatch::GpuQueueBatch() : Held(std::make_unique<State>()) {}
	SDL_GPUCommandBuffer *GpuCommandIdentity(SDL_GPUCommandBuffer *command) {
		if (!command) return nullptr;
		for (auto *batch : CommandOwners) {
			if (!batch || !batch->Held->Identity) continue;
			const auto &commands = batch->Held->OwnerCommands;
			if (std::find(commands.begin(), commands.end(), command) != commands.end())
				return batch->Held->Identity;
		}
		return command;
	}
	GpuQueueBatch::~GpuQueueBatch() {
		Shutdown();
	}
	bool GpuQueueBatch::Requested() {
		const char *mode = std::getenv("MONO_GPU_QUEUES");
		return mode && (std::string_view(mode) == "serial" || std::string_view(mode) == "parallel");
	}
	bool GpuQueueBatch::Initialise(SDL_GPUDevice *device) {
		Held->Owner = std::this_thread::get_id();
		if (!Requested()) return false;
		const auto properties = SDL_GetGPUDeviceProperties(device);
		auto *api = static_cast<const AtomicGpuQueuesV1 *>(
			SDL_GetPointerProperty(properties, ATOMIC_GPU_QUEUES_DEVICE_PROPERTY, nullptr)
		);
		if (!api || api->StructSize < sizeof(AtomicGpuQueuesV1) ||
			api->AbiVersion != ATOMIC_GPU_QUEUES_ABI_VERSION || !api->FreezeResources ||
			!api->ReleaseFrozenResources || !api->DeferDeviceCleanup)
			return false;
		Held->Device = device;
		Held->TeardownDevice = device;
		Held->NativeDevice = api->Device;
		Held->Api = api;
		Held->TeardownApi = api;
		Held->CreateSemaphore =
			reinterpret_cast<PFN_vkCreateSemaphore>(api->GetDeviceProcAddr(api->Device, "vkCreateSemaphore"));
		Held->DestroySemaphore = reinterpret_cast<PFN_vkDestroySemaphore>(
			api->GetDeviceProcAddr(api->Device, "vkDestroySemaphore")
		);
		if (!Held->CreateSemaphore || !Held->DestroySemaphore) {
			Held->Api = nullptr;
			Held->Device = nullptr;
			return false;
		}
		Held->Tasks.reserve(MAX_TASKS);
		Held->RetainedResources.reserve(MAX_TASKS);
		Held->PendingTimings.reserve(MAX_TASKS);
		Held->Semaphores.reserve(ATOMIC_GPU_QUEUES_MAX_SEMAPHORES);
		Held->FailedSemaphores.reserve(ATOMIC_GPU_QUEUES_MAX_SEMAPHORES);
		Held->Flights.reserve(3);
		Held->OwnerCommands.reserve(MAX_TASKS);
		for (auto &recorder : Held->QueryBanks)
			for (uint32_t queue = 0; queue < ATOMIC_GPU_QUEUE_COUNT; ++queue)
				(void)recorder[queue].Queries.Probe(device, queue);
		Held->Parallel = std::string_view(std::getenv("MONO_GPU_QUEUES")) == "parallel";
		if (Held->Parallel)
			for (size_t worker = 0; worker < Held->Workers.size(); ++worker)
				Held->Workers[worker].Thread = std::jthread([this, worker] { Held->Run(worker); });
		SDL_SetPointerProperty(properties, DEVICE_PROPERTY, this);
		ENGINE_INFO(
			"private GPU queues ready on {}: graphics family {}, compute family {}, transfer family {}, "
			"recording {}",
			api->Properties.deviceName,
			api->Family[0],
			api->Family[1],
			api->Family[2],
			Held->Parallel ? "parallel" : "serial"
		);
		return true;
	}
	bool GpuQueueBatch::Begin(SDL_GPUCommandBuffer **mainCommand) {
		Held->RequireOwner();
		if (Held->Active) return true;
		if (!Held->Api || Held->Poisoned || Held->Identity) return false;
		auto position = std::find(CommandOwners.begin(), CommandOwners.end(), nullptr);
		if (position == CommandOwners.end()) return false;
		Held->Collect(false);
		{
			ENGINE_PROFILE_CAT("GPU queue maintenance", core::ProfileCategory::Idle);
			if (!Held->Api->MaintainIdle(Held->Device)) return false;
		}
		auto retirement = std::make_shared<GpuRetirementProof>();
		if (Held->Flights.size() >= 3 || !Held->Api->BeginRecordingBatch(Held->Device)) return false;
		Held->Retirement = std::move(retirement);
		Held->Main = mainCommand;
		Held->View = nullptr;
		Held->MainDependency = INVALID;
		Held->FinalTask = INVALID;
		Held->NextWorker = 0;
		Held->Failed = false;
		Held->Active = true;
		*position = this;
		Held->Last = {.Sequence = Held->Last.Sequence + 1, .Parallel = Held->Parallel};
		State::Task reset;
		reset.Command = Held->Api->Acquire(Held->Device, ATOMIC_GPU_QUEUE_GRAPHICS);
		if (!reset.Command) {
			Complete(false, false);
			EndScope();
			return false;
		}
		reset.Ready = true;
		for (auto &recorder : Held->QueryBanks)
			for (auto &bank : recorder)
				bank.Slot = bank.Queries.Begin(reset.Command);
		Held->Tasks.push_back(std::move(reset));
		Held->MainDependency = 0;
		return true;
	}
	void GpuQueueBatch::BindView(SDL_GPUCommandBuffer **command) {
		Held->View = command;
		if (Held->Active && command && *command && Held->Identity == nullptr) {
			Held->Identity = *command;
			Held->OwnerCommands.push_back(*command);
		}
	}
	void GpuQueueBatch::UnbindView(SDL_GPUCommandBuffer **command) {
		if (Held->View == command) Held->View = nullptr;
	}
	bool GpuQueueBatch::Active() const {
		return Held->Active;
	}
	bool GpuQueueBatch::HasCapacity(uint32_t extra) const {
		return Held->Active && !Held->Failed && extra < MAX_TASKS && Held->Tasks.size() + extra < MAX_TASKS;
	}
	void GpuQueueBatch::RefuseSubmissionAfterForTests(uint32_t successfulSubmits) {
#if ENGINE_ASSERTS_ENABLED
		Held->RequireOwner();
		Held->RefuseAfter = successfulSubmits;
#else
		(void)successfulSubmits;
#endif
	}
	void GpuQueueBatch::RefuseFirstHandoffResultForTests() {
#if ENGINE_ASSERTS_ENABLED
		Held->RequireOwner();
		Held->RefuseFirstHandoffResult = true;
#endif
	}
	void GpuQueueBatch::RefuseDrainForTests() {
#if ENGINE_ASSERTS_ENABLED
		Held->RefuseDrain = true;
#endif
	}
	bool GpuQueueBatch::RetireFailed() {
		Held->RequireOwner();
		if (!Held->FailedRetirement) return true;
		ENGINE_PROFILE_CAT("GPU failed batch retirement", core::ProfileCategory::Idle);
		if (!SDL_WaitForGPUIdle(Held->Device)) return false;
		Held->FailedRetirement->Complete = true;
		Held->FailedRetirement.reset();
		for (auto semaphore : Held->FailedSemaphores)
			Held->DestroySemaphore(Held->Api->Device, semaphore, nullptr);
		Held->FailedSemaphores.clear();
		for (const auto &timing : Held->PendingTimings)
			Held->QueryBanks[timing.Recorder][timing.Queue].Queries.Abandon(timing.Slot);
		Held->PendingTimings.clear();
		Held->RetainedResources.clear();
		Held->RetainedBytes = 0;
		Held->Poisoned = false;
		return true;
	}
	void GpuQueueBatch::RetainUntilDeviceDestruction(std::shared_ptr<void> resource, uint64_t bytes) {
		Held->RequireOwner();
		Held->RetainedResources.push_back(std::move(resource));
		Held->RetainedBytes += bytes;
	}
	uint64_t GpuQueueBatch::RetainedDeviceBytes() const {
		return Held->RetainedBytes;
	}
	void GpuQueueBatch::ReleaseForDeviceDestruction() {
		Held->RequireOwner();
		Held->RetainedResources.clear();
		Held->RetainedBytes = 0;
		if (Held->DeferredNativeCleanup) {
			if (!Held->TeardownApi->DeferDeviceCleanup(
					Held->TeardownDevice, State::FinishNativeCleanup, Held.get()
				)) {
				ENGINE_ERROR("could not register GPU native teardown cleanup: {}", SDL_GetError());
				State::FinishNativeCleanup(Held.get(), false);
			}
		}
	}

	void GpuQueueBatch::Refuse() {
		Held->Failed = true;
	}
	void GpuQueueBatch::EndScope() {
		Abort();
		for (auto &batch : CommandOwners)
			if (batch == this) batch = nullptr;
		Held->Identity = nullptr;
		Held->OwnerCommands.clear();
	}
	const AtomicGpuQueuesV1 *GpuQueueBatch::Api() const {
		return Held->Api;
	}
	uint32_t GpuQueueBatch::Prefix() {
		Held->RequireOwner();
		if (!Held->Active || !Held->View || !*Held->View || Held->Tasks.size() + 2 >= MAX_TASKS)
			return INVALID;
		State::Task prefix;
		prefix.Command = *Held->View;
		prefix.Ready = true;
		if (Held->MainDependency != INVALID) prefix.Dependencies.push_back(Held->MainDependency);
		auto *next = Held->Api->Acquire(Held->Device, ATOMIC_GPU_QUEUE_GRAPHICS);
		if (!next) return INVALID;
		const auto index = static_cast<uint32_t>(Held->Tasks.size());
		Held->Tasks.push_back(std::move(prefix));
		*Held->View = next;
		*Held->Main = next;
		Held->OwnerCommands.push_back(next);
		Held->MainDependency = index;
		return index;
	}
	uint32_t GpuQueueBatch::Add(
		uint32_t queue,
		std::string_view label,
		std::function<bool(SDL_GPUCommandBuffer *)> record,
		std::span<const uint32_t> dependencies,
		GpuTaskCompletion completion
	) {
		Held->RequireOwner();
		if (!Held->Active || Held->Tasks.size() + 1 >= MAX_TASKS || queue >= ATOMIC_GPU_QUEUE_COUNT)
			return INVALID;
		for (const auto dependency : dependencies)
			if (dependency >= Held->Tasks.size()) return INVALID;
		State::Task task;
		task.Queue = Held->Parallel ? queue : static_cast<uint32_t>(ATOMIC_GPU_QUEUE_GRAPHICS);
		task.Label = label;
		task.Record = std::move(record);
		task.Completion = std::move(completion);
		task.Dependencies.assign(dependencies.begin(), dependencies.end());
		if (std::find(task.Dependencies.begin(), task.Dependencies.end(), 0) == task.Dependencies.end())
			task.Dependencies.push_back(0);
		if (Held->Parallel) task.Worker = static_cast<int>(Held->NextWorker++ % Held->Workers.size());
		const auto index = static_cast<uint32_t>(Held->Tasks.size());
		Held->Tasks.push_back(std::move(task));
		return index;
	}
	bool GpuQueueBatch::Raster(std::shared_ptr<GpuDrawPacket> packet) {
		const auto prefix = Prefix();
		if (prefix == INVALID) return false;
		const std::array dependencies{prefix};
		const auto raster = Add(
			ATOMIC_GPU_QUEUE_GRAPHICS,
			"record gpu raster packet",
			[packet = std::move(packet)](SDL_GPUCommandBuffer *command) { return packet->Record(command); },
			dependencies
		);
		if (raster == INVALID) return false;
		Held->MainDependency = raster;
		return true;
	}
	void GpuQueueBatch::JoinAtEnd(uint32_t task) {
		if (task != INVALID) Held->FinalDependencies.push_back(task);
	}
	bool GpuQueueBatch::SubmitEarlier(SDL_GPUCommandBuffer *finalCommand) {
		Held->RequireOwner();
		if (!Held->Active) return true;
		ENGINE_PROFILE_CAT("GPU packet recording", core::ProfileCategory::Render);
		State::Task final;
		final.Command = finalCommand;
		final.Ready = true;
		final.Dependencies = Held->FinalDependencies;
		if (Held->MainDependency != INVALID) final.Dependencies.push_back(Held->MainDependency);
		std::array<bool, MAX_TASKS> consumed{};
		for (const auto &task : Held->Tasks)
			for (const auto dependency : task.Dependencies)
				consumed[dependency] = true;
		for (uint32_t index = 0; index < Held->Tasks.size(); ++index)
			if (!consumed[index] && std::find(final.Dependencies.begin(), final.Dependencies.end(), index) ==
										final.Dependencies.end())
				final.Dependencies.push_back(index);
		Held->FinalTask = static_cast<uint32_t>(Held->Tasks.size());
		Held->Tasks.push_back(std::move(final));
		if (Held->Failed) return false;
		if (Held->Parallel) {
			for (size_t worker = 0; worker < Held->Workers.size(); ++worker)
				Held->Send(worker, State::Request::Record);
			const auto started = Clock::now();
			{
				ENGINE_PROFILE_CAT("GPU recording worker join", core::ProfileCategory::Idle);
				for (size_t worker = 0; worker < Held->Workers.size(); ++worker)
					Held->Await(worker);
			}
			Held->Last.JoinMicroseconds = Microseconds(started);
		} else {
			for (auto &task : Held->Tasks)
				if (task.Record) Held->Record(task);
		}
		for (const auto &task : Held->Tasks) {
			if (!task.Ready) Held->Failed = true;
			if (!task.Record) continue;
			++Held->Last.Recorded;
			Held->Last.WorkerRecorded += task.Worker >= 0;
			Held->Last.ProducerMicroseconds += task.Microseconds;
			Held->Last.RecorderMicroseconds[task.Worker < 0 ? 2 : task.Worker] += task.Microseconds;
			++Held->Last.RecorderTasks[task.Worker < 0 ? 2 : task.Worker];
			if (task.Worker >= 0)
				core::FrameGraph::Report(
					task.Label, core::ProfileCategory::Render, static_cast<float>(task.Microseconds / 1000)
				);
		}
		for (const auto &left : Held->Tasks)
			for (const auto &right : Held->Tasks) {
				if (left.Worker != 0 || right.Worker != 1 || !left.Record || !right.Record) continue;
				const auto begin = std::max(left.Started, right.Started);
				const auto end = std::min(left.Ended, right.Ended);
				if (end > begin)
					Held->Last.ParallelOverlapMicroseconds += static_cast<double>(end - begin) / 1000.0;
			}
		Held->PendingTimings.clear();
		for (const auto &task : Held->Tasks)
			if (task.Record) Held->PendingTimings.push_back(task.GpuTiming);
		for (auto &recorder : Held->QueryBanks)
			for (auto &bank : recorder)
				bank.Queries.Submitted(bank.Slot);
		if (!Held->Failed) {
			VkSemaphoreCreateInfo info{};
			info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
			for (auto &task : Held->Tasks) {
				for (const auto dependency : task.Dependencies) {
					VkSemaphore semaphore = VK_NULL_HANDLE;
					if (Held->Semaphores.size() == ATOMIC_GPU_QUEUES_MAX_SEMAPHORES ||
						Held->CreateSemaphore(Held->Api->Device, &info, nullptr, &semaphore) != VK_SUCCESS) {
						Held->Failed = true;
						break;
					}
					Held->Semaphores.push_back(semaphore);
					task.Waits.push_back(semaphore);
					Held->Tasks[dependency].Signals.push_back(semaphore);
				}
				if (Held->Failed) break;
			}
		}
		Held->Last.Edges = static_cast<uint32_t>(Held->Semaphores.size());
		if (Held->Failed) return false;
		for (uint32_t index = 0; index < Held->FinalTask; ++index) {
			auto &task = Held->Tasks[index];
			if (Held->Last.Submitted == Held->RefuseAfter) {
				Held->RefuseAfter = INVALID;
				Held->Failed = true;
				break;
			}
			if (task.Worker >= 0) {
				Held->Send(static_cast<size_t>(task.Worker), State::Request::Submit, index);
				Held->Await(static_cast<size_t>(task.Worker));
			} else
				Held->Submit(task);
			if (!task.Submitted) {
				Held->Failed = true;
				break;
			}
			++Held->Last.Submitted;
		}
		if (Held->Failed) return false;
		std::array<VkPipelineStageFlags, ATOMIC_GPU_QUEUES_MAX_SEMAPHORES> stages;
		stages.fill(VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
		auto &tail = Held->Tasks.back();
		return Held->Api->SetDependencies(
			finalCommand,
			static_cast<uint32_t>(tail.Waits.size()),
			tail.Waits.data(),
			stages.data(),
			0,
			nullptr
		);
	}
	bool GpuQueueBatch::Complete(bool submitted, bool finalHandedOff) {
		Held->RequireOwner();
		if (!Held->Active) return submitted;
		if (finalHandedOff) ++Held->Last.HandoffAttempts;
		if (finalHandedOff && Held->FinalTask != INVALID) Held->Tasks[Held->FinalTask].Command = nullptr;
		if (submitted && Held->FinalTask != INVALID) {
			Held->Tasks[Held->FinalTask].Submitted = true;
			++Held->Last.Submitted;
			auto *tail = Held->Api->Acquire(Held->Device, ATOMIC_GPU_QUEUE_GRAPHICS);
			if (tail) ++Held->Last.HandoffAttempts;
			auto *fence = tail ? SDL_SubmitGPUCommandBufferAndAcquireFence(tail) : nullptr;
			if (fence) {
				++Held->Last.Submitted;
				State::Flight flight{
					fence, std::move(Held->Semaphores), std::move(Held->PendingTimings), Held->Last.Sequence
				};
				Held->Flights.push_back(std::move(flight));
			} else {
				submitted = false;
			}
		}
		bool retirementKnown = false;
		if (!submitted) {
			retirementKnown = true;
			for (size_t worker = 0; worker < Held->Workers.size(); ++worker)
				if (Held->Parallel) Held->Send(worker, State::Request::Cancel);
			for (size_t worker = 0; worker < Held->Workers.size(); ++worker)
				if (Held->Parallel) Held->Await(worker);
			for (auto &task : Held->Tasks)
				if (task.Worker < 0 && task.Command) {
					SDL_CancelGPUCommandBuffer(task.Command);
					task.Command = nullptr;
				}
			if (Held->FinalTask == INVALID && Held->Main && *Held->Main) {
				SDL_CancelGPUCommandBuffer(*Held->Main);
				*Held->Main = nullptr;
			}
			// A failed device submission can still be pending even when SDL reported failure.
			if (Held->Last.HandoffAttempts != 0 || finalHandedOff) {
				ENGINE_PROFILE_CAT("GPU partial submission drain", core::ProfileCategory::Idle);
				if (Held->RefuseDrain || !SDL_WaitForGPUIdle(Held->Device)) {
					retirementKnown = false;
					Held->FailedRetirement = Held->Retirement;
					Held->Poisoned = true;
					Held->FailedSemaphores.insert(
						Held->FailedSemaphores.end(), Held->Semaphores.begin(), Held->Semaphores.end()
					);
					Held->Semaphores.clear();
					ENGINE_ERROR("GPU partial submission drain failed: {}", SDL_GetError());
				}
			}
		}
		Held->RefuseDrain = false;
		Held->Retirement->Complete = retirementKnown;
		for (auto &task : Held->Tasks)
			if (task.Completion) task.Completion({task.Fence, Held->Retirement});

		for (auto &recorder : Held->QueryBanks)
			for (auto &bank : recorder) {
				if (!submitted && !Held->Poisoned) bank.Queries.Abandon(bank.Slot);
				bank.Slot = VulkanTimestamps::NO_SLOT;
			}
		Held->Tasks.clear();
		Held->FinalDependencies.clear();
		for (auto semaphore : Held->Semaphores)
			Held->DestroySemaphore(Held->Api->Device, semaphore, nullptr);
		Held->Semaphores.clear();
		Held->RefuseAfter = INVALID;
		Held->RefuseFirstHandoffResult = false;
		Held->Last.Complete = submitted;
		Held->Main = nullptr;
		Held->View = nullptr;
		Held->Active = false;
		if (!Held->Api->EndRecordingBatch(Held->Device)) {
			Held->Poisoned = true;
			Held->Last.Complete = false;
			submitted = false;
			ENGINE_ERROR("GPU batch did not release its recording leases: {}", SDL_GetError());
		}
		return submitted;
	}
	void GpuQueueBatch::Abort() {
		Complete(false, false);
	}
	void GpuQueueBatch::Shutdown() {
		if (!Held->Device) return;
		Held->RequireOwner();
		EndScope();
		const bool retired = RetireFailed();
		if (!retired) ENGINE_ERROR("GPU batch retirement still uncertain at device teardown");
		Held->Collect(retired);
		Held->DeferredNativeCleanup = !retired || !Held->Flights.empty();
		for (size_t worker = 0; worker < Held->Workers.size(); ++worker)
			if (Held->Workers[worker].Thread.joinable()) {
				Held->Send(worker, State::Request::Stop);
				Held->Workers[worker].Thread.join();
			}
		if (!Held->DeferredNativeCleanup)
			for (auto &recorder : Held->QueryBanks)
				for (auto &bank : recorder)
					bank.Queries.Shutdown();
		SDL_ClearProperty(SDL_GetGPUDeviceProperties(Held->Device), DEVICE_PROPERTY);
		Held->Api = nullptr;
		Held->Device = nullptr;
	}
	const GpuQueueBatch::DeviceStatistics &GpuQueueBatch::CompletedDeviceStatistics() const {
		return Held->CompletedDevice;
	}
	const GpuQueueBatch::Statistics &GpuQueueBatch::LastStatistics() const {
		return Held->Last;
	}
}
