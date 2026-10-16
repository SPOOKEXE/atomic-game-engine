#include "GpuQueueBatch.hpp"
#include "RecordingGpuCapture.hpp"

#include <engine/assets/Mesh.hpp>
#include <engine/core/FrameGraph.hpp>
#include <engine/core/HeapProfile.hpp>
#include <engine/render/Renderer.hpp>
#include <engine/script/ComputeBridge.hpp>
#include <engine/testing/Bench.hpp>

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

TEST_SUITE_ID("engine.render.bench.recording-batch")

namespace {
	using Clock = std::chrono::steady_clock;
	using namespace engine;
	namespace cg = computegraph;
	constexpr size_t MAX_RECORDING_FRAMES = 4096;
	constexpr std::array QUEUE_NAMES{"graphics", "compute", "transfer"};

	uint64_t NextRecordingInvocationId() {
		static uint64_t next = 0;
		return ++next;
	}

	void PrintCaptureIdentity(uint64_t invocationId, const AtomicGpuQueuesV1 *api) {
		std::cout << " invocation_id=" << invocationId;
		if (api == nullptr) {
			std::cout << " native_device_handle=unavailable";
			for (const auto queueName : QUEUE_NAMES)
				std::cout << " queue_" << queueName << "_handle=unavailable queue_" << queueName
						  << "_family=unavailable queue_" << queueName << "_index=unavailable";
			return;
		}
		std::cout << " native_device_handle=0x" << std::hex
				  << static_cast<uint64_t>(reinterpret_cast<uintptr_t>(api->Device)) << std::dec;
		for (size_t queue = 0; queue < QUEUE_NAMES.size(); ++queue) {
			std::cout << " queue_" << QUEUE_NAMES[queue] << "_handle=0x" << std::hex
					  << static_cast<uint64_t>(reinterpret_cast<uintptr_t>(api->Queue[queue])) << std::dec
					  << " queue_" << QUEUE_NAMES[queue] << "_family=" << api->Family[queue] << " queue_"
					  << QUEUE_NAMES[queue] << "_index=" << api->QueueIndex[queue];
		}
	}

	void PrintHeapSnapshot(std::string_view name, const core::HeapTotals &heap) {
		std::cout << ' ' << name << "_live_bytes=" << heap.LiveBytes << ' ' << name
				  << "_live_blocks=" << heap.LiveBlocks << ' ' << name
				  << "_lifetime_peak_bytes=" << heap.PeakBytes << ' ' << name
				  << "_lifetime_allocated_bytes=" << heap.TotalBytes << ' ' << name
				  << "_lifetime_allocated_blocks=" << heap.TotalBlocks << ' ' << name
				  << "_profiler_overhead_bytes=" << heap.OverheadBytes << ' ' << name
				  << "_foreign_frees=" << heap.ForeignFrees << ' ' << name
				  << "_dropped_scopes=" << heap.DroppedScopes;
	}

	size_t RecordingFrameCount() {
		const char *option = std::getenv("MONO_RENDER_RECORDING_FRAMES");
		if (option == nullptr) return 256;
		const std::string_view text(option);
		unsigned value = 0;
		const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
		if (error != std::errc{} || end != text.data() + text.size() || value < 16 ||
			value > MAX_RECORDING_FRAMES)
			throw std::runtime_error("MONO_RENDER_RECORDING_FRAMES must be an integer from 16 through 4096");
		return value;
	}

	uint32_t RecordingComputeElementCount() {
		const char *option = std::getenv("MONO_RENDER_RECORDING_COMPUTE_ELEMENTS");
		if (option == nullptr) return 262144;
		const std::string_view text(option);
		uint32_t value = 0;
		const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
		// The bridge retains two full result copies against its 16 MiB script result budget.
		if (error != std::errc{} || end != text.data() + text.size() || value < 262144 || value > 524288)
			throw std::runtime_error(
				"MONO_RENDER_RECORDING_COMPUTE_ELEMENTS must be an integer from 262144 through 524288; "
				"the two retained output copies share a 16 MiB budget"
			);
		return value;
	}

	std::string ComputeStatusDetail(cg::Status status, std::string_view detail) {
		const std::string detailText = detail.empty() ? "unavailable" : std::string(detail);
		return std::string("status=") + cg::Describe(status) + " detail=" + detailText;
	}

	template <typename Sample, size_t Capacity>
	void PrintTails(std::string_view name, const std::array<Sample, Capacity> &samples, size_t count) {
		std::cout << ' ' << name << "_samples=" << count << ' ' << name
				  << "_tail_insufficient=" << (count < 100 ? 1 : 0);
		if (count == 0) {
			std::cout << ' ' << name << "_p50=unavailable " << name << "_p95=unavailable " << name
					  << "_p99=unavailable " << name << "_max=unavailable";
			return;
		}
		std::array<double, Capacity> sorted{};
		for (size_t index = 0; index < count; ++index)
			sorted[index] = static_cast<double>(samples[index]);
		std::sort(sorted.begin(), sorted.begin() + static_cast<std::ptrdiff_t>(count));
		const auto percentile = [&](size_t percent) {
			const size_t rank = (count * percent + 99) / 100;
			return sorted[rank - 1];
		};
		std::cout << ' ' << name << "_percentile_method=nearest_rank " << name << "_p50=" << percentile(50)
				  << ' ' << name << "_p95=" << percentile(95) << ' ' << name << "_p99=" << percentile(99)
				  << ' ' << name << "_max=" << sorted[count - 1];
	}

	uint64_t Elapsed(Clock::time_point start) {
		return static_cast<uint64_t>(
			std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start).count()
		);
	}

	cg::Program RecordingComputeProgram(uint32_t capacity) {
		cg::Program program;
		for (const auto name : {"input", "intermediate", "output"})
			program.Definition.Buffers.push_back(
				{.Name = name,
				 .Type = cg::ElementType::F32x4,
				 .Stride = 16,
				 .Capacity = capacity,
				 .LogicalCount = capacity,
				 .Permission = name == std::string_view("input") ? cg::Access::Read : cg::Access::ReadWrite,
				 .External = name == std::string_view("input")}
			);
		program.Definition.Kernels.push_back(
			{.Name = "rotate-add",
			 .Ports = {
				 {.Name = "Values", .Type = cg::ElementType::F32x4, .Stride = 16},
				 {.Name = "Result",
				  .Type = cg::ElementType::F32x4,
				  .Stride = 16,
				  .Permission = cg::Access::Write,
				  .Set = 1}
			 }}
		);
		program.Kernels.push_back({.Name = "rotate-add", .Source = R"GLSL(#version 450
layout(local_size_x=64) in;
layout(set=2,binding=0,std140) uniform ComputeParameters { uvec4 Dispatch; uvec4 Buffers[32]; } config;
layout(set=0,binding=0,std430) readonly buffer Values { vec4 Data[]; } values;
layout(set=1,binding=0,std430) writeonly buffer Result { vec4 Data[]; } result;
void main() {
    uint i=gl_GlobalInvocationID.x;
    if(i>=config.Dispatch.x || i>=config.Buffers[0].y || i>=config.Buffers[16].y) return;
    vec4 v=values.Data[i];
    for(uint step=0u;step<16u;++step) v=v.yzwx+vec4(1,2,3,4);
    result.Data[i]=v;
}
)GLSL"});
		program.Definition.Passes = {
			{.Name = "first",
			 .Kernel = "rotate-add",
			 .DispatchBuffer = "input",
			 .Bindings = {{"Values", "input"}, {"Result", "intermediate"}}},
			{.Name = "second",
			 .Kernel = "rotate-add",
			 .DispatchBuffer = "intermediate",
			 .Bindings = {{"Values", "intermediate"}, {"Result", "output"}}}
		};
		return program;
	}
	template <size_t Count> void RunRecordingBatch() {
		const size_t measuredFrames = RecordingFrameCount();
		const uint32_t computeElements = RecordingComputeElementCount();
		if (std::getenv("MONO_RENDER_RECORDING_PROFILE") == nullptr)
			throw std::runtime_error(
				"set MONO_RENDER_RECORDING_PROFILE=1 to opt in to the recording profile"
			);
		if (!SDL_Init(SDL_INIT_VIDEO)) throw std::runtime_error(SDL_GetError());
		struct VideoQuit {
			~VideoQuit() {
				SDL_QuitSubSystem(SDL_INIT_VIDEO);
			}
		} videoQuit;
		struct RestoreRecording {
			bool Enabled = core::FrameGraph::IsEnabled();
			~RestoreRecording() {
				core::FrameGraph::SetEnabled(Enabled);
			}
		} restore;
		core::FrameGraph::SetEnabled(true);
		render::Renderer renderer;
		if (!renderer.Initialise(nullptr)) throw std::runtime_error("recording profile device unavailable");
		renderer.SetProfiling(render::ProfilingTier::Full);
		auto *device = static_cast<SDL_GPUDevice *>(renderer.Backend().Device);
		const auto properties = SDL_GetGPUDeviceProperties(device);
		auto *batch = static_cast<render::GpuQueueBatch *>(
			SDL_GetPointerProperty(properties, render::GpuQueueBatch::DEVICE_PROPERTY, nullptr)
		);
		const uint64_t invocationId = NextRecordingInvocationId();
		const auto *api = batch ? batch->Api() : nullptr;
		std::cout << "render-recording-capture";
		PrintCaptureIdentity(invocationId, api);
		std::cout << '\n' << std::flush;
		const char *computeOption = std::getenv("MONO_RENDER_RECORDING_COMPUTE");
		const bool computeEnabled = computeOption && std::string_view(computeOption) == "1";
		std::shared_ptr<script::ComputeBridge> bridge;
		cg::Request computeRequest;
		std::vector<std::byte> expectedCompute;
		uint64_t computeTicket = 0;
		struct ReleaseTicket {
			std::shared_ptr<script::ComputeBridge> &Bridge;
			uint64_t &Ticket;
			~ReleaseTicket() {
				if (Bridge && Ticket != 0) {
					Bridge->Cancel(Ticket);
					Bridge->Release(Ticket);
				}
			}
		} releaseTicket{bridge, computeTicket};
		const auto finishCompute = [&](bool verify) {
			if (!bridge || computeTicket == 0) return;
			script::ComputePoll reply;
			const auto deadline = Clock::now() + std::chrono::seconds(20);
			while (Clock::now() < deadline) {
				renderer.PumpCompute();
				const auto status = bridge->Poll(computeTicket, reply);
				if (status != cg::Status::Ok)
					throw std::runtime_error(
						"recording profile compute poll failed: " + ComputeStatusDetail(status, reply.Detail)
					);
				if (reply.Ready) break;
				SDL_Delay(1);
			}
			if (reply.Cancelled)
				throw std::runtime_error(
					"recording profile compute cancelled: " + ComputeStatusDetail(reply.Code, reply.Detail)
				);
			if (!reply.Ready)
				throw std::runtime_error(
					"recording profile compute timed out: " + ComputeStatusDetail(reply.Code, reply.Detail)
				);
			if (reply.Code != cg::Status::Ok)
				throw std::runtime_error(
					"recording profile compute failed: " + ComputeStatusDetail(reply.Code, reply.Detail)
				);
			if (verify) {
				if (reply.Outputs.size() != 1 || reply.Outputs[0].Name != "output" ||
					reply.Outputs[0].Type != cg::ElementType::F32x4 ||
					reply.Outputs[0].Count != computeElements ||
					reply.Outputs[0].Bytes != expectedCompute.size())
					throw std::runtime_error("recording profile compute output shape differs");
				std::vector<std::byte> chunk;
				for (size_t offset = 0; offset < expectedCompute.size(); offset += 65536) {
					const auto bytes = std::min(size_t{65536}, expectedCompute.size() - offset);
					const auto status = bridge->Read(computeTicket, "output", offset, bytes, chunk);
					if (status != cg::Status::Ok)
						throw std::runtime_error(
							"recording profile compute read failed: " + ComputeStatusDetail(status, {})
						);
					if (chunk.size() != bytes ||
						std::memcmp(chunk.data(), expectedCompute.data() + offset, bytes) != 0)
						throw std::runtime_error("recording profile compute exact CPU oracle differs");
				}
			}
			if (!bridge->Release(computeTicket))
				throw std::runtime_error("recording profile compute ticket release failed");
			computeTicket = 0;
			renderer.PumpCompute();
		};
		if (computeEnabled) {
			auto factory = renderer.ComputeForWorld("recording-profile-world", 0);
			if (!factory) throw std::runtime_error("recording profile compute factory unavailable");
			bridge = factory(71, 9);
			if (!bridge || bridge->Identity() != cg::Owner{"recording-profile-world", 71, 9, 0})
				throw std::runtime_error("recording profile compute endpoint identity differs");
			std::string detail;
			const auto defineStatus = bridge->Define(
				"recording-rotate-add", RecordingComputeProgram(computeElements), computeTicket, detail
			);
			if (defineStatus != cg::Status::Ok)
				throw std::runtime_error(
					"recording profile compute definition refused: " +
					ComputeStatusDetail(defineStatus, detail)
				);
			finishCompute(false);
			computeRequest.Identity = bridge->Identity();
			cg::Upload upload{.Buffer = "input", .Type = cg::ElementType::F32x4, .Count = computeElements};
			upload.Bytes.resize(size_t{computeElements} * 4 * sizeof(float));
			expectedCompute.resize(upload.Bytes.size());
			for (size_t index = 0; index < size_t{computeElements} * 4; ++index) {
				const float input = static_cast<float>(index % 128);
				const float expected = input + 80;
				std::memcpy(upload.Bytes.data() + index * sizeof(float), &input, sizeof(float));
				std::memcpy(expectedCompute.data() + index * sizeof(float), &expected, sizeof(float));
			}
			computeRequest.Inputs.push_back(std::move(upload));
			computeRequest.Outputs.push_back({"output", computeElements});
		}
		const auto submitCompute = [&] {
			if (!bridge) return;
			std::string detail;
			const auto status = bridge->Submit("recording-rotate-add", computeRequest, computeTicket, detail);
			if (status != cg::Status::Ok)
				throw std::runtime_error(
					"recording profile compute submission refused: " + ComputeStatusDetail(status, detail)
				);
		};
		constexpr size_t ROWS = 1024;
		constexpr size_t WARM_FRAMES = 16;
		assets::MeshData mesh;
		mesh.Vertices = {
			{{-0.5f, -0.5f, 0}, {0, 0, 1}, {0, 1}},
			{{0.5f, -0.5f, 0}, {0, 0, 1}, {1, 1}},
			{{0.5f, 0.5f, 0}, {0, 0, 1}, {1, 0}},
			{{-0.5f, 0.5f, 0}, {0, 0, 1}, {0, 0}},
		};
		mesh.Indices = {0, 1, 2, 0, 2, 3};
		mesh.ComputeBounds();
		std::vector<scene::DrawInstance> instances(ROWS);
		for (size_t row = 0; row < ROWS; ++row) {
			const core::Name name("recording-profile-mesh-" + std::to_string(row));
			if (!renderer.AddMesh(name, mesh))
				throw std::runtime_error("recording profile mesh upload failed");
			auto &instance = instances[row];
			instance.Source = row + 1;
			instance.Mesh = name;
			instance.CastShadow = false;
			instance.HalfExtent = {0.4f, 0.4f, 0.01f};
			instance.Frame.Position = {
				static_cast<float>(row % 32) - 15.5f, static_cast<float>(row / 32) - 15.5f, -40.0f
			};
		}
		std::array<render::SceneTarget, Count> targets;
		std::array<render::View, Count> views;
		for (size_t index = 0; index < Count; ++index) {
			targets[index] = {128, 128};
			auto &view = views[index];
			view.Target = &targets[index];
			view.Slot = index;
			view.World = 1;
			view.WorldName = core::Name("recording-profile-world");
			view.SnapshotId = "recording-profile-snapshot";
			view.Instances = instances;
			view.Camera.FarPlane = 100;
			view.Grid.Enabled = false;
		}
		render::OverlayImage overlay;
		const auto advance = [&](size_t frame) {
			for (size_t index = 0; index < views.size(); ++index) {
				auto &view = views[index];
				view.CameraFrame.Position.X =
					static_cast<float>(index) * 0.01f + ((frame & 1u) == 0 ? -0.025f : 0.025f);
				view.Damage = {.Scene = true, .Viewport = true};
			}
		};
		const auto wait = [&] {
			const auto started = Clock::now();
			if (!SDL_WaitForGPUIdle(device)) throw std::runtime_error(SDL_GetError());
			return Elapsed(started);
		};
		for (size_t frame = 0; frame < WARM_FRAMES; ++frame) {
			wait();
			advance(frame);
			submitCompute();
			core::FrameGraph::BeginFrame();
			if (!renderer.Render(views, overlay, nullptr, false).Submitted)
				throw std::runtime_error("recording profile warm-up failed");
			core::FrameGraph::EndFrame();
			finishCompute(true);
		}
		RecordingGpuCapture capture(device);
		struct SpanTotal {
			std::string_view Name;
			std::string_view Prefix;
			double Inclusive = 0;
			double Self = 0;
			size_t SpanCount = 0;
		};
		std::array spans{
			SpanTotal{"ViewRecording::Begin", "cpu_view_begin"},
			SpanTotal{"draw slots", "cpu_draw_slots"},
			SpanTotal{"execute graph", "cpu_execute_graph"},
			SpanTotal{"FrameBatch::plan pipelines", "cpu_pipeline_plan"},
			SpanTotal{"submit.scene", "cpu_scene_submit"},
			SpanTotal{"FrameBatch::submit command", "cpu_batch_submit_scope"},
			SpanTotal{"FrameBatch::settle", "cpu_batch_settle"},
		};
		uint64_t cpuNanoseconds = 0, drawCalls = 0, uploadedBytes = 0, dirtyRows = 0;
		size_t droppedSpans = 0, droppedMarks = 0, gpuSamples = 0;
		uint64_t sequence = renderer.PassTimingSequence();
		double gpuPassMicroseconds = 0;
		uint64_t queueSequence = batch ? batch->CompletedDeviceStatistics().Sequence : 0;
		size_t queueCompletedFrames = 0, queueDropped = 0, batchCompleted = 0;
		uint64_t recorded = 0, workerRecorded = 0, submitted = 0, edges = 0;
		double producerMicroseconds = 0, joinMicroseconds = 0, cpuOverlapMicroseconds = 0;
		uint64_t cpuDeviceIdleWaitNanoseconds = 0, submissionWorkerWaits = 0;
		double submissionWorkerWaitMicroseconds = 0;
		std::array<uint64_t, MAX_RECORDING_FRAMES> cpuRenderSamples{};
		std::array<uint64_t, MAX_RECORDING_FRAMES> deviceIdleWaitSamples{};
		std::array<double, MAX_RECORDING_FRAMES> producerSamples{}, joinSamples{};
		std::array<double, MAX_RECORDING_FRAMES> submissionWorkerWaitSamples{};
		size_t controllerSampleCount = 0;
		uint64_t firstControllerSequence = 0, lastControllerSequence = 0;
		std::array<std::array<double, MAX_RECORDING_FRAMES>, ATOMIC_GPU_QUEUE_COUNT> queueFrameSamples{};
		std::array<size_t, ATOMIC_GPU_QUEUE_COUNT> queueFrameSampleCounts{};
		std::array<double, 3> recorderMicroseconds{};
		std::array<uint64_t, 3> recorderTasks{};
		std::array<double, ATOMIC_GPU_QUEUE_COUNT> queueMicroseconds{};
		std::array<uint64_t, ATOMIC_GPU_QUEUE_COUNT> queueSamples{};
		const auto gpuBefore = renderer.MemoryStatistics();
		const auto presteady = core::HeapProfile::Totals();
		render::GpuMemoryStatistics gpuAfter;
		core::HeapTotals measuredEnd{};
		for (size_t frame = 0; frame <= measuredFrames; ++frame) {
			// Host API duration before each measured render; warm-up, capture flush and final drain stay
			// separate.
			const uint64_t deviceIdleWaitNanoseconds = wait();
			if (frame == measuredFrames) {
				gpuAfter = renderer.MemoryStatistics();
				measuredEnd = core::HeapProfile::Totals();
			}
			advance(WARM_FRAMES + frame);
			submitCompute();
			core::FrameGraph::BeginFrame();
			const uint64_t previousControllerSequence = batch ? batch->LastStatistics().Sequence : 0;
			const auto started = Clock::now();
			const auto result = renderer.Render(views, overlay, nullptr, false);
			const auto elapsed = Elapsed(started);
			core::FrameGraph::EndFrame();
			if (!result.Submitted || result.DrawCalls < ROWS * Count)
				throw std::runtime_error("recording profile did not submit the required mesh draw runs");
			if (batch && (batch->LastStatistics().Sequence == previousControllerSequence ||
						  !batch->LastStatistics().Complete))
				throw std::runtime_error("recording profile did not complete a new controller sequence");
			if (computeEnabled && batch &&
				(!batch->LastStatistics().Complete || batch->LastStatistics().Recorded < Count + 3))
				throw std::runtime_error("recording profile compute and raster did not share the batch");
			if (frame < measuredFrames) {
				cpuNanoseconds += elapsed;
				cpuRenderSamples[frame] = elapsed;
				cpuDeviceIdleWaitNanoseconds += deviceIdleWaitNanoseconds;
				deviceIdleWaitSamples[frame] = deviceIdleWaitNanoseconds;
				drawCalls += result.DrawCalls;
				uploadedBytes += result.UploadedBytes;
				dirtyRows += result.InstanceRowsDirty;
				droppedSpans += core::FrameGraph::Dropped();
				droppedMarks += renderer.DroppedProfileMarks();
				if (batch) {
					const auto &stats = batch->LastStatistics();
					if (stats.Sequence != 0) {
						if (controllerSampleCount == 0) firstControllerSequence = stats.Sequence;
						lastControllerSequence = stats.Sequence;
						producerSamples[controllerSampleCount] = stats.ProducerMicroseconds * 1000.0;
						joinSamples[controllerSampleCount] = stats.JoinMicroseconds * 1000.0;
						submissionWorkerWaitSamples[controllerSampleCount] =
							stats.SubmissionWorkerWaitMicroseconds;
						++controllerSampleCount;
					}
					recorded += stats.Recorded;
					workerRecorded += stats.WorkerRecorded;
					submitted += stats.Submitted;
					edges += stats.Edges;
					producerMicroseconds += stats.ProducerMicroseconds;
					joinMicroseconds += stats.JoinMicroseconds;
					submissionWorkerWaitMicroseconds += stats.SubmissionWorkerWaitMicroseconds;
					submissionWorkerWaits += stats.SubmissionWorkerWaits;
					cpuOverlapMicroseconds += stats.ParallelOverlapMicroseconds;
					for (size_t recorder = 0; recorder < 3; ++recorder) {
						recorderMicroseconds[recorder] += stats.RecorderMicroseconds[recorder];
						recorderTasks[recorder] += stats.RecorderTasks[recorder];
					}
					batchCompleted += stats.Complete;
				}
				for (const auto &span : core::FrameGraph::Spans()) {
					if (span.Reported || span.Category != core::ProfileCategory::Render) continue;
					for (auto &total : spans) {
						if (span.Name != total.Name) continue;
						total.Inclusive += span.Milliseconds;
						total.Self += span.SelfMilliseconds;
						++total.SpanCount;
						break;
					}
				}
			}
			const auto currentSequence = renderer.PassTimingSequence();
			if (frame != 0 && currentSequence != sequence && !renderer.PassTimings().empty()) {
				for (const auto &[pass, microseconds] : renderer.PassTimings()) {
					(void)pass;
					gpuPassMicroseconds += microseconds;
				}
				++gpuSamples;
			}
			sequence = currentSequence;
			if (batch) {
				const auto &completed = batch->CompletedDeviceStatistics();
				if (frame != 0 && completed.Sequence != queueSequence && controllerSampleCount != 0 &&
					completed.Sequence >= firstControllerSequence &&
					completed.Sequence <= lastControllerSequence) {
					++queueCompletedFrames;
					queueDropped += completed.Dropped;
					for (uint32_t queue = 0; queue < ATOMIC_GPU_QUEUE_COUNT; ++queue) {
						if (completed.Samples[queue] == 0) continue;
						queueMicroseconds[queue] += completed.Microseconds[queue];
						queueSamples[queue] += completed.Samples[queue];
						queueFrameSamples[queue][queueFrameSampleCounts[queue]++] =
							completed.Microseconds[queue];
					}
				}
				queueSequence = completed.Sequence;
			}
			finishCompute(true);
			if (frame == 0) {
				wait();
				capture.Stop();
			}
		}
		wait();
		const auto finalSettlement = core::HeapProfile::Totals();
		const bool parallel = batch && batch->LastStatistics().Parallel;
		const char *mode = batch ? (parallel ? "packet_parallel" : "packet_serial") : "owner_ordinary";
		std::cout << "render-recording-profile";
		PrintCaptureIdentity(invocationId, api);
		std::cout << " backend=" << renderer.BackendName() << " device=\""
				  << SDL_GetStringProperty(properties, SDL_PROP_GPU_DEVICE_NAME_STRING, "unknown")
				  << "\" mode=" << mode << " views=" << Count << " worlds=1 meshes=" << ROWS
				  << " rows=" << ROWS << " extent=128x128 camera=moving warm_frames=" << WARM_FRAMES
				  << " frames=" << measuredFrames
				  << " profiling=full frame_graph=on waits_outside_cpu=1 recording_workers="
				  << (parallel ? 2 : 0) << " compute=" << (computeEnabled ? "rotate_add_f32x4" : "disabled")
				  << " compute_elements=" << (computeEnabled ? computeElements : 0)
				  << " compute_passes=" << (computeEnabled ? 2 : 0)
				  << " compute_request_input_bytes_per_frame=" << expectedCompute.size()
				  << " compute_verified_bytes_per_frame=" << expectedCompute.size()
				  << " compute_oracle=" << (computeEnabled ? "exact_full_buffer" : "disabled")
				  << " cpu_render_ns=" << cpuNanoseconds / measuredFrames
				  << " cpu_device_idle_wait_ns=" << cpuDeviceIdleWaitNanoseconds / measuredFrames
				  << " cpu_device_idle_wait_scope=before_measured_render"
				  << " cpu_device_idle_wait_basis=host_sdl_wait_for_gpu_idle_including_api_overhead"
				  << " packet_records_per_frame=" << static_cast<double>(recorded) / measuredFrames
				  << " worker_records_per_frame=" << static_cast<double>(workerRecorded) / measuredFrames
				  << " controller_submits_per_frame=" << static_cast<double>(submitted) / measuredFrames
				  << " dependency_edges_per_frame=" << static_cast<double>(edges) / measuredFrames
				  << " completed_batches=" << batchCompleted
				  << " cpu_producer_us=" << producerMicroseconds / measuredFrames
				  << " cpu_worker_join_us=" << joinMicroseconds / measuredFrames
				  << " cpu_submission_worker_wait_us=" << submissionWorkerWaitMicroseconds / measuredFrames
				  << " cpu_submission_worker_wait_calls=" << submissionWorkerWaits
				  << " cpu_submission_worker_wait_basis=owner_await_including_worker_submission_api_work"
				  << " cpu_worker_interval_overlap_us=" << cpuOverlapMicroseconds / measuredFrames
				  << " gpu_queue_unique_completed_frames=" << queueCompletedFrames
				  << " gpu_queue_dropped_queries=" << queueDropped
				  << " gpu_dependency_wait=unmeasured gpu_overlap=unmeasured";
		for (size_t recorder = 0; recorder < 3; ++recorder)
			std::cout << " cpu_recorder_" << recorder
					  << "_us=" << recorderMicroseconds[recorder] / measuredFrames << " cpu_recorder_"
					  << recorder
					  << "_tasks_per_frame=" << static_cast<double>(recorderTasks[recorder]) / measuredFrames;
		for (uint32_t queue = 0; queue < ATOMIC_GPU_QUEUE_COUNT; ++queue) {
			std::cout << " gpu_" << QUEUE_NAMES[queue] << "_query_samples=" << queueSamples[queue] << " gpu_"
					  << QUEUE_NAMES[queue] << "_sum_per_completed_frame_us=";
			if (queueSamples[queue] != 0 && queueCompletedFrames != 0)
				std::cout << queueMicroseconds[queue] / queueCompletedFrames;
			else
				std::cout << "unavailable";
			PrintTails(
				std::string("gpu_") + QUEUE_NAMES[queue] + "_frame_us",
				queueFrameSamples[queue],
				queueFrameSampleCounts[queue]
			);
			if (api)
				std::cout << " " << QUEUE_NAMES[queue]
						  << "_timestamp_bits=" << api->TimestampValidBits[queue];
		}
		if (api)
			std::cout << " native_device_name=\"" << api->Properties.deviceName
					  << "\" vendor_id=" << api->Properties.vendorID
					  << " device_id=" << api->Properties.deviceID;
		for (const auto &total : spans)
			std::cout << ' ' << total.Prefix << "_us=" << total.Inclusive * 1000 / measuredFrames << ' '
					  << total.Prefix << "_self_us=" << total.Self * 1000 / measuredFrames << ' '
					  << total.Prefix << "_spans=" << total.SpanCount;
		std::cout << " draw_calls_per_frame=" << drawCalls / measuredFrames
				  << " required_mesh_draw_runs_per_frame=" << ROWS * Count
				  << " instance_dirty_rows_per_frame=" << dirtyRows / measuredFrames
				  << " uploaded_bytes_per_frame=" << uploadedBytes / measuredFrames
				  << " cpu_profile_dropped_spans=" << droppedSpans << " gpu_dropped_marks=" << droppedMarks
				  << " gpu_unique_samples=" << gpuSamples << " gpu_pass_sum_mean_us=";
		if (gpuSamples != 0)
			std::cout << gpuPassMicroseconds / gpuSamples;
		else
			std::cout << "unavailable";
		std::cout << " gpu_timing_status="
				  << (gpuSamples == 0	  ? "unavailable"
					  : droppedMarks != 0 ? "partial"
										  : "available")
				  << " gpu_allocated_bytes=" << gpuAfter.AllocatedBytes - gpuBefore.AllocatedBytes
				  << " gpu_released_bytes=" << gpuAfter.ReleasedBytes - gpuBefore.ReleasedBytes
				  << " gpu_live_bytes=" << gpuAfter.LiveBytes << " gpu_peak_bytes=" << gpuAfter.PeakBytes
				  << " gpu_memory=logical_payload";
		PrintTails("cpu_render_ns", cpuRenderSamples, measuredFrames);
		PrintTails("cpu_device_idle_wait_ns", deviceIdleWaitSamples, measuredFrames);
		PrintTails("cpu_producer_ns", producerSamples, controllerSampleCount);
		PrintTails("cpu_worker_join_ns", joinSamples, controllerSampleCount);
		PrintTails("cpu_submission_worker_wait_us", submissionWorkerWaitSamples, controllerSampleCount);
		std::cout << '\n';
		std::cout << "render-recording-cpp-heap";
		PrintCaptureIdentity(invocationId, api);
		const bool heapHooks = core::HeapProfile::IsCompiledIn();
		std::cout << " mode=" << mode << " views=" << Count << " hooks=" << heapHooks
				  << " scope=process_tracked_cpp worker_allocator_coverage=process_global"
				  << " includes_benchmark_and_oracle=true driver_memory=unavailable vm_memory=unavailable"
				  << " peak_scope=process_lifetime_not_phase";
		if (heapHooks) {
			std::cout << " steady_allocated_bytes=" << measuredEnd.TotalBytes - presteady.TotalBytes
					  << " steady_allocated_blocks=" << measuredEnd.TotalBlocks - presteady.TotalBlocks;
			PrintHeapSnapshot("presteady", presteady);
			PrintHeapSnapshot("measured_end", measuredEnd);
			PrintHeapSnapshot("final_settlement", finalSettlement);
		} else {
			std::cout << " allocation_live_peak=unavailable";
		}
		std::cout << '\n';
		testing::Consume(cpuNanoseconds);
	}
}

BENCH("GPU recording whole harness | 1 moving view | 1024 resident meshes | use printed per-frame rows", 1) {
	RunRecordingBatch<1>();
}
BENCH("GPU recording whole harness | 2 moving views | 1024 resident meshes | use printed per-frame rows", 1) {
	RunRecordingBatch<2>();
}
BENCH("GPU recording whole harness | 8 moving views | 1024 resident meshes | use printed per-frame rows", 1) {
	RunRecordingBatch<8>();
}
