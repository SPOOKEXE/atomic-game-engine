#pragma once

// Real-device execution of the separate typed buffer domain. The renderer lends
// its existing device; this adapter never opens a second graphics device.
// @tier L12 · client

#include <engine/computegraph/Runtime.hpp>

#include <memory>
#include <string>
#include <vector>

namespace engine::render {
	// Named executable code supplied independently of the authored binding contract.
	struct ComputeKernelCode {
		// Stable identity matching one document kernel.
		std::string Name{};
		// Runtime-authored GLSL, or a staged engine shader filename, exclusively.
		std::string Source{};
		std::string BuiltIn{};
	};
	// One admitted domain document and its executable sources.
	struct ComputeGraphProgram {
		// Typed buffers, interfaces and resource dependencies.
		computegraph::Document Definition{};
		// One executable for every reusable kernel declaration.
		std::vector<ComputeKernelCode> Kernels{};
		// Named u32 diagnostic outputs. Every nonzero word prevents publication.
		// These may not also serve as device-count buffers.
		std::vector<std::string> ErrorBuffers{};
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
		// Moves a completed result out, releasing its retained CPU byte reservation.
		computegraph::Status
		Take(uint64_t ticket, computegraph::Completion &out, computegraph::Diagnostic &diagnostic);
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
