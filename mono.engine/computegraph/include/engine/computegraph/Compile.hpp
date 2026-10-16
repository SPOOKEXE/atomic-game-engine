#pragma once

// Pure buffer admission and translation into the shared dependency compiler.
// Compiled plans own declarations; failure never replaces an accepted plan.
// @tier L3 · shared

#include <engine/computegraph/Document.hpp>
#include <engine/graphcore/Compile.hpp>

#include <span>

namespace engine::computegraph {
	// Derived byte allocation, including one sentinel element for empty capacity.
	struct BufferAllocation {
		// Physical bytes reserved, and dense declaration index of the count buffer.
		uint64_t Bytes = 0;
		// Count buffer index, or UINT32_MAX when no device count is used.
		uint32_t CountBuffer = UINT32_MAX;
	};
	// Checked dispatch dimensions; Empty means the adapter records no dispatch.
	struct Dispatch {
		// Workgroup counts, zero on every axis for empty work.
		std::array<uint32_t, 3> Groups{};
		// Logical count for host-count work, capacity bound for device-count work.
		uint32_t Elements = 0;
		// Whether the host knows the logical element count before completion.
		bool DeviceCount = false;
	};
	// One resolved shader binding with declaration-local indices.
	struct ResolvedBinding {
		// Index into the selected kernel's port declarations and document buffers.
		uint32_t Port = 0;
		// Index into the document's buffer declarations.
		uint32_t Buffer = 0;
	};
	// One immutable-by-convention invocation ready for a rendering adapter.
	struct CompiledPass {
		// Dense kernel and dispatch-buffer indices.
		uint32_t Kernel = 0;
		// Document buffer index that bounds this pass's dispatch.
		uint32_t DispatchBuffer = 0;
		// Resolved bindings in kernel port declaration order.
		std::vector<ResolvedBinding> Bindings{};
		// Maximum admitted work; runtime input counts may reduce it.
		Dispatch Work;
	};
	// Device write-to-read dependency, including count-buffer dependencies.
	struct BufferBarrier {
		// Declaration-local producer, consumer and buffer indices.
		uint32_t Producer = 0;
		// Pass that reads the buffer after the producer writes it.
		uint32_t Consumer = 0;
		// Document buffer handed from producer to consumer.
		uint32_t Buffer = 0;
	};
	// Owned typed plan. The backend must admit executable reflection independently.
	struct CompiledGraph {
		// Accepted authored domain data.
		Document Definition;
		// Backend limits under which executable dispatches were admitted.
		DispatchLimits DeviceLimits;
		// Buffer reservations and resolved pass work.
		std::vector<BufferAllocation> Allocations{};
		// Resolved invocation work for each authored pass.
		std::vector<CompiledPass> Passes{};
		// Shared dependency order and domain-specific buffer handovers.
		graphcore::Topology Topology;
		// Buffer handovers required between dependent passes.
		std::vector<BufferBarrier> Barriers{};
		// Total bounded device payload, including all sentinel allocations.
		uint64_t Bytes = 0;
	};
	// Checks exact reflected kernel identity, version, named layouts and local dimensions.
	// Shader descriptor slots are contiguous per readonly/set-0 and writable/set-1 class.
	// This checks interface facts, not arbitrary executable shader memory safety.
	Status ValidateComputeBindings(
		const KernelDesc &kernel,
		const KernelReflection &reflection,
		const DispatchLimits &limits,
		Diagnostic &diagnostic
	);
	// Covers a one-dimensional buffer with checked workgroup arithmetic.
	// Y and Z local sizes must be one. Empty work succeeds with all group counts zero.
	Status ResolveComputeDispatch(
		uint32_t elements,
		const std::array<uint32_t, 3> &local,
		const DispatchLimits &limits,
		Dispatch &out,
		Diagnostic &diagnostic
	);
	// Admits typed declarations and every executable reflection, then compiles resource
	// dependencies through graphcore. External buffers are host inputs, never writable
	// graph outputs. Single-assignment buffers keep producer identity unambiguous.
	Status Compile(
		const Document &document,
		std::span<const KernelReflection> reflections,
		const Limits &limits,
		const DispatchLimits &dispatchLimits,
		CompiledGraph &out,
		Diagnostic &diagnostic
	);
}
