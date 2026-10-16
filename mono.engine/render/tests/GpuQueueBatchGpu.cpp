#include "ComputeGraph.hpp"
#include "GpuQueueBatch.hpp"
#include "GraphHistory.hpp"
#include "RenderFixture.hpp"

#include <engine/assets/Mesh.hpp>
#include <engine/computegraph/Stock.hpp>
#include <engine/core/Assert.hpp>
#include <engine/core/Name.hpp>
#include <engine/render/Renderer.hpp>
#include <engine/scene/DrawInstance.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

TEST_SUITE_ID("engine.render.gpuqueuebatchgpu")
TEST_DEPENDS("engine.render.imagecomparison")
TEST_DEPENDS("engine.computegraph.runtime")

namespace {
	using namespace engine;
	using namespace engine::render;
	using namespace engine::render::test;

	constexpr uint32_t VIEW_COUNT = 2;
	constexpr uint32_t RUNS_PER_VIEW = 128;
	constexpr uint32_t WIDTH = 192;
	constexpr uint32_t HEIGHT = 128;

	struct QueueFixture {
		GpuQueueBatch *Batch = nullptr;
		SDL_GPUDevice *Device = nullptr;
		bool Parallel = false;
	};

	struct DeferredSemaphoreCleanup {
		VkDevice Device = VK_NULL_HANDLE;
		PFN_vkDestroySemaphore Destroy = nullptr;
		VkSemaphore Semaphore = VK_NULL_HANDLE;
		bool Called = false;
		bool Retired = false;
	};

	void SDLCALL CleanupDeferredSemaphore(void *userdata, bool resourcesRetired) noexcept {
		auto &cleanup = *static_cast<DeferredSemaphoreCleanup *>(userdata);
		cleanup.Called = true;
		cleanup.Retired = resourcesRetired;
		if (resourcesRetired && cleanup.Semaphore != VK_NULL_HANDLE)
			cleanup.Destroy(cleanup.Device, cleanup.Semaphore, nullptr);
		cleanup.Semaphore = VK_NULL_HANDLE;
	}

	class ScopedQueueOptOut {
	  public:
		ScopedQueueOptOut() {
			if (const char *mode = std::getenv("MONO_GPU_QUEUES")) {
				HadValue = true;
				SavedValue = mode;
			}
		}

		~ScopedQueueOptOut() {
			Restore();
		}

		bool Clear() const {
			return SDL_unsetenv_unsafe("MONO_GPU_QUEUES") == 0;
		}

		bool Restore() {
			if (!Active) return true;
			int result = SDL_unsetenv_unsafe("MONO_GPU_QUEUES");
			if (HadValue) result = SDL_setenv_unsafe("MONO_GPU_QUEUES", SavedValue.c_str(), 1);
			if (result == 0) Active = false;
			return result == 0;
		}

	  private:
		std::string SavedValue;
		bool HadValue = false;
		bool Active = true;
	};

	QueueFixture RequireQueueFixture(FixtureDevice &fixture) {
		const char *mode = std::getenv("MONO_GPU_QUEUES");
		if (mode == nullptr || (std::string_view(mode) != "serial" && std::string_view(mode) != "parallel"))
			SKIP("Set MONO_GPU_QUEUES=serial or parallel to opt in to the private GPU queue backend.");

		auto *device = static_cast<SDL_GPUDevice *>(fixture.Render.Backend().Device);
		const SDL_PropertiesID properties = SDL_GetGPUDeviceProperties(device);
		const auto *api = static_cast<const AtomicGpuQueuesV1 *>(
			SDL_GetPointerProperty(properties, ATOMIC_GPU_QUEUES_DEVICE_PROPERTY, nullptr)
		);
		if (api == nullptr) {
			INFO("requested queue mode: " << mode);
			INFO("renderer backend: " << fixture.Render.BackendName());
			SKIP(
				"GPU backend did not publish Atomic.gpu.vulkan.queues.v1; this backend is unsupported or its "
				"queue opt-in was ignored."
			);
		}

		auto *batch = static_cast<GpuQueueBatch *>(
			SDL_GetPointerProperty(properties, GpuQueueBatch::DEVICE_PROPERTY, nullptr)
		);
		if (batch == nullptr) {
			INFO("requested queue mode: " << mode);
			INFO("renderer backend: " << fixture.Render.BackendName());
			SKIP(
				"Renderer did not publish its private queue batch; the requested backend is unsupported or "
				"queue recording is unavailable."
			);
		}
		return {batch, device, std::string_view(mode) == "parallel"};
	}

	assets::MeshData QueueQuad() {
		assets::MeshData mesh;
		mesh.Vertices = {
			{{-0.5f, -0.5f, 0}, {0, 0, 1}, {0, 1}},
			{{0.5f, -0.5f, 0}, {0, 0, 1}, {1, 1}},
			{{0.5f, 0.5f, 0}, {0, 0, 1}, {1, 0}},
			{{-0.5f, 0.5f, 0}, {0, 0, 1}, {0, 0}},
		};
		mesh.Indices = {0, 1, 2, 0, 2, 3};
		mesh.ComputeBounds();
		return mesh;
	}

	std::string MeshName(uint32_t view, uint32_t run) {
		return "gpu-queue-view-" + std::to_string(view) + "-mesh-" + std::to_string(run);
	}

	std::array<std::vector<scene::DrawInstance>, VIEW_COUNT> AddResidentRuns(Renderer &renderer) {
		const assets::MeshData mesh = QueueQuad();
		std::array<std::vector<scene::DrawInstance>, VIEW_COUNT> instances;
		for (uint32_t view = 0; view < VIEW_COUNT; ++view) {
			instances[view].reserve(RUNS_PER_VIEW);
			const float origin = static_cast<float>(view) * 6.0f;
			for (uint32_t run = 0; run < RUNS_PER_VIEW; ++run) {
				const std::string name = MeshName(view, run);
				const core::Name meshName(name);
				REQUIRE(renderer.AddMesh(meshName, mesh));

				scene::DrawInstance instance;
				instance.Source = uint64_t(view) * RUNS_PER_VIEW + run + 1;
				instance.Frame.Position = {
					origin + static_cast<float>(run % 16) * 0.2f - 1.5f,
					static_cast<float>(run / 16) * 0.2f - 0.7f,
					-4.0f
				};
				instance.HalfExtent = {0.075f, 0.075f, 0.025f};
				instance.Tint = {
					0.35f + static_cast<float>(run % 3) * 0.2f,
					0.4f + static_cast<float>((run + 1) % 3) * 0.15f,
					0.45f + static_cast<float>((run + 2) % 3) * 0.1f
				};
				instance.Mesh = meshName;
				instances[view].push_back(instance);
			}
		}
		return instances;
	}

	std::array<render::View, VIEW_COUNT> MakeViews(
		std::array<std::vector<scene::DrawInstance>, VIEW_COUNT> &instances, const render::SceneTarget &target
	) {
		std::array<render::View, VIEW_COUNT> views;
		for (uint32_t index = 0; index < VIEW_COUNT; ++index) {
			auto &view = views[index];
			view.Slot = index;
			view.World = 7200 + index;
			view.WorldName = core::Name(index == 0 ? "gpu-queue-world-a" : "gpu-queue-world-b");
			view.Target = &target;
			view.Instances = instances[index];
			view.Camera.FieldOfViewRadians = 1.2f;
			view.Camera.NearPlane = 0.25f;
			view.Camera.FarPlane = 32.0f;
			view.CameraFrame.Position.X = static_cast<float>(index) * 6.0f;
		}
		return views;
	}

	CapturedImage CaptureComposed(Renderer &renderer, size_t slot) {
		return CaptureResource(
			renderer, core::Name("composed-image"), slot, WIDTH, HEIGHT, ImageFormat::Bgra8Unorm
		);
	}

	void SubmitAndWaitEmpty(SDL_GPUDevice *device) {
		const auto releaseFence = [device](SDL_GPUFence *fence) { SDL_ReleaseGPUFence(device, fence); };
		std::unique_ptr<SDL_GPUFence, decltype(releaseFence)> fence(nullptr, releaseFence);
		SDL_GPUCommandBuffer *command = SDL_AcquireGPUCommandBuffer(device);
		REQUIRE(command != nullptr);
		fence.reset(SDL_SubmitGPUCommandBufferAndAcquireFence(command));
		REQUIRE(fence != nullptr);
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
		while (!SDL_QueryGPUFence(device, fence.get()) && std::chrono::steady_clock::now() < deadline)
			SDL_Delay(1);
		REQUIRE(SDL_QueryGPUFence(device, fence.get()));
		SDL_GPUFence *completed = fence.get();
		REQUIRE(SDL_WaitForGPUFences(device, true, &completed, 1));
	}
}

TEST_CASE(
	"private queue recording draws resident named runs in moving production views", "[render][gpu][queues]"
) {
	FixtureDevice fixture;
	fixture.Initialise();
	const QueueFixture queues = RequireQueueFixture(fixture);
	auto instances = AddResidentRuns(fixture.Render);
	const render::SceneTarget target{WIDTH, HEIGHT};
	auto views = MakeViews(instances, target);
	render::OverlayImage overlay;

	const FrameResult residentUpload = fixture.Render.Render(views, overlay, nullptr, false);
	REQUIRE(residentUpload.Submitted);
	views[0].CameraFrame.Position.X += 0.12f;
	views[1].CameraFrame.Position.X -= 0.12f;
	for (render::View &view : views) {
		view.Damage.Scene = true;
		view.Damage.Viewport = true;
	}
	const FrameResult movingViews = fixture.Render.Render(views, overlay, nullptr, false);
	REQUIRE(movingViews.Submitted);
	CHECK(movingViews.DrawCalls >= VIEW_COUNT * RUNS_PER_VIEW);

	const GpuQueueBatch::Statistics statistics = queues.Batch->LastStatistics();
	CHECK(statistics.Recorded >= VIEW_COUNT);
	CHECK(statistics.Complete);
	CHECK((statistics.WorkerRecorded > 0) == queues.Parallel);

	const std::array<CapturedImage, VIEW_COUNT> firstCapture{
		CaptureComposed(fixture.Render, views[0].Slot), CaptureComposed(fixture.Render, views[1].Slot)
	};

	{
		FixtureDevice ordinary;
		{
			ScopedQueueOptOut optOut;
			REQUIRE(optOut.Clear());
			ordinary.Initialise();
			REQUIRE(optOut.Restore());
		}
		const auto ordinaryProperties =
			SDL_GetGPUDeviceProperties(static_cast<SDL_GPUDevice *>(ordinary.Render.Backend().Device));
		CHECK(SDL_GetPointerProperty(ordinaryProperties, GpuQueueBatch::DEVICE_PROPERTY, nullptr) == nullptr);
		auto ordinaryInstances = AddResidentRuns(ordinary.Render);
		const render::SceneTarget ordinaryTarget{WIDTH, HEIGHT};
		auto ordinaryViews = MakeViews(ordinaryInstances, ordinaryTarget);
		render::OverlayImage ordinaryOverlay;
		const FrameResult ordinaryWarm =
			ordinary.Render.Render(ordinaryViews, ordinaryOverlay, nullptr, false);
		REQUIRE(ordinaryWarm.Submitted);
		ordinaryViews[0].CameraFrame.Position.X += 0.12f;
		ordinaryViews[1].CameraFrame.Position.X -= 0.12f;
		for (render::View &view : ordinaryViews) {
			view.Damage.Scene = true;
			view.Damage.Viewport = true;
		}
		const FrameResult ordinaryMoving =
			ordinary.Render.Render(ordinaryViews, ordinaryOverlay, nullptr, false);
		REQUIRE(ordinaryMoving.Submitted);
		for (size_t index = 0; index < ordinaryViews.size(); ++index) {
			const CapturedImage ordinaryCapture = CaptureComposed(ordinary.Render, ordinaryViews[index].Slot);
			CHECK(firstCapture[index].Bytes == ordinaryCapture.Bytes);
		}
	}

	for (render::View &view : views) {
		view.Damage.Scene = true;
		view.Damage.Viewport = true;
	}
	const FrameResult repeatedViews = fixture.Render.Render(views, overlay, nullptr, false);
	REQUIRE(repeatedViews.Submitted);
	for (size_t index = 0; index < views.size(); ++index) {
		const CapturedImage repeated = CaptureComposed(fixture.Render, views[index].Slot);
		CHECK(firstCapture[index].Bytes == repeated.Bytes);
	}
}

TEST_CASE(
	"failed private recording cancels owner segments and leaves the batch reusable", "[render][gpu][queues]"
) {
	FixtureDevice fixture;
	fixture.Initialise();
	const QueueFixture queues = RequireQueueFixture(fixture);
	GpuQueueBatch &batch = *queues.Batch;

	auto runRefusal = [&](bool refuseRecording) {
		SDL_GPUCommandBuffer *main = nullptr;
		REQUIRE(batch.Begin(&main));
		main = batch.Api()->Acquire(queues.Device, ATOMIC_GPU_QUEUE_GRAPHICS);
		REQUIRE(main != nullptr);
		SDL_GPUCommandBuffer *initialCommand = main;
		batch.BindView(&main);
		const uint32_t firstPrefix = batch.Prefix();
		REQUIRE(firstPrefix != GpuQueueBatch::INVALID);
		const std::array dependency{firstPrefix};
		if (!refuseRecording) batch.RefuseSubmissionAfterForTests(2);
		const uint32_t refused = batch.Add(
			ATOMIC_GPU_QUEUE_GRAPHICS,
			"queue refusal test",
			[refuseRecording](SDL_GPUCommandBuffer *) { return !refuseRecording; },
			dependency
		);
		REQUIRE(refused != GpuQueueBatch::INVALID);
		const uint32_t laterOwnerPrefix = batch.Prefix();
		REQUIRE(laterOwnerPrefix != GpuQueueBatch::INVALID);
		SDL_GPUCommandBuffer *finalToken = main;

		GraphHistoryGeneration history;
		history.Signature = 41;
		history.Ready = true;
		history.Stage(73, finalToken);
		REQUIRE(history.Pending);
		CHECK(history.PendingCommand == GpuCommandIdentity(finalToken));
		CHECK(GpuCommandIdentity(finalToken) == GpuCommandIdentity(initialCommand));

		CHECK_FALSE(batch.SubmitEarlier(finalToken));
		CHECK_FALSE(batch.Complete(false, false));
		CHECK_FALSE(batch.LastStatistics().Complete);
		if (refuseRecording) {
			CHECK(batch.LastStatistics().Submitted == 0);
		} else {
			CHECK(batch.LastStatistics().Submitted >= 1);
		}
		history.Discard(finalToken);
		CHECK(history.Ready);
		CHECK(history.Signature == 41);
		CHECK_FALSE(history.Pending);
		CHECK(history.Matches(41, finalToken));
		CHECK_FALSE(history.Matches(73, finalToken));
		batch.EndScope();

		SDL_GPUCommandBuffer *recovery = nullptr;
		REQUIRE(batch.Begin(&recovery));
		recovery = batch.Api()->Acquire(queues.Device, ATOMIC_GPU_QUEUE_GRAPHICS);
		REQUIRE(recovery != nullptr);
		CHECK_FALSE(batch.Complete(false, false));
		batch.EndScope();
		SubmitAndWaitEmpty(queues.Device);
	};

	SECTION("all recorded work is refused") {
		runRefusal(true);
	}

#if ENGINE_ASSERTS_ENABLED
	SECTION("a partial submission is drained after the next handoff is refused") {
		runRefusal(false);
	}
#else
	SECTION("partial submission refusal needs the assert-enabled queue test hook") {
		SKIP("The partial-submit injection hook is active only when ENGINE_ASSERTS_ENABLED is non-zero.");
	}
#endif
}

#if ENGINE_ASSERTS_ENABLED
TEST_CASE(
	"a refused owner cancellation result requires retirement proof before reuse", "[render][gpu][queues]"
) {
	FixtureDevice fixture;
	fixture.Initialise();
	const QueueFixture queues = RequireQueueFixture(fixture);
	GpuQueueBatch &batch = *queues.Batch;

	auto runCancellationFailure = [&](bool refuseDrain) {
		SDL_GPUCommandBuffer *main = nullptr;
		REQUIRE(batch.Begin(&main));
		main = batch.Api()->Acquire(queues.Device, ATOMIC_GPU_QUEUE_GRAPHICS);
		REQUIRE(main != nullptr);
		batch.BindView(&main);

		bool completionCalled = false;
		std::shared_ptr<const GpuRetirementProof> retirement;
		const uint32_t task = batch.Add(
			ATOMIC_GPU_QUEUE_GRAPHICS,
			"cancel result refusal test",
			[](SDL_GPUCommandBuffer *) { return false; },
			std::span<const uint32_t>{},
			[&](GpuTaskResult result) noexcept {
				completionCalled = true;
				retirement = std::move(result.Retirement);
			}
		);
		REQUIRE(task != GpuQueueBatch::INVALID);
		batch.Refuse();
		batch.RefuseCancellationResultForTests();
		if (refuseDrain) batch.RefuseDrainForTests();

		CHECK_FALSE(batch.Complete(false, false));
		CHECK(batch.LastStatistics().HandoffAttempts == 0);
		CHECK_FALSE(batch.LastStatistics().Complete);
		CHECK(completionCalled);
		REQUIRE(retirement != nullptr);
		if (refuseDrain) {
			CHECK_FALSE(retirement->Complete);
			batch.EndScope();

			SDL_GPUCommandBuffer *rejectedMain = nullptr;
			CHECK_FALSE(batch.Begin(&rejectedMain));
			CHECK(rejectedMain == nullptr);
			REQUIRE(batch.RetireFailed());
			CHECK(retirement->Complete);
		} else {
			CHECK(retirement->Complete);
			batch.EndScope();
		}

		SDL_GPUCommandBuffer *recovery = nullptr;
		REQUIRE(batch.Begin(&recovery));
		recovery = batch.Api()->Acquire(queues.Device, ATOMIC_GPU_QUEUE_GRAPHICS);
		REQUIRE(recovery != nullptr);
		CHECK_FALSE(batch.Complete(false, false));
		batch.EndScope();
		SubmitAndWaitEmpty(queues.Device);
	};

	SECTION("a successful drain completes the proof with no handoffs") {
		runCancellationFailure(false);
	}
	SECTION("a refused drain keeps the proof until explicit retirement") {
		runCancellationFailure(true);
	}
}
#else
TEST_CASE(
	"canceled owner command result injection needs assert-enabled queue hooks", "[render][gpu][queues]"
) {
	SKIP("The cancellation result injection hook is active only when ENGINE_ASSERTS_ENABLED is non-zero.");
}
#endif

#if ENGINE_ASSERTS_ENABLED
TEST_CASE(
	"partial private queue submission fails a real compute request and retains uncertain GPU ownership",
	"[render][gpu][queues]"
) {
	FixtureDevice fixture;
	fixture.Initialise();
	const QueueFixture queues = RequireQueueFixture(fixture);
	GpuQueueBatch &batch = *queues.Batch;
	const computegraph::Owner owner{"gpu-queue-c01-failure", 1, 1, UINT32_MAX};
	render::ComputeGraph host(queues.Device, owner);
	computegraph::Diagnostic diagnostic;
	computegraph::Program program;
	REQUIRE(computegraph::StockStableCompactU32(8, program, diagnostic) == computegraph::Status::Ok);
	REQUIRE(host.Install(program, diagnostic) == computegraph::Status::Ok);

	const auto runFailure = [&](bool refuseDrain, bool refuseFirstHandoff = false) {
		SDL_GPUCommandBuffer *main = nullptr;
		REQUIRE(batch.Begin(&main));
		main = batch.Api()->Acquire(queues.Device, ATOMIC_GPU_QUEUE_GRAPHICS);
		REQUIRE(main != nullptr);
		batch.BindView(&main);

		const std::array<uint32_t, 4> inputValues{0, 2, 3, 0};
		computegraph::Upload upload;
		upload.Buffer = "input";
		upload.Type = computegraph::ElementType::U32;
		upload.Count = static_cast<uint32_t>(inputValues.size());
		upload.Bytes.resize(sizeof(inputValues));
		std::memcpy(upload.Bytes.data(), inputValues.data(), sizeof(inputValues));
		const computegraph::Request request{
			.Identity = owner,
			.Inputs = {std::move(upload)},
			.Outputs = {{"packed", 8}},
		};
		uint64_t ticket = 0;
		REQUIRE(host.Submit(request, ticket, diagnostic) == computegraph::Status::Ok);
		REQUIRE(ticket != 0);
		const uint64_t reservedBytes = host.ReservedDeviceBytes();
		REQUIRE(reservedBytes > 0);

		if (refuseFirstHandoff)
			batch.RefuseFirstHandoffResultForTests();
		else
			batch.RefuseSubmissionAfterForTests(2);
		if (refuseDrain) batch.RefuseDrainForTests();
		CHECK_FALSE(batch.SubmitEarlier(main));
		CHECK(batch.LastStatistics().Submitted == (refuseFirstHandoff ? 0u : 2u));
		CHECK(batch.LastStatistics().HandoffAttempts == (refuseFirstHandoff ? 1u : 2u));
		CHECK_FALSE(batch.Complete(false, false));
		CHECK_FALSE(batch.LastStatistics().Complete);
		batch.EndScope();

		host.Poll();
		computegraph::Completion completion;
		CHECK(host.Take(ticket, completion, diagnostic) == computegraph::Status::Ok);
		CHECK(completion.Ticket == ticket);
		CHECK(completion.Code == computegraph::Status::DeviceFailure);
		CHECK(completion.Outputs.empty());
		if (!refuseDrain) {
			CHECK(host.PendingGpuCount() == 0);
			CHECK(host.ReservedDeviceBytes() == 0);
			return;
		}

		CHECK(host.PendingGpuCount() == 1);
		CHECK(host.ReservedDeviceBytes() == reservedBytes);
		SDL_GPUCommandBuffer *rejectedMain = nullptr;
		CHECK_FALSE(batch.Begin(&rejectedMain));
		CHECK(rejectedMain == nullptr);
		REQUIRE(batch.RetireFailed());
		host.Poll();
		CHECK(host.PendingGpuCount() == 0);
		CHECK(host.ReservedDeviceBytes() == 0);
	};

	SECTION("a successful drain publishes failure and releases device ownership") {
		runFailure(false);
	}
	SECTION("an uncertain drain keeps ownership until explicit failed-flight retirement") {
		runFailure(true);
	}
	SECTION("a first handoff with a refused controller result is drained") {
		runFailure(false, true);
	}
	SECTION("a first handoff with an uncertain drain keeps GPU ownership") {
		runFailure(true, true);
	}
}
#else
TEST_CASE(
	"partial private queue compute failure requires assert-enabled injection hooks", "[render][gpu][queues]"
) {
	SKIP("The queue submit and drain refusal hooks are active only when ENGINE_ASSERTS_ENABLED is non-zero.");
}
#endif

TEST_CASE(
	"private device cleanup callback retires a Vulkan child before native device destruction",
	"[render][gpu][queues]"
) {
	FixtureDevice fixture;
	fixture.Initialise();
	RequireQueueFixture(fixture);
	auto *device = static_cast<SDL_GPUDevice *>(fixture.Render.Backend().Device);
	const auto properties = SDL_GetGPUDeviceProperties(device);
	const auto *api = static_cast<const AtomicGpuQueuesV1 *>(
		SDL_GetPointerProperty(properties, ATOMIC_GPU_QUEUES_DEVICE_PROPERTY, nullptr)
	);
	REQUIRE(api != nullptr);
	REQUIRE(api->GetDeviceProcAddr != nullptr);
	const auto createSemaphore =
		reinterpret_cast<PFN_vkCreateSemaphore>(api->GetDeviceProcAddr(api->Device, "vkCreateSemaphore"));
	REQUIRE(createSemaphore != nullptr);
	DeferredSemaphoreCleanup native{.Device = api->Device};
	native.Destroy =
		reinterpret_cast<PFN_vkDestroySemaphore>(api->GetDeviceProcAddr(api->Device, "vkDestroySemaphore"));
	REQUIRE(native.Destroy != nullptr);
	VkSemaphoreCreateInfo createInfo{};
	createInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
	REQUIRE(createSemaphore(native.Device, &createInfo, nullptr, &native.Semaphore) == VK_SUCCESS);
	if (!api->DeferDeviceCleanup(device, CleanupDeferredSemaphore, &native)) {
		native.Destroy(native.Device, native.Semaphore, nullptr);
		native.Semaphore = VK_NULL_HANDLE;
		FAIL(SDL_GetError());
	}
	CHECK_FALSE(native.Called);

	fixture.Render.Shutdown();
	CHECK(native.Called);
	CHECK(native.Retired);
	CHECK(native.Semaphore == VK_NULL_HANDLE);
	CHECK(fixture.Render.Backend().Device == nullptr);
}

TEST_CASE("an inspection download falls back before private queue batch admission", "[render][gpu][queues]") {
	FixtureDevice fixture;
	fixture.Initialise();
	const QueueFixture queues = RequireQueueFixture(fixture);
	auto instances = AddResidentRuns(fixture.Render);
	const render::SceneTarget target{WIDTH, HEIGHT};
	auto views = MakeViews(instances, target);
	render::OverlayImage overlay;

	const FrameResult warm = fixture.Render.Render(views, overlay, nullptr, false);
	REQUIRE(warm.Submitted);
	REQUIRE(queues.Batch->LastStatistics().Complete);
	const uint64_t sequence = queues.Batch->LastStatistics().Sequence;

	fixture.Render.Inspect(core::Name("composed-image"), views[0].Slot);
	for (render::View &view : views) {
		view.Damage.Scene = true;
		view.Damage.Viewport = true;
	}
	const FrameResult inspected = fixture.Render.Render(views, overlay, nullptr, false);
	REQUIRE(inspected.Submitted);
	const GpuQueueBatch::Statistics fallback = queues.Batch->LastStatistics();
	CHECK(fallback.PresentationFallbackReason == "download requested");
	CHECK_FALSE(fallback.PresentationAdopted);
	CHECK(fallback.Sequence == sequence);

	SubmitAndWaitEmpty(queues.Device);
	fixture.Render.Inspect(core::Name{});
	for (render::View &view : views) {
		view.Damage.Scene = true;
		view.Damage.Viewport = true;
	}
	const FrameResult resumed = fixture.Render.Render(views, overlay, nullptr, false);
	REQUIRE(resumed.Submitted);
	CHECK(queues.Batch->LastStatistics().Complete);
	CHECK(queues.Batch->LastStatistics().Sequence > sequence);
}
