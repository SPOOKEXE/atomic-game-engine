// Real-device buffer graph baseline. Detailed rows separate installation,
// submission, fence-observed CPU latency and retirement. No GPU timing is inferred.

#include "ComputeGraph.hpp"

#include "BenchGpuPacing.hpp"

#include <engine/computegraph/Reference.hpp>
#include <engine/computegraph/Stock.hpp>
#include <engine/core/HeapProfile.hpp>
#include <engine/render/Renderer.hpp>
#include <engine/testing/Bench.hpp>

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

TEST_SUITE_ID("engine.render.bench.computegraph")

namespace {
	namespace cg = engine::computegraph;
	using Clock = std::chrono::steady_clock;
	using engine::render::ComputeGraph;
	constexpr uint32_t BATCHES = 128;
	constexpr uint32_t GROUP = 64;
	const cg::Owner OWNER{"computegraph-benchmark", 1, 1, UINT32_MAX};
	struct Latencies {
		// Fixed last-window storage keeps profiling independent of run duration.
		std::array<uint64_t, 1024> Values{};
		uint64_t Seen = 0;
		uint64_t Maximum = 0;
		void Add(uint64_t nanoseconds) {
			Values[Seen % Values.size()] = nanoseconds;
			++Seen;
			Maximum = std::max(Maximum, nanoseconds);
		}
		size_t Count() const {
			return static_cast<size_t>(std::min(Seen, uint64_t(Values.size())));
		}
	};
	struct Report {
		uint32_t Capacity = 0;
		uint32_t LogicalCount = 0;
		uint32_t InFlight = 0;
		bool Vector = false;
		uint64_t Samples = 0;
		uint64_t Jobs = 0;
		uint64_t InstallNanoseconds = 0;
		uint64_t SubmitNanoseconds = 0;
		uint64_t CompleteNanoseconds = 0;
		uint64_t DrainNanoseconds = 0;
		uint64_t Polls = 0;
		uint64_t DerivedDispatches = 0;
		uint64_t DerivedGroups = 0;
		uint64_t UploadBytes = 0;
		uint64_t DerivedDownloadBytes = 0;
		uint64_t ResultBytes = 0;
		uint64_t ReservedPeakBytes = 0;
		uint64_t LivePayloadPeakBytes = 0;
		uint64_t TransferPayloadPeakBytes = 0;
		uint64_t AllocatedBytes = 0;
		uint64_t BufferAllocations = 0;
		uint64_t TransferAllocations = 0;
		bool GpuTimingsEnabled = false;
		uint64_t UnavailableGpuTimings = 0;
		uint64_t RecordedDispatches = 0;
		uint64_t RecordedWorkgroups = 0;
		uint64_t RecordedUploadBytes = 0;
		uint64_t RecordedReadbackBytes = 0;
		engine::render::ComputeGraphStatistics GpuCounts{};
		Latencies Submit{};
		Latencies Poll{};
		Latencies Take{};
		Latencies ObservedComplete{};
		Latencies Retirement{};
		Latencies DeviceCommand{};
		engine::core::HeapTotals HeapBefore{};
		engine::core::HeapTotals HeapResident{};
		engine::core::HeapTotals HeapAfter{};
		engine::core::HeapTotals HeapRetired{};
		uint64_t CppAllocatedBytes = 0;
		uint64_t CppAllocatedBlocks = 0;
	};
	std::array<std::optional<Report>, 7> Reports;
	std::array<uint64_t, 7> CompletedInvocations{};
	uint64_t Elapsed(Clock::time_point start) {
		return static_cast<uint64_t>(
			std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start).count()
		);
	}
	void Require(bool accepted, std::string_view operation) {
		if (!accepted) throw std::runtime_error(std::string(operation));
	}
	void Accepted(cg::Status status, const cg::Diagnostic &diagnostic) {
		if (status != cg::Status::Ok)
			throw std::runtime_error(std::string(cg::Describe(status)) + ": " + diagnostic.Subject);
	}
	void PrintLatencies(std::string_view phase, const Latencies &latencies) {
		auto ordered = latencies.Values;
		const auto count = latencies.Count();
		if (count == 0) return;
		std::sort(ordered.begin(), ordered.begin() + count);
		const auto percentile = [&](size_t percent) { return ordered[(count * percent + 99) / 100 - 1]; };
		std::cout << " phase=" << phase << " lifetime_count=" << latencies.Seen
				  << " retained_last1024=" << count << " tail_evidence="
				  << (count < 100 ? "insufficient_samples_below_100" : "count_at_least100")
				  << " quantile_window=last1024 quantile_rule=nearest_rank p50_ns=" << percentile(50)
				  << " p95_ns=" << percentile(95) << " p99_ns=" << percentile(99)
				  << " all_observations_max_ns=" << latencies.Maximum << '\n';
	}
	void PrintHeap(std::string_view boundary, const engine::core::HeapTotals &heap) {
		std::cout << " boundary=" << boundary << " live_bytes=" << heap.LiveBytes
				  << " live_blocks=" << heap.LiveBlocks << " lifetime_peak_bytes=" << heap.PeakBytes
				  << " lifetime_allocated_bytes=" << heap.TotalBytes
				  << " lifetime_allocated_blocks=" << heap.TotalBlocks
				  << " profiler_overhead_bytes=" << heap.OverheadBytes
				  << " foreign_frees=" << heap.ForeignFrees << " dropped_scopes=" << heap.DroppedScopes
				  << '\n';
	}
	void PrintReports() {
		for (size_t index = 0; index < Reports.size(); ++index) {
			const auto &stored = Reports[index];
			if (!stored) continue;
			const auto &r = *stored;
			std::cout
				<< "computegraph-baseline type="
				<< (r.Vector ? "f32x4-two-pass" : "compact-u32-authored-double") << " capacity=" << r.Capacity
				<< " logical_count=" << r.LogicalCount << " in_flight=" << r.InFlight
				<< " cohort=latest_completed_body completed_body_invocations=" << CompletedInvocations[index]
				<< " previous_body_cohorts_included=false includes_previous_runner_warmups=false batches="
				<< BATCHES << " samples=" << r.Samples << " jobs=" << r.Jobs
				<< " install_ns_per_sample=" << r.InstallNanoseconds / r.Samples
				<< " submit_ns_per_job=" << r.SubmitNanoseconds / r.Jobs
				<< " observed_complete_ns_per_job=" << r.CompleteNanoseconds / r.Jobs
				<< " retirement_ns_per_sample=" << r.DrainNanoseconds / r.Samples << " polls=" << r.Polls
				<< " derived_dispatches=" << r.DerivedDispatches << " derived_workgroups=" << r.DerivedGroups
				<< " upload_bytes=" << r.UploadBytes << " derived_download_bytes=" << r.DerivedDownloadBytes
				<< " result_bytes=" << r.ResultBytes << " reserved_device_peak_bytes=" << r.ReservedPeakBytes
				<< " logical_live_payload_peak_delta_bytes=" << r.LivePayloadPeakBytes
				<< " logical_transfer_payload_peak_delta_bytes=" << r.TransferPayloadPeakBytes
				<< " allocated_payload_bytes=" << r.AllocatedBytes
				<< " buffer_allocations=" << r.BufferAllocations
				<< " transfer_allocations=" << r.TransferAllocations
				<< " polling=yield oracle=exact live_payload_after_delta_bytes=0"
				<< " gpu_timing_enabled=" << r.GpuTimingsEnabled
				<< " gpu_observations=" << r.DeviceCommand.Seen
				<< " gpu_unavailable_jobs=" << r.UnavailableGpuTimings
				<< " completion_clock=steady_host observation_includes_instrumentation=true"
				<< " quantiles_exclude_install_preflight=true\n";
			const auto prefix = [&](std::string_view kind) {
				std::cout << "computegraph-" << kind << " type=" << (r.Vector ? "f32x4" : "u32")
						  << " capacity=" << r.Capacity << " logical_count=" << r.LogicalCount
						  << " in_flight=" << r.InFlight;
			};
			const std::array<std::pair<std::string_view, const Latencies *>, 5> phases{
				{{"submit_cpu", &r.Submit},
				 {"poll_cpu", &r.Poll},
				 {"successful_take_cpu", &r.Take},
				 {"host_observed_completion", &r.ObservedComplete},
				 {"cancelled_retirement", &r.Retirement}}
			};
			for (const auto &[name, latencies] : phases) {
				prefix("latency");
				PrintLatencies(name, *latencies);
			}
			prefix("latency");
			if (r.DeviceCommand.Count())
				PrintLatencies("gpu_whole_upload_compute_download_command", r.DeviceCommand);
			else
				std::cout << " phase=gpu_whole_upload_compute_download_command duration=unavailable\n";
			prefix("device-work");
			std::cout << " available=" << r.GpuTimingsEnabled
					  << " steady_recorded_dispatches=" << r.RecordedDispatches
					  << " steady_recorded_workgroups=" << r.RecordedWorkgroups
					  << " steady_recorded_upload_bytes=" << r.RecordedUploadBytes
					  << " steady_recorded_readback_bytes=" << r.RecordedReadbackBytes
					  << " post_preflight_accepted_jobs=" << r.GpuCounts.AcceptedJobs
					  << " retired_jobs=" << r.GpuCounts.RetiredJobs
					  << " timed_jobs_including_canceled=" << r.GpuCounts.TimedJobs
					  << " dropped_timings=" << r.GpuCounts.DroppedTimings
					  << " batched_jobs=" << r.GpuCounts.BatchedJobs
					  << " canceled_retirements=" << r.GpuCounts.CanceledRetirements
					  << " duration_scope=whole_command kernel_only=false cross_queue_overlap=unavailable\n";
			prefix("cpp-heap");
			std::cout << " hooks=" << engine::core::HeapProfile::IsCompiledIn()
					  << " scope=process_tracked_cpp benchmark_allocations=included "
						 "driver_memory=unavailable vm_memory=unavailable\n";
			if (engine::core::HeapProfile::IsCompiledIn()) {
				prefix("cpp-heap");
				std::cout << " steady_allocated_bytes=" << r.CppAllocatedBytes
						  << " steady_allocated_blocks=" << r.CppAllocatedBlocks
						  << " snapshots=latest_sample\n";
				const std::array<std::pair<std::string_view, const engine::core::HeapTotals *>, 4> snapshots{
					{{"before_steady", &r.HeapBefore},
					 {"sampled_inflight_max_live", &r.HeapResident},
					 {"after_steady", &r.HeapAfter},
					 {"after_retirement", &r.HeapRetired}}
				};
				for (const auto &[name, heap] : snapshots) {
					prefix("cpp-heap");
					PrintHeap(name, *heap);
				}
			} else {
				prefix("cpp-heap");
				std::cout << " allocation_live_peak=unavailable reason=heap_hooks_not_compiled\n";
			}
		}
	}
	struct Host {
		engine::testing::BenchGpuPacing Budget;
		bool VideoReady = false;
		engine::render::Renderer Renderer;
		Host() {
			Budget.Begin();
			VideoReady = SDL_Init(SDL_INIT_VIDEO);
			Require(VideoReady, SDL_GetError());
			Require(Renderer.Initialise(nullptr), SDL_GetError());
			Require(Renderer.Capabilities().HasCompute, "compute capability unavailable");
			auto *device = static_cast<SDL_GPUDevice *>(Renderer.Backend().Device);
			const auto properties = SDL_GetGPUDeviceProperties(device);
			const char *preset = std::getenv("MONO_COMPUTEGRAPH_PRESET");
			std::cout << "computegraph-device preset=" << (preset ? preset : "unspecified")
					  << " backend=" << Renderer.BackendName() << " device=\""
					  << SDL_GetStringProperty(properties, SDL_PROP_GPU_DEVICE_NAME_STRING, "unavailable")
					  << "\" driver=\""
					  << SDL_GetStringProperty(
							 properties, SDL_PROP_GPU_DEVICE_DRIVER_VERSION_STRING, "unavailable"
						 )
					  << "\" clock_override=none clock_frequency=unmeasured validation=environment\n";
			SettleBudget();
		}
		void SettleBudget() {
			if (Budget.Percent)
				Require(
					SDL_WaitForGPUIdle(static_cast<SDL_GPUDevice *>(Renderer.Backend().Device)),
					SDL_GetError()
				);
			Budget.Complete();
			Budget.Wait();
		}
		~Host() {
			Renderer.Shutdown();
			if (VideoReady) SDL_QuitSubSystem(SDL_INIT_VIDEO);
		}
	};
	Host &GetHost() {
		static Host host;
		return host;
	}
	uint32_t Groups(uint32_t count) {
		return count / GROUP + uint32_t(count % GROUP != 0);
	}
	cg::Program Compact(uint32_t capacity) {
		cg::Program program;
		cg::Diagnostic diagnostic;
		Accepted(cg::StockStableCompactU32(capacity, program, diagnostic), diagnostic);
		program.Definition.Buffers.push_back(
			{.Name = "output", .Capacity = capacity, .CountBuffer = "count"}
		);
		program.Definition.Kernels.push_back(
			{.Name = "authored-double",
			 .Ports = {
				 {.Name = "Values"},
				 {.Name = "Count", .Binding = 1, .MinimumBytes = 4},
				 {.Name = "Result", .Permission = cg::Access::Write, .Set = 1}
			 }}
		);
		program.Kernels.push_back({.Name = "authored-double", .Source = R"GLSL(#version 450
layout(local_size_x = 64) in;
layout(set=2,binding=0,std140) uniform ComputeParameters { uvec4 Dispatch; uvec4 Buffers[32]; } config;
layout(set=0,binding=0,std430) readonly buffer Values { uint Data[]; } values;
layout(set=0,binding=1,std430) readonly buffer Count { uint Data[1]; } count;
layout(set=1,binding=0,std430) writeonly buffer Result { uint Data[]; } result;
void main() {
    uint i=gl_GlobalInvocationID.x;
    if(i<count.Data[0] && i<config.Buffers[0].y && i<config.Buffers[16].y)
        result.Data[i]=values.Data[i]*2u;
}
)GLSL"});
		program.Definition.Passes.push_back(
			{.Name = "consume",
			 .Kernel = "authored-double",
			 .DispatchBuffer = "packed",
			 .Bindings = {{"Values", "packed"}, {"Count", "count"}, {"Result", "output"}}}
		);
		return program;
	}
	cg::Program Vector(uint32_t capacity) {
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
	struct Data {
		cg::Request Request{.Identity = OWNER};
		std::vector<std::byte> Expected{};
		uint32_t Count = 0;
	};
	Data Input(uint32_t count, uint32_t capacity, bool vector) {
		Data data;
		cg::Upload upload{
			.Buffer = "input", .Type = vector ? cg::ElementType::F32x4 : cg::ElementType::U32, .Count = count
		};
		if (vector) {
			std::vector<float> values(size_t(count) * 4), expected(values.size());
			for (size_t i = 0; i < values.size(); ++i)
				values[i] = static_cast<float>(i % 128);
			for (uint32_t i = 0; i < count; ++i) {
				std::array<float, 4> row;
				std::copy_n(values.begin() + size_t(i) * 4, 4, row.begin());
				for (uint32_t step = 0; step < 32; ++step)
					row = {row[1] + 1, row[2] + 2, row[3] + 3, row[0] + 4};
				std::copy(row.begin(), row.end(), expected.begin() + size_t(i) * 4);
			}
			upload.Bytes.resize(values.size() * sizeof(float));
			data.Expected.resize(upload.Bytes.size());
			if (!values.empty()) {
				std::memcpy(upload.Bytes.data(), values.data(), upload.Bytes.size());
				std::memcpy(data.Expected.data(), expected.data(), data.Expected.size());
			}
			data.Count = count;
		} else {
			std::vector<uint32_t> values(count), expected;
			for (uint32_t i = 0; i < count; ++i)
				values[i] = i % 3 == 0 ? 0 : i % 100003 + 1;
			cg::Diagnostic diagnostic;
			Accepted(cg::CompactDoubleU32(values, capacity, capacity, expected, diagnostic), diagnostic);
			upload.Bytes.resize(values.size() * sizeof(uint32_t));
			data.Expected.resize(expected.size() * sizeof(uint32_t));
			if (!values.empty()) std::memcpy(upload.Bytes.data(), values.data(), upload.Bytes.size());
			if (!expected.empty()) std::memcpy(data.Expected.data(), expected.data(), data.Expected.size());
			data.Count = static_cast<uint32_t>(expected.size());
		}
		data.Request.Inputs.push_back(std::move(upload));
		data.Request.Outputs.push_back({"output", capacity});
		return data;
	}
	void Verify(const cg::Completion &completion, const Data &data) {
		Require(
			completion.Code == cg::Status::Ok && completion.Identity == OWNER &&
				completion.Outputs.size() == 1,
			"GPU completion failed"
		);
		const auto &output = completion.Outputs.front();
		Require(
			output.Buffer == "output" && output.Type == data.Request.Inputs.front().Type &&
				output.Count == data.Count && output.Bytes == data.Expected,
			"exact CPU oracle differs"
		);
	}
	void Batch(ComputeGraph &graph, const Data &data, uint32_t inFlight, Report *report) {
		std::vector<uint64_t> tickets(inFlight);
		std::vector<Clock::time_point> started(inFlight);
		std::vector<uint8_t> completed(inFlight, 0);
		std::vector<cg::Completion> completions(inFlight);
		std::vector<engine::render::ComputeGraphTiming> timings(inFlight);
		cg::Diagnostic diagnostic;
		GetHost().Budget.Begin();
		for (uint32_t i = 0; i < inFlight; ++i) {
			started[i] = Clock::now();
			Accepted(graph.Submit(data.Request, tickets[i], diagnostic), diagnostic);
			if (report) {
				const auto elapsed = Elapsed(started[i]);
				report->SubmitNanoseconds += elapsed;
				report->Submit.Add(elapsed);
			}
		}
		if (!report && inFlight == 3 && data.Request.Inputs.front().Count == 1048576) {
			uint64_t refused = 0;
			Require(
				graph.Submit(data.Request, refused, diagnostic) == cg::Status::LimitExceeded && refused == 0,
				"fourth large job did not meet bounded readback admission"
			);
		}
		if (report) {
			report->ReservedPeakBytes = std::max(report->ReservedPeakBytes, graph.ReservedDeviceBytes());
			const auto resident = GetHost().Renderer.MemoryStatistics();
			report->LivePayloadPeakBytes = std::max(report->LivePayloadPeakBytes, resident.LiveBytes);
			report->TransferPayloadPeakBytes =
				std::max(report->TransferPayloadPeakBytes, resident.TransferBufferBytes);
			const auto heap = engine::core::HeapProfile::Totals();
			if (report->Poll.Seen == 0 || heap.LiveBytes > report->HeapResident.LiveBytes)
				report->HeapResident = heap;
		}
		const auto deadline = Clock::now() + std::chrono::seconds(20);
		uint32_t remaining = inFlight;
		while (remaining && Clock::now() < deadline) {
			const auto pollStarted = Clock::now();
			graph.Poll();
			if (report) {
				report->Poll.Add(Elapsed(pollStarted));
				++report->Polls;
			}
			for (uint32_t i = 0; i < inFlight; ++i) {
				if (completed[i]) continue;
				const auto takeStarted = Clock::now();
				const auto status = graph.Take(tickets[i], completions[i], diagnostic, &timings[i]);
				const auto takeFinished = Clock::now();
				if (status == cg::Status::NotReady) continue;
				Accepted(status, diagnostic);
				if (report) {
					const auto observed = static_cast<uint64_t>(
						std::chrono::duration_cast<std::chrono::nanoseconds>(takeFinished - started[i])
							.count()
					);
					const auto take = static_cast<uint64_t>(
						std::chrono::duration_cast<std::chrono::nanoseconds>(takeFinished - takeStarted)
							.count()
					);
					report->CompleteNanoseconds += observed;
					report->ObservedComplete.Add(observed);
					report->Take.Add(take);
				}
				completed[i] = true;
				--remaining;
			}
			if (remaining) std::this_thread::yield();
		}
		Require(remaining == 0, "GPU completion deadline exceeded");
		// Every Take timestamp above precedes CPU oracle work and timing aggregation.
		for (uint32_t index = 0; index < inFlight; ++index) {
			Verify(completions[index], data);
			if (!report) continue;
			const auto &timing = timings[index];
			Require(timing.Ticket == tickets[index], "device timing ticket mismatch");
			if (timing.Available)
				report->DeviceCommand.Add(static_cast<uint64_t>(timing.CommandNanoseconds));
			else
				++report->UnavailableGpuTimings;
			report->RecordedDispatches += timing.Dispatches;
			report->RecordedWorkgroups += timing.Workgroups;
			report->RecordedUploadBytes += timing.UploadBytes;
			report->RecordedReadbackBytes += timing.ReadbackBytes;
		}
		GetHost().SettleBudget();
	}
	void Measure(
		size_t index,
		uint32_t capacity,
		uint32_t inFlight,
		bool vector,
		std::optional<uint32_t> logicalCount = {}
	) {
		static const bool reporting = [] {
			if (std::getenv("MONO_COMPUTEGRAPH_REPORT")) std::atexit(PrintReports);
			return true;
		}();
		(void)reporting;
		auto &host = GetHost();
		const auto budgetBefore = host.Budget.CompletedBursts;
		const auto program = vector ? Vector(capacity) : Compact(capacity);
		const auto measuredCount = logicalCount.value_or(capacity);
		const auto data = Input(measuredCount, capacity, vector);
		const auto before = host.Renderer.MemoryStatistics();
		host.Budget.Begin();
		ComputeGraph graph(host.Renderer.Backend().Device, OWNER);
		Require(
			graph.SetOutsideDeviceBytes(before.BufferBytes), "outside renderer payload reservation refused"
		);
		cg::Diagnostic diagnostic;
		Report sample;
		sample.Capacity = capacity;
		sample.LogicalCount = measuredCount;
		sample.InFlight = inFlight;
		sample.Vector = vector;
		sample.Samples = 1;
		sample.Jobs = BATCHES * inFlight;
		const auto install = Clock::now();
		Accepted(graph.Install(program, diagnostic), diagnostic);
		sample.InstallNanoseconds = Elapsed(install);
		host.SettleBudget();
		if (!vector) {
			for (const auto count : {0u, 1u, std::min(65u, capacity), capacity, 7u, 0u})
				Batch(graph, Input(count, capacity, false), 1, nullptr);
			if (inFlight == 3) Batch(graph, data, inFlight, nullptr);
			auto oversized = data.Request;
			oversized.Inputs.front().Count = capacity + 1;
			oversized.Inputs.front().Bytes.resize(size_t(capacity + 1) * 4);
			uint64_t ticket = 0;
			Require(
				graph.Submit(oversized, ticket, diagnostic) == cg::Status::InvalidCount && ticket == 0,
				"oversized submission was admitted"
			);
		}
		// Private Vulkan cleanup capability is optional. MONO_GPU_QUEUES=serial
		// enables it without turning these standalone jobs into C04 recording tasks.
		host.Budget.Begin();
		sample.GpuTimingsEnabled = graph.EnableGpuTimings(true);
		host.SettleBudget();
		const auto measuredBefore = host.Renderer.MemoryStatistics();
		sample.HeapBefore = engine::core::HeapProfile::Totals();
		for (uint32_t batch = 0; batch < BATCHES; ++batch) {
			Batch(graph, data, inFlight, &sample);
		}
		const auto measuredAfter = host.Renderer.MemoryStatistics();
		sample.HeapAfter = engine::core::HeapProfile::Totals();
		sample.CppAllocatedBytes = sample.HeapAfter.TotalBytes - sample.HeapBefore.TotalBytes;
		sample.CppAllocatedBlocks = sample.HeapAfter.TotalBlocks - sample.HeapBefore.TotalBlocks;
		sample.AllocatedBytes = measuredAfter.AllocatedBytes - measuredBefore.AllocatedBytes;
		sample.BufferAllocations = measuredAfter.BufferAllocations - measuredBefore.BufferAllocations;
		sample.TransferAllocations =
			measuredAfter.TransferBufferAllocations - measuredBefore.TransferBufferAllocations;
		sample.LivePayloadPeakBytes -= before.LiveBytes;
		sample.TransferPayloadPeakBytes -= before.TransferBufferBytes;
		// Retirement includes cancellation with a live fence, without host result publication.
		host.Budget.Begin();
		uint64_t retired = 0;
		Accepted(graph.Submit(data.Request, retired, diagnostic), diagnostic);
		Require(graph.Cancel(retired), "retirement cancellation failed");
		const auto drain = Clock::now();
		const auto deadline = drain + std::chrono::seconds(20);
		while (graph.PendingGpuCount() && Clock::now() < deadline) {
			graph.Poll();
			if (graph.PendingGpuCount()) std::this_thread::yield();
		}
		sample.DrainNanoseconds = Elapsed(drain);
		sample.Retirement.Add(sample.DrainNanoseconds);
		Require(graph.PendingGpuCount() == 0 && graph.ReservedDeviceBytes() == 0, "device retirement failed");
		sample.GpuCounts = graph.Statistics();
		if (sample.GpuTimingsEnabled) {
			Require(
				sample.GpuCounts.AcceptedJobs == sample.Jobs + 1 &&
					sample.GpuCounts.RetiredJobs == sample.GpuCounts.AcceptedJobs &&
					sample.GpuCounts.CanceledRetirements == 1 && sample.GpuCounts.BatchedJobs == 0 &&
					sample.GpuCounts.TimedJobs == sample.Jobs + 1 && sample.GpuCounts.DroppedTimings == 0 &&
					sample.DeviceCommand.Seen == sample.Jobs && sample.UnavailableGpuTimings == 0,
				"opt-in device timing retirement counters differ"
			);
		}
		graph.Shutdown();
		sample.HeapRetired = engine::core::HeapProfile::Totals();
		const auto after = host.Renderer.MemoryStatistics();
		Require(
			after.LiveBytes == before.LiveBytes && after.BufferBytes == before.BufferBytes &&
				after.TransferBufferBytes == before.TransferBufferBytes,
			"logical GPU payload failed to return to baseline"
		);
		host.SettleBudget();
		sample.UploadBytes = data.Request.Inputs.front().Bytes.size() * sample.Jobs;
		sample.ResultBytes = data.Expected.size() * sample.Jobs;
		uint64_t downloads = uint64_t(capacity) * (vector ? 16u : 4u);
		if (!vector) downloads += 4;
		for (const auto &name : program.ErrorBuffers) {
			const auto found = std::find_if(
				program.Definition.Buffers.begin(),
				program.Definition.Buffers.end(),
				[&](const auto &buffer) { return buffer.Name == name; }
			);
			Require(found != program.Definition.Buffers.end(), "unknown error buffer");
			downloads += uint64_t(found->LogicalCount) * found->Stride;
		}
		sample.DerivedDownloadBytes = downloads * sample.Jobs;
		for (const auto &pass : program.Definition.Passes) {
			const auto found = std::find_if(
				program.Definition.Buffers.begin(),
				program.Definition.Buffers.end(),
				[&](const auto &buffer) { return buffer.Name == pass.DispatchBuffer; }
			);
			Require(found != program.Definition.Buffers.end(), "unknown dispatch buffer");
			const auto elements = found->CountBuffer.empty() ? found->LogicalCount : found->Capacity;
			// Dispatch buffers define guarded work, even when the logical input is empty.
			const auto groups = Groups(elements);
			sample.DerivedGroups += groups * sample.Jobs;
			sample.DerivedDispatches += uint64_t(groups != 0) * sample.Jobs;
		}
		if (sample.GpuTimingsEnabled) {
			const auto initialized = downloads - uint64_t(capacity) * (vector ? 16u : 4u);
			Require(
				sample.RecordedDispatches == sample.DerivedDispatches &&
					sample.RecordedWorkgroups == sample.DerivedGroups &&
					sample.RecordedReadbackBytes == sample.DerivedDownloadBytes &&
					sample.RecordedUploadBytes == sample.UploadBytes + initialized * sample.Jobs,
				"recorded GPU boundary counts differ from guarded dispatch and initialization work"
			);
		}
		engine::testing::Consume(sample.CompleteNanoseconds);
		++CompletedInvocations[index];
		Reports[index] = std::move(sample);
		std::cout << "computegraph-gpu-budget counter_scope=device_lifetime_cumulative case=" << index
				  << " cohort_completed_bursts=" << host.Budget.CompletedBursts - budgetBefore;
		host.Budget.Print(std::cout);
		std::cout << '\n';
	}
}

BENCH("GPU ComputeGraph | compact authored consumer | 65 | serial", 1) {
	Measure(0, 65, 1, false);
}
BENCH("GPU ComputeGraph | compact authored consumer | 65537 | serial", 1) {
	Measure(1, 65537, 1, false);
}
BENCH("GPU ComputeGraph | compact authored consumer | 1048576 | serial", 1) {
	Measure(2, 1048576, 1, false);
}
BENCH("GPU ComputeGraph | compact authored consumer | 1048576 | three in flight saturation", 1) {
	Measure(3, 1048576, 3, false);
}
BENCH("GPU ComputeGraph | authored f32x4 reused twice | 65537 | serial", 1) {
	Measure(4, 65537, 1, true);
}
BENCH("GPU ComputeGraph | authored f32x4 reused twice | 65537 | four in flight", 1) {
	Measure(5, 65537, 4, true);
}
BENCH("GPU ComputeGraph | compact authored consumer | capacity 65 logical 0 | serial", 1) {
	Measure(6, 65, 1, false, 0);
}
