// C01: real VM marshalling, renderer pumping and binary result materialization.
// Detailed internal cohorts exclude their CPU oracle and source-file loading.
#include "ScriptComputeBridge.hpp"

#include "BenchGpuPacing.hpp"

#include <engine/core/HeapProfile.hpp>
#include <engine/core/Paths.hpp>
#include <engine/ecs/Attributes.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/render/Renderer.hpp>
#include <engine/scene/Part.hpp>
#include <engine/scene/Registration.hpp>
#include <engine/scene/Services.hpp>
#include <engine/scripthost/Runtime.hpp>
#include <engine/testing/Bench.hpp>

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

TEST_SUITE_ID("engine.render.bench.scriptcomputebridge")

namespace {
	using Clock = std::chrono::steady_clock;
	namespace script = engine::script;
	namespace render = engine::render;
	constexpr size_t MAX_TRIALS = 512;
	struct Distribution {
		std::array<uint64_t, MAX_TRIALS * 2> Values{};
		size_t Count = 0;
		void Add(uint64_t value) {
			if (Count == Values.size()) throw std::runtime_error("C01 sample storage exceeded");
			Values[Count++] = value;
		}
	};
	struct LoopWork {
		uint64_t PumpCpu = 0, HeartbeatCpu = 0, Pending = 0;
	};
	struct MemoryProbe {
		script::VmMemorySnapshot Vm;
		engine::core::HeapTotals Cpp;
		render::GpuMemoryStatistics Gpu;
	};
	struct Report {
		std::string Language, Cohort;
		uint32_t Count = 0, Requests = 0;
		size_t Trials = 0, Warmups = 0;
		bool MemoryMode = false;
		uint64_t Setups = 0, Definitions = 0, Jobs = 0, Polls = 0, ReadCalls = 0, ReadBytes = 0;
		uint64_t Completed = 0, Released = 0, Retired = 0, Cancelled = 0, Refused = 0;
		uint64_t GpuSamples = 0, GpuUnavailable = 0, GpuDrops = 0, CountsJobs = 0;
		uint64_t Dispatches = 0, Workgroups = 0, UploadBytes = 0, ReadbackBytes = 0;
		uint64_t PeakEntries = 0, PeakUpload = 0, PeakReadback = 0, PeakResult = 0, PeakDevice = 0;
		Distribution Whole, Bootstrap, Definition, SubmitMaterializeRelease, Cleanup;
		Distribution PumpCpu, HeartbeatCpu, HostPending, QueueWait, NativeReady, TicketRelease, DeviceCommand;
		std::array<MemoryProbe, 4> Memory;
		MemoryProbe PendingPeak;
	};
	std::vector<Report> Latest;
	uint64_t BodyInvocation = 0;
	uint64_t Elapsed(Clock::time_point start) {
		return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start).count();
	}
	void Require(bool accepted, std::string_view message) {
		if (!accepted) throw std::runtime_error(std::string(message));
	}
	bool Enabled(const char *name) {
		const char *value = std::getenv(name);
		return value && std::string_view(value) == "1";
	}
	size_t BoundedOption(const char *name, size_t fallback, size_t minimum, size_t maximum) {
		const char *value = std::getenv(name);
		if (!value) return fallback;
		size_t parsed = 0;
		const std::string_view text(value);
		const auto result = std::from_chars(text.data(), text.data() + text.size(), parsed);
		Require(
			result.ec == std::errc{} && result.ptr == text.data() + text.size() && parsed >= minimum &&
				parsed <= maximum,
			std::string("invalid bounded option ") + name
		);
		return parsed;
	}
	void PrintDistribution(const Report &report, std::string_view phase, const Distribution &values) {
		std::cout << "c01-latency vm=" << report.Language << " cohort=" << report.Cohort
				  << " elements=" << report.Count << " requests=" << report.Requests << " phase=" << phase;
		if (!values.Count) {
			std::cout << " duration=unavailable samples=0\n";
			return;
		}
		auto sorted = values.Values;
		std::sort(sorted.begin(), sorted.begin() + values.Count);
		const auto percentile = [&](size_t percent) {
			return sorted[(values.Count * percent + 99) / 100 - 1];
		};
		std::cout << " samples=" << values.Count
				  << " unit=" << (phase == "host_pending_iterations" ? "iterations" : "ns")
				  << " rule=nearest_rank p50=" << percentile(50) << " p95=" << percentile(95)
				  << " p99=" << percentile(99) << " max=" << sorted[values.Count - 1]
				  << " tail_evidence=" << (values.Count >= 100 ? "at_least100" : "insufficient_below100")
				  << '\n';
	}
	void PrintProbe(const Report &report, std::string_view boundary, const MemoryProbe &probe) {
		const auto optional = [](const std::optional<uint64_t> &value) {
			return value ? std::to_string(*value) : "unavailable";
		};
		const char *basis = probe.Vm.Basis == script::VmMemoryBasis::RequestedBytes ? "luau_requested_bytes"
							: probe.Vm.Basis == script::VmMemoryBasis::QuickJsChargedBytes
								? "quickjs_charged_bytes"
								: "unavailable";
		std::cout << "c01-memory vm=" << report.Language << " cohort=" << report.Cohort
				  << " elements=" << report.Count << " boundary=" << boundary << " vm_basis=" << basis
				  << " snapshot_scope=latest_setup_or_trial timing_valid=false"
				  << " vm_live_bytes="
				  << (probe.Vm.Basis == script::VmMemoryBasis::Unavailable
						  ? "unavailable"
						  : std::to_string(probe.Vm.LiveBytes))
				  << " vm_live_blocks="
				  << (probe.Vm.Basis == script::VmMemoryBasis::Unavailable
						  ? "unavailable"
						  : std::to_string(probe.Vm.LiveBlocks))
				  << " vm_window_peak_bytes=" << optional(probe.Vm.PeakLiveBytes)
				  << " vm_lifetime_growth_bytes=" << optional(probe.Vm.GrowthBytes)
				  << " vm_lifetime_growth_ops=" << optional(probe.Vm.GrowthOps)
				  << " cpp_hooks=" << engine::core::HeapProfile::IsCompiledIn()
				  << " cpp_process_live_bytes=" << probe.Cpp.LiveBytes
				  << " cpp_process_live_blocks=" << probe.Cpp.LiveBlocks
				  << " cpp_process_lifetime_peak_bytes=" << probe.Cpp.PeakBytes
				  << " cpp_process_total_allocated_bytes=" << probe.Cpp.TotalBytes
				  << " cpp_process_total_allocated_blocks=" << probe.Cpp.TotalBlocks
				  << " cpp_profiler_overhead_bytes=" << probe.Cpp.OverheadBytes
				  << " gpu_logical_live_bytes=" << probe.Gpu.LiveBytes
				  << " gpu_device_lifetime_logical_peak_bytes=" << probe.Gpu.PeakBytes
				  << " gpu_total_allocated_bytes=" << probe.Gpu.AllocatedBytes
				  << " gpu_total_released_bytes=" << probe.Gpu.ReleasedBytes
				  << " gpu_buffer_allocations=" << probe.Gpu.BufferAllocations
				  << " gpu_transfer_buffer_allocations=" << probe.Gpu.TransferBufferAllocations
				  << " gpu_texture_allocations=" << probe.Gpu.TextureAllocations
				  << " gpu_release_operations=unavailable"
				  << " driver_commitment=unavailable rss=unavailable\n";
	}
	void PrintMemoryDelta(const Report &report) {
		const auto &before = report.Memory[1];
		const auto &after = report.Memory[2];
		const auto cppDelta = [](uint64_t value) {
			return engine::core::HeapProfile::IsCompiledIn() ? std::to_string(value) : "unavailable";
		};
		std::cout
			<< "c01-memory-delta vm=" << report.Language << " cohort=" << report.Cohort
			<< " elements=" << report.Count
			<< " phase=graph_defined_to_materialized_released snapshot_scope=latest_trial timing_valid=false"
			<< " cpp_allocated_bytes=" << cppDelta(after.Cpp.TotalBytes - before.Cpp.TotalBytes)
			<< " cpp_allocated_blocks=" << cppDelta(after.Cpp.TotalBlocks - before.Cpp.TotalBlocks)
			<< " gpu_allocated_bytes=" << after.Gpu.AllocatedBytes - before.Gpu.AllocatedBytes
			<< " gpu_released_bytes=" << after.Gpu.ReleasedBytes - before.Gpu.ReleasedBytes
			<< " gpu_buffer_allocations=" << after.Gpu.BufferAllocations - before.Gpu.BufferAllocations
			<< " gpu_transfer_buffer_allocations="
			<< after.Gpu.TransferBufferAllocations - before.Gpu.TransferBufferAllocations
			<< " gpu_texture_allocations=" << after.Gpu.TextureAllocations - before.Gpu.TextureAllocations
			<< '\n';
	}
	void PrintLatest() {
		for (const auto &report : Latest) {
			std::cout
				<< "c01-cohort body=" << BodyInvocation << " vm=" << report.Language
				<< " cohort=" << report.Cohort << " elements=" << report.Count
				<< " requests=" << report.Requests << " internal_trials=" << report.Trials
				<< " setups=" << report.Setups << " definitions=" << report.Definitions
				<< " excluded_warmups=" << report.Warmups << " measured_jobs=" << report.Jobs
				<< " completed=" << report.Completed << " released=" << report.Released
				<< " retired=" << report.Retired << " canceled=" << report.Cancelled
				<< " refused=" << report.Refused
				<< " oracle=every_result_word_outside_timing source_loading=excluded"
				<< " device=retained driver_cache=unspecified polling=owner_yield "
				   "driver=ecs_attributes_heartbeat"
				<< " memory_diagnostic=" << report.MemoryMode << " profiling=opt_in_whole_command"
				<< " bridge_polls=" << report.Polls << " read_calls=" << report.ReadCalls
				<< " materialized_bytes=" << report.ReadBytes
				<< " native_exact_ticket_gpu_samples=" << report.GpuSamples
				<< " gpu_unavailable_jobs=" << report.GpuUnavailable
				<< " gpu_dropped_queries=" << report.GpuDrops
				<< " actual_device_count_jobs=" << report.CountsJobs << " actual_dispatches="
				<< (report.CountsJobs == report.Jobs ? std::to_string(report.Dispatches) : "unavailable")
				<< " actual_workgroups="
				<< (report.CountsJobs == report.Jobs ? std::to_string(report.Workgroups) : "unavailable")
				<< " actual_upload_bytes="
				<< (report.CountsJobs == report.Jobs ? std::to_string(report.UploadBytes) : "unavailable")
				<< " actual_readback_bytes="
				<< (report.CountsJobs == report.Jobs ? std::to_string(report.ReadbackBytes) : "unavailable")
				<< " reservation_peak_entries=" << report.PeakEntries
				<< " reservation_peak_upload_bytes=" << report.PeakUpload
				<< " reservation_peak_readback_bytes=" << report.PeakReadback
				<< " reservation_peak_result_bytes=" << report.PeakResult
				<< " reservation_peak_device_bytes=" << report.PeakDevice
				<< " reservation_scope=runtime_lifetime_including_warmups "
				   "current_reservations_after_cleanup=0"
				<< " outer_body_includes_setup_oracle_cleanup=true\n";
			if (report.MemoryMode) {
				const std::array<std::string_view, 4> boundaries{
					"runtime_ready", "graph_defined", "materialized_released", "after_runtime_destroy"
				};
				for (size_t index = 0; index < boundaries.size(); ++index)
					PrintProbe(report, boundaries[index], report.Memory[index]);
				PrintProbe(report, "sampled_pump_max_live", report.PendingPeak);
				PrintMemoryDelta(report);
				continue;
			}
			const std::array<std::pair<std::string_view, const Distribution *>, 12> phases{
				{{"whole_until_materialized_release", &report.Whole},
				 {"runtime_store_bootstrap", &report.Bootstrap},
				 {"definition_until_ready_release", &report.Definition},
				 {"submit_materialize_release", &report.SubmitMaterializeRelease},
				 {"runtime_detach_retirement_cleanup", &report.Cleanup},
				 {"owner_pump_cpu", &report.PumpCpu},
				 {"heartbeat_cpu", &report.HeartbeatCpu},
				 {"host_pending_iterations", &report.HostPending},
				 {"ticket_queue_until_native_submit", &report.QueueWait},
				 {"ticket_submit_to_native_ready", &report.NativeReady},
				 {"ticket_submit_to_vm_release", &report.TicketRelease},
				 {"gpu_whole_upload_compute_download_command", &report.DeviceCommand}}
			};
			for (const auto &[name, values] : phases)
				PrintDistribution(report, name, *values);
		}
	}

	std::string Source(script::Language language, uint32_t capacity) {
		const bool luau = language == script::Language::Luau;
		const auto path = engine::core::Paths::Base().parent_path() / "assets/examples/scripts" /
						  (luau ? "BufferCompute.luau" : "BufferCompute.js");
		std::ifstream input(path);
		Require(bool(input), "C01 shipped BufferCompute source missing");
		std::string source((std::istreambuf_iterator<char>(input)), {});
		const auto replace = [&](std::string_view before, const std::string &after) {
			const auto position = source.find(before);
			Require(position != std::string::npos, "C01 shipped example prefix differs");
			source.replace(position, before.size(), after);
		};
		const auto marker =
			source.find(luau ? "local ticket = compute:DefineGraph" : "let ticket = compute.DefineGraph");
		Require(marker != std::string::npos, "C01 shipped guarded consumer missing");
		source.resize(marker);
		replace(
			luau ? "local capacity = 4097" : "const capacity = 4097;",
			(luau ? "local capacity = " : "const capacity = ") + std::to_string(capacity) + (luau ? "" : ";")
		);
		const auto groups = std::to_string(std::max(1u, capacity / 64 + uint32_t(capacity % 64 != 0)));
		replace(
			luau ? "Capacity = 65, Count = 65" : "Capacity:65,Count:65",
			luau ? "Capacity = " + groups + ", Count = " + groups : "Capacity:" + groups + ",Count:" + groups
		);
		if (luau)
			source += R"lua(
local marker=Instance.new('Folder'); marker.Name='C01'; marker.Parent=workspace
local definition=compute:DefineGraph('c01',plan)
local defined=false
local lastCommand=0
local tickets,results={},{ }
local seedValue,inputCount,requestCount=0,0,0
function C01Submit(seed,count,requests)
    seedValue,inputCount,requestCount=seed,count,requests
    tickets,results={},{}
    marker:SetAttribute('Done',false); marker:SetAttribute('Verified',false)
    for request=1,requests do
        local bytes=buffer.create(count*4)
        for index=0,count-1 do
            local value=if index%3==0 then 0 else 1+((index+seed*17+request*13)%1000000)
            buffer.writeu32(bytes,index*4,value)
        end
        tickets[request]=compute:SubmitGraph('c01',{Inputs={{Name='input',Type='u32',Count=count}},Outputs={{Name='doubled',MaximumElements=capacity}}},bytes)
    end
end
function C01Verify()
    assert(#results==requestCount)
    for request=1,requestCount do
        local result=assert(results[request])
        local expectedCount=inputCount-math.floor((inputCount+2)/3)
        assert(result.Count==expectedCount and result.Name=='doubled' and result.Type=='u32')
        assert(buffer.len(result.Bytes)==expectedCount*4)
        local outputIndex=0
        for index=0,inputCount-1 do
            if index%3~=0 then
                local expected=(1+((index+seedValue*17+request*13)%1000000))*2
                assert(buffer.readu32(result.Bytes,outputIndex*4)==expected,'C01 result word differs')
                outputIndex+=1
            end
        end
        assert(outputIndex==expectedCount)
    end
    results,tickets={},{}
    marker:SetAttribute('Verified',true)
end
game:GetService('RunService').Heartbeat:Connect(function()
    if not defined then
        local reply=compute:PollGraph(definition)
        if reply.Status=='pending' then return end
        assert(reply.Status=='complete',reply.Detail)
        assert(compute:ReleaseGraph(definition))
        defined=true; marker:SetAttribute('Defined',true)
        return
    end
    if marker:GetAttribute('Verify')==true then
        marker:SetAttribute('Verify',false); C01Verify(); return
    end
    local command=marker:GetAttribute('Command')
    if typeof(command)=='number' and command~=lastCommand then
        lastCommand=command
        C01Submit(marker:GetAttribute('Seed'),marker:GetAttribute('Count'),marker:GetAttribute('Requests'))
    end
    if #tickets==0 then return end
    local remaining=0
    for request,ticket in tickets do
        if ticket~=false then
            local reply=compute:PollGraph(ticket)
            if reply.Status=='pending' then remaining+=1
            else
                assert(reply.Status=='complete',reply.Detail)
                local output=reply.Outputs[1]
                local bytes=buffer.create(output.Bytes)
                local offset=0
                while offset<output.Bytes do
                    local chunk=compute:ReadGraphResult(ticket,'doubled',offset,math.min(65536,output.Bytes-offset))
                    buffer.copy(bytes,offset,chunk)
                    offset+=buffer.len(chunk)
                end
                results[request]={Bytes=bytes,Count=output.Count,Name=output.Name,Type=output.Type}
                assert(compute:ReleaseGraph(ticket)); tickets[request]=false
            end
        end
    end
    if remaining==0 then marker:SetAttribute('Done',true) end
end)
)lua";
		else
			source += R"js(
const marker=Instance.new('Folder'); marker.Name='C01'; marker.Parent=workspace;
const definition=compute.DefineGraph('c01',plan);
let defined=false,lastCommand=0,tickets=[],results=[],seedValue=0,inputCount=0,requestCount=0;
function check(ok,detail) { if (!ok) throw new Error(detail || 'C01 result differs'); }
function C01Submit(seed,count,requests) {
    seedValue=seed; inputCount=count; requestCount=requests; tickets=[]; results=[];
    marker.SetAttribute('Done',false); marker.SetAttribute('Verified',false);
    for (let request=1;request<=requests;++request) {
        const bytes=new ArrayBuffer(count*4), view=new DataView(bytes);
        for (let index=0;index<count;++index) {
            const value=index%3===0 ? 0 : 1+((index+seed*17+request*13)%1000000);
            view.setUint32(index*4,value,true);
        }
        tickets.push(compute.SubmitGraph('c01',{Inputs:[{Name:'input',Type:'u32',Count:count}],Outputs:[{Name:'doubled',MaximumElements:capacity}]},bytes));
    }
}
function C01Verify() {
    check(results.length===requestCount);
    for (let requestIndex=0;requestIndex<requestCount;++requestIndex) {
        const result=results[requestIndex]; check(result!==undefined);
        const expectedCount=inputCount-Math.floor((inputCount+2)/3);
        check(result.Count===expectedCount && result.Name==='doubled' && result.Type==='u32');
        check(result.Bytes.byteLength===expectedCount*4);
        const view=new DataView(result.Bytes.buffer); let outputIndex=0;
        for (let index=0;index<inputCount;++index) if (index%3!==0) {
            const expected=(1+((index+seedValue*17+(requestIndex+1)*13)%1000000))*2;
            check(view.getUint32(outputIndex*4,true)===expected,'C01 result word differs'); ++outputIndex;
        }
        check(outputIndex===expectedCount);
    }
    results=[]; tickets=[]; marker.SetAttribute('Verified',true);
}
game.GetService('RunService').Heartbeat.Connect(()=>{
    if (!defined) {
        const reply=compute.PollGraph(definition);
        if (reply.Status==='pending') return;
        check(reply.Status==='complete',reply.Detail); check(compute.ReleaseGraph(definition));
        defined=true; marker.SetAttribute('Defined',true); return;
    }
    if (marker.GetAttribute('Verify')===true) {
        marker.SetAttribute('Verify',false); C01Verify(); return;
    }
    const command=marker.GetAttribute('Command');
    if (typeof command==='number' && command!==lastCommand) {
        lastCommand=command;
        C01Submit(marker.GetAttribute('Seed'),marker.GetAttribute('Count'),marker.GetAttribute('Requests'));
    }
    if (!tickets.length) return;
    let remaining=0;
    tickets.forEach((ticket,index)=>{
        if (ticket===null) return;
        const reply=compute.PollGraph(ticket);
        if (reply.Status==='pending') { ++remaining; return; }
        check(reply.Status==='complete',reply.Detail);
        const output=reply.Outputs[0], bytes=new Uint8Array(output.Bytes);
        for (let offset=0;offset<output.Bytes;) {
            const chunk=compute.ReadGraphResult(ticket,'doubled',offset,Math.min(65536,output.Bytes-offset));
            bytes.set(new Uint8Array(chunk),offset); offset+=chunk.byteLength;
        }
        results[index]={Bytes:bytes,Count:output.Count,Name:output.Name,Type:output.Type};
        check(compute.ReleaseGraph(ticket)); tickets[index]=null;
    });
    if (!remaining) marker.SetAttribute('Done',true);
});
)js";
		return source;
	}

	struct Host {
		engine::testing::BenchGpuPacing Budget;
		render::Renderer Renderer;
		Host() {
			Budget.Begin();
			Require(SDL_Init(SDL_INIT_VIDEO), SDL_GetError());
			Require(Renderer.Initialise(nullptr), SDL_GetError());
			Require(Renderer.Capabilities().HasCompute, "C01 device has no compute support");
			engine::scene::EnsureClassTree();
			engine::scene::RegisterSceneComponents();
			const auto properties =
				SDL_GetGPUDeviceProperties(static_cast<SDL_GPUDevice *>(Renderer.Backend().Device));
			const char *preset = std::getenv("MONO_C01_PRESET");
			std::cout << "c01-device preset=" << (preset ? preset : "unspecified")
					  << " backend=" << Renderer.BackendName() << " device=\""
					  << SDL_GetStringProperty(properties, SDL_PROP_GPU_DEVICE_NAME_STRING, "unavailable")
					  << "\" driver=\""
					  << SDL_GetStringProperty(
							 properties, SDL_PROP_GPU_DEVICE_DRIVER_VERSION_STRING, "unavailable"
						 )
					  << "\" device_creation=excluded source_loading=excluded clock_control=none\n";
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
			SDL_QuitSubSystem(SDL_INIT_VIDEO);
		}
	};
	struct Vm {
		std::unique_ptr<engine::ecs::Store> Store;
		std::shared_ptr<script::Runtime> Runtime;
		std::shared_ptr<script::ComputeBridge> Endpoint;
		uint64_t CommandSequence = 0;
	};
	bool Attribute(Vm &vm, const char *name) {
		const auto workspace = engine::scene::WorkspaceOf(*vm.Store);
		const auto marker = vm.Store->FindFirstChild(workspace, "C01");
		engine::ecs::AttributeValue value;
		return marker != engine::ecs::NULL_ENTITY &&
			   engine::ecs::GetAttribute(*vm.Store, marker, engine::core::Name(name), value) &&
			   value.Type == engine::ecs::PropertyType::Bool && value.Bool;
	}
	void Command(Vm &vm, const char *name, double number) {
		const auto workspace = engine::scene::WorkspaceOf(*vm.Store);
		const auto marker = vm.Store->FindFirstChild(workspace, "C01");
		engine::ecs::AttributeValue value;
		value.Type = engine::ecs::PropertyType::Double;
		value.Double = number;
		Require(
			engine::ecs::SetAttribute(*vm.Store, marker, engine::core::Name(name), value),
			"C01 command attribute refused"
		);
	}
	void Flag(Vm &vm, const char *name, bool enabled) {
		const auto workspace = engine::scene::WorkspaceOf(*vm.Store);
		const auto marker = vm.Store->FindFirstChild(workspace, "C01");
		engine::ecs::AttributeValue value;
		value.Type = engine::ecs::PropertyType::Bool;
		value.Bool = enabled;
		Require(
			engine::ecs::SetAttribute(*vm.Store, marker, engine::core::Name(name), value),
			"C01 command flag refused"
		);
	}
	MemoryProbe Probe(Host &host, const Vm &vm) {
		return {
			vm.Runtime ? vm.Runtime->MemorySnapshot() : script::VmMemorySnapshot{},
			engine::core::HeapProfile::Totals(),
			host.Renderer.MemoryStatistics()
		};
	}
	void Pump(Host &host, LoopWork &work) {
		const auto start = Clock::now();
		host.Renderer.PumpCompute();
		work.PumpCpu += Elapsed(start);
	}
	void Await(Host &host, Vm &vm, const char *attribute, LoopWork &work, Report *memory) {
		const auto deadline = Clock::now() + std::chrono::seconds(30);
		while (!Attribute(vm, attribute)) {
			Require(Clock::now() < deadline, "C01 owner pump deadline exceeded");
			++work.Pending;
			Pump(host, work);
			vm.Store->ClearChanges();
			vm.Store->AdvanceTick(1.0 / 60.0);
			const auto heartbeat = Clock::now();
			Require(vm.Runtime->Heartbeat(1.0f / 60.0f), vm.Runtime->LastError());
			work.HeartbeatCpu += Elapsed(heartbeat);
			vm.Store->FlushSignals();
			if (memory) {
				const auto observed = Probe(host, vm);
				if (observed.Vm.LiveBytes >= memory->PendingPeak.Vm.LiveBytes) memory->PendingPeak = observed;
			}
			std::this_thread::yield();
		}
		// VM Release revokes publication; this pump also removes the charged inbox entry.
		Pump(host, work);
	}
	uint64_t Bootstrap(Host &host, Vm &vm, script::Language language, bool memory) {
		const auto start = Clock::now();
		vm.CommandSequence = 0;
		vm.Store = std::make_unique<engine::ecs::Store>("c01-world");
		script::RuntimeLimits limits;
		limits.Role = script::HostRole::OfBoth();
		auto factory = host.Renderer.ComputeForWorld(std::string(vm.Store->Name()));
		limits.Compute = [&vm, factory](uint64_t runtime, uint64_t generation) {
			vm.Endpoint = factory(runtime, generation);
			Require(bool(vm.Endpoint), "C01 native endpoint unavailable");
			Require(
				render::ScriptComputeBridge::EnableProfiling(*vm.Endpoint),
				"C01 native profiling enable refused"
			);
			return vm.Endpoint;
		};
		vm.Runtime = script::MakeRuntime(*vm.Store, language, limits);
		Require(bool(vm.Runtime), "C01 runtime unavailable");
		const auto elapsed = Elapsed(start);
		if (memory) vm.Runtime->BeginMemoryWindow();
		return elapsed;
	}
	void Collect(
		Report &report,
		const render::ScriptComputeProfile &before,
		const render::ScriptComputeProfile &after,
		const Vm &vm
	) {
		Require(after.Enabled && after.DroppedRecords == 0, "C01 required profiling records missing");
		Require(after.Jobs.size() == report.Requests, "C01 native ticket coverage incomplete");
		Require(
			after.Refused == before.Refused && after.Cancelled == before.Cancelled,
			"C01 request refused or canceled"
		);
		Require(
			after.CurrentEntries == 0 && after.CurrentDeviceBytes == 0 && after.CurrentResultBytes == 0 &&
				after.CurrentUploadBytes == 0 && after.CurrentReadbackBytes == 0,
			"C01 request reservations not released"
		);
		report.Jobs += after.Submitted - before.Submitted;
		report.Completed += after.Completed - before.Completed;
		report.Released += after.Released - before.Released;
		report.Retired += after.RetiredJobs - before.RetiredJobs;
		report.Cancelled += after.Cancelled - before.Cancelled;
		report.Refused += after.Refused - before.Refused;
		report.Polls += after.PollCalls - before.PollCalls;
		report.ReadCalls += after.ReadCalls - before.ReadCalls;
		report.ReadBytes += after.ReadBytes - before.ReadBytes;
		report.PeakEntries = std::max(report.PeakEntries, after.PeakEntries);
		report.PeakUpload = std::max(report.PeakUpload, after.PeakUploadBytes);
		report.PeakReadback = std::max(report.PeakReadback, after.PeakReadbackBytes);
		report.PeakResult = std::max(report.PeakResult, after.PeakResultBytes);
		report.PeakDevice = std::max(report.PeakDevice, after.PeakDeviceBytes);
		uint64_t previousTicket = 0;
		for (const auto &job : after.Jobs) {
			Require(
				job.Owner == vm.Endpoint->Identity() && job.Ticket > previousTicket && job.Released &&
					job.Retired && !job.Cancelled,
				"C01 exact native ticket attribution differs"
			);
			previousTicket = job.Ticket;
			report.QueueWait.Add(job.QueueNanoseconds);
			report.NativeReady.Add(job.SubmitToReadyNanoseconds);
			report.TicketRelease.Add(job.SubmitToReleaseNanoseconds);
			if (job.DeviceCountsAvailable) {
				Require(job.Device.Ticket != 0, "C01 native device ticket missing");
				++report.CountsJobs;
				report.Dispatches += job.Device.Dispatches;
				report.Workgroups += job.Device.Workgroups;
				report.UploadBytes += job.Device.UploadBytes;
				report.ReadbackBytes += job.Device.ReadbackBytes;
				if (job.Device.Available) {
					++report.GpuSamples;
					report.DeviceCommand.Add(static_cast<uint64_t>(job.Device.CommandNanoseconds));
				} else
					++report.GpuDrops;
			} else
				++report.GpuUnavailable;
		}
	}
	uint64_t Cleanup(Host &host, Vm &vm, uint64_t baselineBytes) {
		const auto start = Clock::now();
		vm.Runtime.reset();
		const auto deadline = Clock::now() + std::chrono::seconds(30);
		for (;;) {
			host.Renderer.PumpCompute();
			const auto profile = render::ScriptComputeBridge::TakeProfile(*vm.Endpoint);
			if (!profile.CurrentEntries && !profile.CurrentDeviceBytes) break;
			Require(Clock::now() < deadline, "C01 retirement deadline exceeded");
			std::this_thread::yield();
		}
		vm.Store.reset();
		Require(
			host.Renderer.MemoryStatistics().LiveBytes == baselineBytes,
			"C01 logical GPU payload failed to return to baseline"
		);
		return Elapsed(start);
	}
	Report Measure(
		Host &host,
		script::Language language,
		uint32_t count,
		uint32_t requests,
		bool cold,
		size_t trials,
		size_t warmups,
		bool memory,
		const std::string &source
	) {
		const auto budgetBefore = host.Budget.CompletedBursts;
		Report report;
		report.Language = language == script::Language::Luau ? "luau" : "javascript";
		report.Cohort = cold ? "fresh_store_runtime_graph" : "retained_runtime_graph";
		report.Count = count;
		report.Requests = requests;
		report.Trials = trials;
		report.Warmups = cold ? 0 : warmups;
		report.MemoryMode = memory;
		Vm vm;
		render::ScriptComputeProfile previous;
		const auto baseline = host.Renderer.MemoryStatistics().LiveBytes;
		const auto define = [&](LoopWork &work) {
			const auto start = Clock::now();
			Require(vm.Runtime->Run(source, "C01.BufferCompute"), vm.Runtime->LastError());
			Await(host, vm, "Defined", work, memory ? &report : nullptr);
			++report.Definitions;
			return Elapsed(start);
		};
		const auto run = [&](uint32_t seed, LoopWork &work) {
			const auto start = Clock::now();
			// The shipped VM chunk retains its own closure. Use ordinary world data
			// for commands; Luau intentionally isolates globals between Run calls.
			Flag(vm, "Done", false);
			Flag(vm, "Verified", false);
			Command(vm, "Seed", seed);
			Command(vm, "Count", count);
			Command(vm, "Requests", requests);
			Command(vm, "Command", ++vm.CommandSequence);
			Await(host, vm, "Done", work, memory ? &report : nullptr);
			return Elapsed(start);
		};
		const auto verify = [&] {
			Flag(vm, "Verify", true);
			LoopWork oracle;
			Await(host, vm, "Verified", oracle, nullptr);
		};
		if (!cold) {
			host.Budget.Begin();
			report.Bootstrap.Add(Bootstrap(host, vm, language, memory));
			++report.Setups;
			if (memory) report.Memory[0] = Probe(host, vm);
			LoopWork excluded;
			report.Definition.Add(define(excluded));
			host.SettleBudget();
			for (size_t warm = 0; warm < warmups; ++warm) {
				host.Budget.Begin();
				run(static_cast<uint32_t>(10000 + warm), excluded);
				verify();
				(void)render::ScriptComputeBridge::TakeProfile(*vm.Endpoint);
				host.SettleBudget();
			}
			previous = render::ScriptComputeBridge::TakeProfile(*vm.Endpoint);
		}
		for (size_t trial = 0; trial < trials; ++trial) {
			host.Budget.Begin();
			const auto whole = Clock::now();
			LoopWork work;
			if (cold) {
				report.Bootstrap.Add(Bootstrap(host, vm, language, memory));
				++report.Setups;
				if (memory) report.Memory[0] = Probe(host, vm);
				report.Definition.Add(define(work));
				previous = render::ScriptComputeBridge::TakeProfile(*vm.Endpoint);
			}
			if (memory) {
				report.PendingPeak = {};
				vm.Runtime->BeginMemoryWindow();
				report.Memory[1] = Probe(host, vm);
			}
			const auto submitted = run(static_cast<uint32_t>(1 + trial * 101), work);
			const auto wholeElapsed = Elapsed(whole);
			if (memory) report.Memory[2] = Probe(host, vm);
			const auto after = render::ScriptComputeBridge::TakeProfile(*vm.Endpoint);
			Collect(report, previous, after, vm);
			previous = after;
			report.Whole.Add(wholeElapsed);
			report.SubmitMaterializeRelease.Add(submitted);
			report.PumpCpu.Add(work.PumpCpu);
			report.HeartbeatCpu.Add(work.HeartbeatCpu);
			report.HostPending.Add(work.Pending);
			verify(); // Exact count, metadata, byte length and every word, outside every latency sample.
			if (cold) {
				report.Cleanup.Add(Cleanup(host, vm, baseline));
				if (memory) report.Memory[3] = Probe(host, vm);
			}
			host.SettleBudget();
		}
		if (!cold) {
			host.Budget.Begin();
			report.Cleanup.Add(Cleanup(host, vm, baseline));
			if (memory) report.Memory[3] = Probe(host, vm);
			host.SettleBudget();
		}
		if (count == 1048576 && requests == 2)
			Require(
				report.PeakResult == 16u * 1024u * 1024u,
				"C01 complex pair missed exact result-reservation boundary"
			);
		std::cout << "c01-gpu-budget counter_scope=body_cumulative vm=" << report.Language
				  << " cohort=" << report.Cohort
				  << " cohort_completed_bursts=" << host.Budget.CompletedBursts - budgetBefore;
		host.Budget.Print(std::cout);
		std::cout << '\n';
		return report;
	}
	void Run() {
		if (!Enabled("MONO_C01_GPU")) {
			std::cout << "c01 skipped=opt_in_required env=MONO_C01_GPU\n";
			return;
		}
		static const bool registered = [] {
			std::atexit(PrintLatest);
			return true;
		}();
		(void)registered;
		++BodyInvocation;
		Latest.clear();
		Latest.reserve(16);
		const bool memory = Enabled("MONO_C01_MEMORY");
		const auto trials = BoundedOption("MONO_C01_TRIALS", memory ? 1 : 101, 1, MAX_TRIALS);
		const auto warmups = BoundedOption("MONO_C01_WARMUPS", 3, 0, 32);
		Host host;
		for (const auto language : {script::Language::Luau, script::Language::JavaScript})
			for (const uint32_t count : {0u, 65u, 65537u, 1048576u}) {
				const auto source =
					Source(language, count); // Files and source construction never enter internal timing.
				const uint32_t requests = count > 65 ? 2 : 1;
				Latest.push_back(
					Measure(host, language, count, requests, true, trials, warmups, memory, source)
				);
				Latest.push_back(
					Measure(host, language, count, requests, false, trials, warmups, memory, source)
				);
			}
	}
}

BENCH("C01 real ComputeService | both VMs | cold and retained graph cohorts", 1) {
	Run();
}
