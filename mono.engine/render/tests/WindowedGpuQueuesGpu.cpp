#include "FrameBatch.hpp"
#include "GpuQueueBatch.hpp"
#include "RenderFixture.hpp"
#include "RendererTestHooks.hpp"
#include "VulkanTimestamps.hpp"

#include <engine/assets/Mesh.hpp>
#include <engine/core/Assert.hpp>
#include <engine/core/Name.hpp>
#include <engine/render/Renderer.hpp>
#include <engine/render/ShaderCompiler.hpp>
#include <engine/scene/DrawInstance.hpp>
#include <engine/testing/Suite.hpp>

#include <SDL3/SDL_gpu.h>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdlib>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

TEST_SUITE_ID("engine.render.windowedgpuqueuesgpu")
TEST_DEPENDS("engine.render.imagecomparison")

namespace {
	using namespace engine;
	using namespace engine::render;
	using namespace engine::render::test;

	constexpr uint32_t WIDTH = 320;
	constexpr uint32_t HEIGHT = 240;
	constexpr uint32_t VIEW_COUNT = 2;
	constexpr uint32_t RUNS_PER_VIEW = 32;

	void RequireWindowedQueueOptIn() {
		const char *enabled = std::getenv("MONO_RENDER_WINDOWED_QUEUE_TESTS");
		if (enabled == nullptr || std::string_view(enabled) != "1") {
			SKIP("Set MONO_RENDER_WINDOWED_QUEUE_TESTS=1 to create a real SDL window.");
		}
		const char *mode = std::getenv("MONO_GPU_QUEUES");
		if (mode == nullptr || (std::string_view(mode) != "serial" && std::string_view(mode) != "parallel")) {
			SKIP("Set MONO_GPU_QUEUES=serial or parallel to opt in to the private GPU queue backend.");
		}
	}

	struct WindowedFixture {
		bool VideoReady = false;
		std::unique_ptr<SDL_Window, decltype(&SDL_DestroyWindow)> Window{nullptr, SDL_DestroyWindow};
		Renderer Render;

		~WindowedFixture() {
			Render.Shutdown();
			Window.reset();
			if (VideoReady) SDL_QuitSubSystem(SDL_INIT_VIDEO);
		}

		void Initialise(bool hidden = false) {
			VideoReady = SDL_Init(SDL_INIT_VIDEO);
			REQUIRE(VideoReady);
			const SDL_WindowFlags flags = hidden ? SDL_WINDOW_HIDDEN : SDL_WINDOW_RESIZABLE;
			Window.reset(SDL_CreateWindow("Atomic windowed GPU queue test", WIDTH, HEIGHT, flags));
			INFO(SDL_GetError());
			REQUIRE(Window != nullptr);
			REQUIRE(Render.Initialise(Window.get()));
			REQUIRE_FALSE(Render.IsHeadless());
		}
	};

	struct QueueFixture {
		GpuQueueBatch *Batch = nullptr;
		SDL_GPUDevice *Device = nullptr;
		bool Parallel = false;
	};

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

	struct ResetWindowedFrameBatchFailures {
		~ResetWindowedFrameBatchFailures() {
			test_support::SetFrameBatchFailureBeforeGroupForTests(std::nullopt);
			test_support::SetFrameBatchAcquisitionFailureForTests(false);
			test_support::SetFrameBatchSubmissionFailureForTests(false);
		}
	};

	struct SolidHostOverlay final : FrameOverlayHook {
		SDL_GPUDevice *Device = nullptr;
		Renderer *RendererOwner = nullptr;
		SDL_GPUGraphicsPipeline *Pipeline = nullptr;
		SDL_GPUCommandBuffer *PrepareCommand = nullptr;
		SDL_GPUCommandBuffer *RecordCommand = nullptr;
		std::thread::id PrepareThread;
		std::thread::id RecordThread;
		uint32_t RecordCount = 0;
		uint32_t CompletionCount = 0;
		uint32_t NominalSampleFrames = 0;
		uint64_t ObservedFrameCounter = 0;
		uint32_t ObservedLegacyTimingSlot = UINT32_MAX;
		bool ObservedActiveBatch = false;
		bool LastCompletion = false;

		explicit SolidHostOverlay(Renderer &renderer)
			: Device(static_cast<SDL_GPUDevice *>(renderer.Backend().Device)), RendererOwner(&renderer) {
			ShaderCompiler compiler;
			const auto vertex = compiler.Compile(
				R"(#version 450
void main(){ vec2 p=vec2((gl_VertexIndex<<1)&2,gl_VertexIndex&2); gl_Position=vec4(p*2.0-1.0,0.0,1.0); })",
				ShaderStage::Vertex
			);
			const auto fragment = compiler.Compile(
				R"(#version 450
layout(location=0) out vec4 colour;
void main(){ colour=vec4(0.25,0.0,0.0,0.5); })",
				ShaderStage::Fragment
			);
			REQUIRE_FALSE(vertex.Failed);
			REQUIRE_FALSE(fragment.Failed);
			const auto createShader = [&](const ShaderCompilation &code, SDL_GPUShaderStage stage) {
				SDL_GPUShaderCreateInfo info{};
				info.code = reinterpret_cast<const uint8_t *>(code.SpirV.data());
				info.code_size = code.SpirV.size() * sizeof(uint32_t);
				info.entrypoint = "main";
				info.format = SDL_GPU_SHADERFORMAT_SPIRV;
				info.stage = stage;
				return SDL_CreateGPUShader(Device, &info);
			};
			auto *vertexShader = createShader(vertex, SDL_GPU_SHADERSTAGE_VERTEX);
			auto *fragmentShader = createShader(fragment, SDL_GPU_SHADERSTAGE_FRAGMENT);
			REQUIRE(vertexShader != nullptr);
			REQUIRE(fragmentShader != nullptr);
			SDL_GPUColorTargetDescription target{};
			target.format = static_cast<SDL_GPUTextureFormat>(renderer.Backend().ColourFormat);
			SDL_GPUGraphicsPipelineCreateInfo info{};
			info.vertex_shader = vertexShader;
			info.fragment_shader = fragmentShader;
			info.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
			info.target_info.color_target_descriptions = &target;
			info.target_info.num_color_targets = 1;
			Pipeline = SDL_CreateGPUGraphicsPipeline(Device, &info);
			SDL_ReleaseGPUShader(Device, vertexShader);
			SDL_ReleaseGPUShader(Device, fragmentShader);
			REQUIRE(Pipeline != nullptr);
		}

		~SolidHostOverlay() override {
			if (Pipeline != nullptr) SDL_ReleaseGPUGraphicsPipeline(Device, Pipeline);
		}

		bool Prepare(void *commandBuffer) override {
			PrepareCommand = static_cast<SDL_GPUCommandBuffer *>(commandBuffer);
			PrepareThread = std::this_thread::get_id();
			const FrameBatchTestSnapshot snapshot = FrameBatch::SnapshotForTests(*RendererOwner);
			ObservedFrameCounter = snapshot.FrameCounter;
			ObservedLegacyTimingSlot = snapshot.LegacyTimingSlot;
			ObservedActiveBatch = snapshot.Active;
			if (snapshot.Active && snapshot.FrameCounter % 4 == 0) {
				++NominalSampleFrames;
				CHECK(snapshot.LegacyTimingSlot == VulkanTimestamps::NO_SLOT);
			}
			return PrepareCommand != nullptr;
		}

		void Record(void *commandBuffer, void *renderPass) override {
			RecordCommand = static_cast<SDL_GPUCommandBuffer *>(commandBuffer);
			RecordThread = std::this_thread::get_id();
			auto *pass = static_cast<SDL_GPURenderPass *>(renderPass);
			SDL_BindGPUGraphicsPipeline(pass, Pipeline);
			SDL_DrawGPUPrimitives(pass, 3, 1, 0, 0);
			++RecordCount;
		}

		void CompleteFrame(bool submitted) override {
			LastCompletion = submitted;
			++CompletionCount;
		}
	};

	void CheckHostOverlayRecorded(const SolidHostOverlay &hook, uint32_t previousRecords) {
		CHECK(hook.RecordCount == previousRecords + 1);
		CHECK(hook.CompletionCount == hook.RecordCount);
		CHECK(hook.LastCompletion);
		CHECK(hook.PrepareCommand != nullptr);
		CHECK(hook.RecordCommand != nullptr);
		CHECK(hook.PrepareCommand != hook.RecordCommand);
		CHECK(hook.PrepareThread == hook.RecordThread);
		CHECK(hook.RecordThread == std::this_thread::get_id());
		if (hook.ObservedActiveBatch) CHECK(hook.ObservedLegacyTimingSlot == VulkanTimestamps::NO_SLOT);
	}

	QueueFixture RequireQueueFixture(Renderer &renderer) {
		auto *device = static_cast<SDL_GPUDevice *>(renderer.Backend().Device);
		REQUIRE(device != nullptr);
		const SDL_PropertiesID properties = SDL_GetGPUDeviceProperties(device);
		const auto *api = static_cast<const AtomicGpuQueuesV1 *>(
			SDL_GetPointerProperty(properties, ATOMIC_GPU_QUEUES_DEVICE_PROPERTY, nullptr)
		);
		if (api == nullptr) {
			INFO("renderer backend: " << renderer.BackendName());
			SKIP("GPU backend did not publish Atomic.gpu.vulkan.queues.v1.");
		}
		auto *batch = static_cast<GpuQueueBatch *>(
			SDL_GetPointerProperty(properties, GpuQueueBatch::DEVICE_PROPERTY, nullptr)
		);
		if (batch == nullptr) SKIP("Renderer did not publish its private queue batch.");
		const char *mode = std::getenv("MONO_GPU_QUEUES");
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

	std::array<std::vector<scene::DrawInstance>, VIEW_COUNT> AddResidentRuns(Renderer &renderer) {
		const assets::MeshData mesh = QueueQuad();
		std::array<std::vector<scene::DrawInstance>, VIEW_COUNT> instances;
		for (uint32_t view = 0; view < VIEW_COUNT; ++view) {
			instances[view].reserve(RUNS_PER_VIEW);
			for (uint32_t run = 0; run < RUNS_PER_VIEW; ++run) {
				const core::Name meshName(
					"windowed-queue-view-" + std::to_string(view) + "-mesh-" + std::to_string(run)
				);
				REQUIRE(renderer.AddMesh(meshName, mesh));

				scene::DrawInstance instance;
				instance.Source = uint64_t(view) * RUNS_PER_VIEW + run + 1;
				instance.Frame.Position = {
					static_cast<float>(view) * 5.0f + static_cast<float>(run % 8) * 0.35f - 1.2f,
					static_cast<float>(run / 8) * 0.3f - 0.5f,
					-4.0f
				};
				instance.HalfExtent = {0.12f, 0.12f, 0.025f};
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

	std::array<View, VIEW_COUNT> MakeViews(
		std::array<std::vector<scene::DrawInstance>, VIEW_COUNT> &instances, const SceneTarget &target
	) {
		std::array<View, VIEW_COUNT> views;
		for (uint32_t index = 0; index < VIEW_COUNT; ++index) {
			auto &view = views[index];
			view.Slot = index;
			view.World = 8200 + index;
			view.WorldName = core::Name(index == 0 ? "windowed-queue-world-a" : "windowed-queue-world-b");
			view.Target = &target;
			view.Instances = instances[index];
			view.Camera.FieldOfViewRadians = 1.2f;
			view.Camera.NearPlane = 0.25f;
			view.Camera.FarPlane = 32.0f;
			view.CameraFrame.Position.X = static_cast<float>(index) * 5.0f;
		}
		return views;
	}

	CapturedImage CaptureComposed(Renderer &renderer, size_t slot) {
		return CaptureResource(
			renderer, core::Name("composed-image"), slot, WIDTH, HEIGHT, ImageFormat::Bgra8Unorm
		);
	}

	void CheckPresentationStatistics(const GpuQueueBatch::Statistics &statistics, bool parallel) {
		CHECK(statistics.PresentationAdopted);
		CHECK(statistics.PresentationHandoffs == 1);
		CHECK(statistics.Complete);
		CHECK(statistics.Recorded >= VIEW_COUNT);
		CHECK(statistics.Submitted > 0);
		CHECK((statistics.WorkerRecorded > 0) == parallel);
	}
}

TEST_CASE(
	"private GPU queues adopt one real window presentation after resident moving views",
	"[render][gpu][queues][windowed][wsi][.]"
) {
	RequireWindowedQueueOptIn();
	WindowedFixture fixture;
	fixture.Initialise();
	fixture.Render.SetProfiling(ProfilingTier::Full);
	const QueueFixture queues = RequireQueueFixture(fixture.Render);
	SolidHostOverlay hostOverlay(fixture.Render);
	auto instances = AddResidentRuns(fixture.Render);
	const SceneTarget target{WIDTH, HEIGHT};
	auto views = MakeViews(instances, target);
	OverlayImage overlay;

	const uint64_t generationBefore = fixture.Render.RenderGeneration();
	uint32_t previousRecords = hostOverlay.RecordCount;
	const FrameResult upload = fixture.Render.Render(views, overlay, nullptr, true, &hostOverlay);
	REQUIRE(upload.Submitted);
	REQUIRE(upload.Presented);
	CheckHostOverlayRecorded(hostOverlay, previousRecords);
	CHECK(fixture.Render.RenderGeneration() > generationBefore);
	CheckPresentationStatistics(queues.Batch->LastStatistics(), queues.Parallel);

	const uint64_t generationAfterUpload = fixture.Render.RenderGeneration();
	views[0].CameraFrame.Position.X += 0.12f;
	views[1].CameraFrame.Position.X -= 0.12f;
	for (View &view : views) {
		view.Damage.Scene = true;
		view.Damage.Viewport = true;
	}
	previousRecords = hostOverlay.RecordCount;
	const FrameResult moving = fixture.Render.Render(views, overlay, nullptr, true, &hostOverlay);
	REQUIRE(moving.Submitted);
	REQUIRE(moving.Presented);
	CheckHostOverlayRecorded(hostOverlay, previousRecords);
	CHECK(moving.DrawCalls >= VIEW_COUNT * RUNS_PER_VIEW);
	CHECK(fixture.Render.RenderGeneration() > generationAfterUpload);
	CheckPresentationStatistics(queues.Batch->LastStatistics(), queues.Parallel);

	const std::array<CapturedImage, VIEW_COUNT> queued{
		CaptureComposed(fixture.Render, views[0].Slot), CaptureComposed(fixture.Render, views[1].Slot)
	};

	// A separate device without MONO_GPU_QUEUES supplies the ordinary fallback image oracle.
	ScopedQueueOptOut optOut;
	REQUIRE(optOut.Clear());
	WindowedFixture ordinary;
	ordinary.Initialise(true);
	REQUIRE(optOut.Restore());
	const auto ordinaryProperties =
		SDL_GetGPUDeviceProperties(static_cast<SDL_GPUDevice *>(ordinary.Render.Backend().Device));
	CHECK(SDL_GetPointerProperty(ordinaryProperties, GpuQueueBatch::DEVICE_PROPERTY, nullptr) == nullptr);
	auto ordinaryInstances = AddResidentRuns(ordinary.Render);
	const SceneTarget ordinaryTarget{WIDTH, HEIGHT};
	auto ordinaryViews = MakeViews(ordinaryInstances, ordinaryTarget);
	OverlayImage ordinaryOverlay;
	const FrameResult ordinaryWarm = ordinary.Render.Render(ordinaryViews, ordinaryOverlay, nullptr, false);
	REQUIRE(ordinaryWarm.Submitted);
	ordinaryViews[0].CameraFrame.Position.X += 0.12f;
	ordinaryViews[1].CameraFrame.Position.X -= 0.12f;
	for (View &view : ordinaryViews) {
		view.Damage.Scene = true;
		view.Damage.Viewport = true;
	}
	const FrameResult ordinaryMoving = ordinary.Render.Render(ordinaryViews, ordinaryOverlay, nullptr, false);
	REQUIRE(ordinaryMoving.Submitted);
	for (size_t index = 0; index < ordinaryViews.size(); ++index) {
		const CapturedImage fallback = CaptureComposed(ordinary.Render, ordinaryViews[index].Slot);
		CHECK(queued[index].Bytes == fallback.Bytes);
	}

	SDL_SetWindowSize(fixture.Window.get(), WIDTH + 80, HEIGHT + 60);
	SDL_Event event;
	while (SDL_PollEvent(&event)) {}
	bool resizeFramePresented = false;
	for (uint32_t attempt = 0; attempt < 8 && !resizeFramePresented; ++attempt) {
		for (View &view : views) {
			view.Damage.Scene = true;
			view.Damage.Viewport = true;
		}
		previousRecords = hostOverlay.RecordCount;
		const FrameResult resized = fixture.Render.Render(views, overlay, nullptr, true, &hostOverlay);
		resizeFramePresented = resized.Submitted && resized.Presented;
		if (resizeFramePresented) CheckHostOverlayRecorded(hostOverlay, previousRecords);
		if (!resizeFramePresented) SDL_Delay(10);
	}
	REQUIRE(resizeFramePresented);
	for (View &view : views) {
		view.Damage.Scene = true;
		view.Damage.Viewport = true;
	}
	previousRecords = hostOverlay.RecordCount;
	const FrameResult afterResize = fixture.Render.Render(views, overlay, nullptr, true, &hostOverlay);
	REQUIRE(afterResize.Submitted);
	REQUIRE(afterResize.Presented);
	CheckHostOverlayRecorded(hostOverlay, previousRecords);
	CHECK(hostOverlay.NominalSampleFrames > 0);
	CheckPresentationStatistics(queues.Batch->LastStatistics(), queues.Parallel);
}

TEST_CASE(
	"a refused first window handoff reports failure and recovers after failed flight retirement",
	"[render][gpu][queues][windowed][wsi][.]"
) {
	RequireWindowedQueueOptIn();
#if !ENGINE_ASSERTS_ENABLED
	SKIP("The first handoff refusal hook is active only when ENGINE_ASSERTS_ENABLED is non-zero.");
#else
	WindowedFixture fixture;
	fixture.Initialise();
	const QueueFixture queues = RequireQueueFixture(fixture.Render);
	auto instances = AddResidentRuns(fixture.Render);
	const SceneTarget target{WIDTH, HEIGHT};
	auto views = MakeViews(instances, target);
	OverlayImage overlay;
	queues.Batch->RefuseFirstHandoffResultForTests();
	queues.Batch->RefuseDrainForTests();

	const FrameResult refused = fixture.Render.Render(views, overlay, nullptr, true);
	CHECK_FALSE(refused.Submitted);
	CHECK_FALSE(refused.Presented);
	CHECK(queues.Batch->LastStatistics().PresentationAdopted);
	CHECK(queues.Batch->LastStatistics().PresentationHandoffs == 1);
	CHECK(queues.Batch->LastStatistics().HandoffAttempts > 0);
	CHECK_FALSE(queues.Batch->LastStatistics().Complete);

	SDL_GPUCommandBuffer *blockedCommand = nullptr;
	CHECK_FALSE(queues.Batch->Begin(&blockedCommand));
	CHECK(blockedCommand == nullptr);
	REQUIRE(queues.Batch->RetireFailed());

	for (View &view : views) {
		view.Damage.Scene = true;
		view.Damage.Viewport = true;
	}
	const FrameResult recovery = fixture.Render.Render(views, overlay, nullptr, true);
	REQUIRE(recovery.Submitted);
	REQUIRE(recovery.Presented);
	CheckPresentationStatistics(queues.Batch->LastStatistics(), queues.Parallel);
#endif
}

#if ENGINE_ASSERTS_ENABLED
TEST_CASE(
	"a late window owner prefix failure retains uncertain GPU ownership until retirement",
	"[render][gpu][queues][windowed][wsi][.]"
) {
	RequireWindowedQueueOptIn();
	WindowedFixture fixture;
	fixture.Initialise();
	fixture.Render.SetProfiling(ProfilingTier::Full);
	const QueueFixture queues = RequireQueueFixture(fixture.Render);
	SolidHostOverlay hostOverlay(fixture.Render);
	auto instances = AddResidentRuns(fixture.Render);
	const SceneTarget target{WIDTH, HEIGHT};
	auto views = MakeViews(instances, target);
	OverlayImage overlay;

	for (uint32_t warmFrame = 0; warmFrame < 3; ++warmFrame) {
		for (View &view : views) {
			view.Damage.Scene = true;
			view.Damage.Viewport = true;
		}
		const FrameResult warm = fixture.Render.Render(views, overlay, nullptr, true, &hostOverlay);
		REQUIRE(warm.Submitted);
		REQUIRE(warm.Presented);
		CheckPresentationStatistics(queues.Batch->LastStatistics(), queues.Parallel);
	}

	queues.Batch->RefuseSubmissionAfterForTests(2);
	queues.Batch->RefuseDrainForTests();
	for (View &view : views) {
		view.Damage.Scene = true;
		view.Damage.Viewport = true;
	}
	const FrameResult failed = fixture.Render.Render(views, overlay, nullptr, true, &hostOverlay);
	CHECK_FALSE(failed.Submitted);
	CHECK_FALSE(failed.Presented);
	CHECK(hostOverlay.ObservedFrameCounter == 4);
	CHECK(hostOverlay.ObservedActiveBatch);
	CHECK(hostOverlay.ObservedLegacyTimingSlot == VulkanTimestamps::NO_SLOT);
	CHECK(hostOverlay.NominalSampleFrames == 1);
	const GpuQueueBatch::Statistics failedStatistics = queues.Batch->LastStatistics();
	CHECK(failedStatistics.PresentationAdopted);
	CHECK(failedStatistics.PresentationHandoffs == 1);
	CHECK(failedStatistics.Submitted == 2);
	CHECK_FALSE(failedStatistics.Complete);

	SDL_GPUCommandBuffer *blockedCommand = nullptr;
	CHECK_FALSE(queues.Batch->Begin(&blockedCommand));
	CHECK(blockedCommand == nullptr);
	REQUIRE(queues.Batch->RetireFailed());

	for (View &view : views) {
		view.Damage.Scene = true;
		view.Damage.Viewport = true;
	}
	const FrameResult recovery = fixture.Render.Render(views, overlay, nullptr, true, &hostOverlay);
	REQUIRE(recovery.Submitted);
	REQUIRE(recovery.Presented);
	CheckPresentationStatistics(queues.Batch->LastStatistics(), queues.Parallel);
}
#endif

TEST_CASE(
	"a hidden window with no acquired swapchain image shuts down cleanly",
	"[render][gpu][queues][windowed][wsi][.]"
) {
	RequireWindowedQueueOptIn();
	WindowedFixture fixture;
	fixture.Initialise(true);
	const QueueFixture queues = RequireQueueFixture(fixture.Render);
	CHECK_FALSE(fixture.Render.WaitForFrame());
	CHECK_FALSE(queues.Batch->LastStatistics().PresentationAdopted);
	fixture.Render.Shutdown();
}

TEST_CASE(
	"an acquired visible presentation can be retired by shutdown without rendering",
	"[render][gpu][queues][windowed][wsi][.]"
) {
	RequireWindowedQueueOptIn();
	WindowedFixture fixture;
	fixture.Initialise();
	RequireQueueFixture(fixture.Render);
	REQUIRE(fixture.Render.WaitForFrame());
	fixture.Render.Shutdown();
}

#if ENGINE_ASSERTS_ENABLED
TEST_CASE(
	"a group-zero window frame failure submits cleanup presentation once",
	"[render][gpu][queues][windowed][wsi][.]"
) {
	RequireWindowedQueueOptIn();
	WindowedFixture fixture;
	fixture.Initialise();
	const QueueFixture queues = RequireQueueFixture(fixture.Render);
	auto instances = AddResidentRuns(fixture.Render);
	const SceneTarget target{WIDTH, HEIGHT};
	auto views = MakeViews(instances, target);
	OverlayImage overlay;
	ResetWindowedFrameBatchFailures reset;
	test_support::SetFrameBatchFailureBeforeGroupForTests(0);

	const FrameResult failed = fixture.Render.Render(views, overlay, nullptr, true);
	CHECK_FALSE(failed.Submitted);
	CHECK_FALSE(failed.Presented);
	CHECK(queues.Batch->LastStatistics().PresentationAdopted);
	CHECK(queues.Batch->LastStatistics().PresentationHandoffs == 1);
	CHECK_FALSE(queues.Batch->LastStatistics().Complete);

	test_support::SetFrameBatchFailureBeforeGroupForTests(std::nullopt);
	for (View &view : views) {
		view.Damage.Scene = true;
		view.Damage.Viewport = true;
	}
	const FrameResult recovered = fixture.Render.Render(views, overlay, nullptr, true);
	REQUIRE(recovered.Submitted);
	REQUIRE(recovered.Presented);
	CheckPresentationStatistics(queues.Batch->LastStatistics(), queues.Parallel);
}

TEST_CASE(
	"a final window submission failure retires earlier packets and recovers",
	"[render][gpu][queues][windowed][wsi][.]"
) {
	RequireWindowedQueueOptIn();
	WindowedFixture fixture;
	fixture.Initialise();
	const QueueFixture queues = RequireQueueFixture(fixture.Render);
	auto instances = AddResidentRuns(fixture.Render);
	const SceneTarget target{WIDTH, HEIGHT};
	auto views = MakeViews(instances, target);
	OverlayImage overlay;
	ResetWindowedFrameBatchFailures reset;
	test_support::SetFrameBatchSubmissionFailureForTests(true);

	const FrameResult failed = fixture.Render.Render(views, overlay, nullptr, true);
	CHECK_FALSE(failed.Submitted);
	CHECK_FALSE(failed.Presented);
	CHECK(queues.Batch->LastStatistics().PresentationAdopted);
	CHECK(queues.Batch->LastStatistics().PresentationHandoffs == 1);
	CHECK(queues.Batch->LastStatistics().Submitted > 0);
	CHECK_FALSE(queues.Batch->LastStatistics().Complete);

	test_support::SetFrameBatchSubmissionFailureForTests(false);
	for (View &view : views) {
		view.Damage.Scene = true;
		view.Damage.Viewport = true;
	}
	const FrameResult recovered = fixture.Render.Render(views, overlay, nullptr, true);
	REQUIRE(recovered.Submitted);
	REQUIRE(recovered.Presented);
	CheckPresentationStatistics(queues.Batch->LastStatistics(), queues.Parallel);
}
#endif
