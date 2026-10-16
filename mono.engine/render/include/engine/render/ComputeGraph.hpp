#pragma once

// Real-device execution of the separate typed buffer domain. The renderer lends
// its existing device; this adapter never opens a second graphics device.
// @tier L12 · client

#include <engine/computegraph/Program.hpp>
#include <engine/computegraph/Runtime.hpp>

#include <memory>
#include <string>
#include <vector>

namespace engine::render {
	// Neutral executable-source declaration shared with script hosts.
	using ComputeKernelCode = computegraph::KernelCode;
	// Neutral typed document plus independent executable sources.
	using ComputeGraphProgram = computegraph::Program;
	// Optional native observation for one exact ticket. Device duration covers its
	// whole standalone command, including upload, compute and download.
	// Counts are captured only while profiling is enabled. Four query slots are
	// retained through fence retirement; saturation leaves Available false.
	struct ComputeGraphTiming {
		// Submission ticket measured by this record.
		uint64_t Ticket = 0;
		// False when this ticket has no retained timing query.
		bool Available = false;
		// Elapsed device command duration in nanoseconds.
		double CommandNanoseconds = 0;
		// Number of compute dispatches in the command.
		uint32_t Dispatches = 0;
		// Total workgroups submitted across its dispatches.
		uint64_t Workgroups = 0;
		// Input bytes uploaded for the request.
		uint64_t UploadBytes = 0;
		// Resident instance bytes copied for the request.
		uint64_t InstanceCopyBytes = 0;
		// Output bytes copied back for the request.
		uint64_t ReadbackBytes = 0;
	};
	// Opt-in owner-thread counts. Canceled jobs count as retired only after their
	// fence or explicit retirement proof, independently of result consumption.
	// TimedJobs includes canceled measurements. DroppedTimings excludes C04 jobs.
	struct ComputeGraphStatistics {
		// Whether opt-in native profiling is enabled.
		bool Enabled = false;
		// Jobs admitted since profiling was enabled.
		uint64_t AcceptedJobs = 0;
		// Jobs whose device resources are safe to release.
		uint64_t RetiredJobs = 0;
		// Jobs with a retained device timing measurement.
		uint64_t TimedJobs = 0;
		// Measurements unavailable because query slots saturated.
		uint64_t DroppedTimings = 0;
		// Jobs submitted through the C04 recording batch.
		uint64_t BatchedJobs = 0;
		// Canceled jobs retired after device completion was proven.
		uint64_t CanceledRetirements = 0;
	};
	// Fixed host ABI v1: set 2/binding 0 is an optional std140 block containing
	// uvec4 Dispatch followed by uvec4 Buffers[32]. Dispatch.x is the current
	// invocation bound. Readonly descriptor slot N uses row N, writable slot N
	// uses row 16+N. Rows contain {host count, capacity, stride, device-count flag}.
	// Device counts remain explicit storage bindings; a flagged host count is zero.
	class ComputeGraph {
	  public:
		// Borrows the renderer's SDL_GPUDevice through its existing opaque boundary.
		// Construct, use and destroy on the device owner thread, before device shutdown.
		ComputeGraph(void *device, computegraph::Owner owner, computegraph::RuntimeLimits limits = {});
		~ComputeGraph();
		ComputeGraph(const ComputeGraph &) = delete;
		ComputeGraph &operator=(const ComputeGraph &) = delete;

		// Reflects executable SPIR-V and validates interfaces before device allocation.
		// Replacement refuses while GPU jobs remain in flight, preserving installed work.
		computegraph::Status
		Install(const ComputeGraphProgram &program, computegraph::Diagnostic &diagnostic);
		// Copies and bounds request data, then records every dependent GPU pass together.
		// There is no intermediate CPU count readback or immediate fence wait.
		computegraph::Status Submit(
			const computegraph::Request &request, uint64_t &ticketOut, computegraph::Diagnostic &diagnostic
		);
		// Queries completed fences and publishes validated copied results without waiting.
		void Poll();
		// Moves a completed result out and optionally copies its exact ticket timing.
		// Both retained result and timing records are consumed, even when timing is omitted.
		computegraph::Status Take(
			uint64_t ticket,
			computegraph::Completion &out,
			computegraph::Diagnostic &diagnostic,
			ComputeGraphTiming *timing = nullptr
		);
		// Disabled by default, with no query pools. Refuses changes while jobs/results
		// remain, a C04 recording batch is active, or submission is uncertain until
		// explicit shutdown proves device idle. Requires the private Vulkan cleanup
		// capability; ordinary opt-out and unsupported devices return false.
		// C04 jobs always use their controller's per-queue timing instead.
		bool EnableGpuTimings(bool enabled);
		// Snapshot since the most recent enable. No counters are drained by reading it.
		ComputeGraphStatistics Statistics() const;
		// Invalidates publication while retaining device resources until their fence completes.
		bool Cancel(uint64_t ticket);
		// Cancels every ticket from the previous host generation without blocking.
		void Reset(computegraph::Owner owner);
		// Supplies renderer graph-buffer use outside this pool. Admission accounts for
		// this amount plus all in-flight jobs under the combined 256 MiB ceiling.
		bool SetOutsideDeviceBytes(uint64_t bytes);
		// Supplies the one reserved readonly resident-instance exception. Each request
		// copies its admitted prefix on the GPU before execution. Null clears the binding.
		bool SetInstanceBuffer(void *buffer, uint32_t capacity, uint32_t stride);
		// Device reservations remain charged through cancellation until fence retirement.
		uint64_t ReservedDeviceBytes() const;
		// GPU jobs pending retirement, including canceled jobs.
		size_t PendingGpuCount() const;
		// Most recent compilation, admission, backend or completion failure.
		const std::string &LastError() const;
		// Explicit device teardown may wait for outstanding fences. Normal Poll never waits.
		void Shutdown();

	  private:
		struct State;
		std::unique_ptr<State> Held;
	};
}
