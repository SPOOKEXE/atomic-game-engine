#pragma once

// Typed compute declarations without devices, world storage or a common graph VM.
// Counts produced on the device remain unknown to the host until completion.
// @tier L3 · shared

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace engine::computegraph {
	// Closed storage layouts. EngineInstance is the host's readonly resident exception.
	enum class ElementType : uint8_t { U32, F32, F32x4, EngineInstance };
	// A buffer declaration grants access; each kernel port requests a subset.
	enum class Access : uint8_t { Read, Write, ReadWrite };
	// Device ownership scope; none permits pointers to cross a world boundary.
	enum class Lifetime : uint8_t { Submission, World, View };
	// Bounded failure vocabulary shared by admission, references and runtime contracts.
	enum class Status : uint8_t {
		Ok,
		LimitExceeded,
		InvalidName,
		DuplicateName,
		UnknownBuffer,
		UnknownKernel,
		InvalidType,
		InvalidCount,
		AccessDenied,
		AliasConflict,
		ReflectionMismatch,
		InvalidDispatch,
		MissingProducer,
		MultipleProducers,
		Cycle,
		OutputOverflow,
		StaleOwner,
		UnknownTicket,
		NotReady,
		AllocationFailed,
		DeviceUnavailable,
		DeviceFailure,
	};
	// Refusal identity is bounded independently of untrusted authored names.
	struct Diagnostic {
		// Machine-readable refusal.
		Status Code = Status::Ok;
		// Buffer, pass or kernel name, truncated to 128 bytes.
		std::string Subject{};
	};
	// Explicit host admission limits, never inferred from memory availability.
	struct Limits {
		// Maximum declarations in each collection.
		size_t MaximumBuffers = 128;
		size_t MaximumKernels = 64;
		size_t MaximumPasses = 256;
		// Combined authored/reflected ports and pass bindings, respectively.
		size_t MaximumPorts = 512;
		size_t MaximumBindings = 1024;
		// Combined authored and reflected identity bytes.
		size_t MaximumNameBytes = 64 * 1024;
		// Device payload budgets, including empty-buffer sentinel allocations.
		uint64_t MaximumBufferBytes = 64u * 1024u * 1024u;
		uint64_t MaximumTotalBytes = 256u * 1024u * 1024u;
	};
	// Backend-reported dispatch limits. Defaults are conservative admission policy.
	struct DispatchLimits {
		// Largest local size and workgroup count on each axis.
		std::array<uint32_t, 3> MaximumLocal{256, 256, 64};
		std::array<uint32_t, 3> MaximumGroups{65535, 65535, 65535};
		// Largest product of local dimensions.
		uint32_t MaximumThreads = 256;
	};
	// Named buffer with separate physical capacity and logical element count.
	struct BufferDesc {
		// Durable document-local identity.
		std::string Name{};
		// Exact element layout and bytes between consecutive elements.
		ElementType Type = ElementType::U32;
		uint32_t Stride = 4;
		// Maximum elements, independent of the current count.
		uint32_t Capacity = 0;
		// Host-known logical count, used only when CountBuffer is empty.
		uint32_t LogicalCount = 0;
		// Optional u32 scalar buffer containing the device-produced count.
		std::string CountBuffer{};
		// Granted access and whether a host supplies this buffer before execution.
		Access Permission = Access::ReadWrite;
		bool External = false;
		// Stable owner identity and retention scope.
		std::string Owner{};
		Lifetime Retention = Lifetime::Submission;
	};
	// One named shader buffer binding. Stride and type describe std430 element storage.
	struct KernelPort {
		// Durable shader block identity, not its ordinal in a vector.
		std::string Name{};
		// Reflected element layout and required access.
		ElementType Type = ElementType::U32;
		uint32_t Stride = 4;
		Access Permission = Access::Read;
		// Explicit descriptor location under the backend's binding convention.
		uint32_t Set = 0;
		uint32_t Binding = 0;
		// Minimum full buffer allocation required by the shader block.
		uint64_t MinimumBytes = 0;
		// Compares the full versioned binding contract.
		bool operator==(const KernelPort &) const = default;
	};
	// Reusable authored kernel interface; the rendering host resolves executable code.
	struct KernelDesc {
		// Stable kernel asset or built-in name and interface version.
		std::string Name{};
		uint32_t Version = 1;
		// Shader-declared local dimensions.
		std::array<uint32_t, 3> Local{64, 1, 1};
		// Named ports, independent of declaration order.
		std::vector<KernelPort> Ports{};
	};
	// Backend reflection must be supplied independently from the authored declaration.
	struct KernelReflection {
		// Stable kernel identity and agreed interface metadata version.
		std::string Name{};
		uint32_t Version = 1;
		// Actual executable local dimensions and storage interface.
		std::array<uint32_t, 3> Local{64, 1, 1};
		std::vector<KernelPort> Ports{};
	};
	// One pass maps a named shader port to a named document buffer.
	struct Binding {
		// Kernel-local port and document-local buffer.
		std::string Port{};
		std::string Buffer{};
	};
	// One invocation; an optional count buffer controls shader guards, never host waits.
	struct PassDesc {
		// Durable pass and reusable kernel identities.
		std::string Name{};
		std::string Kernel{};
		// Buffer whose host count or device-count capacity bounds dispatch.
		std::string DispatchBuffer{};
		// Exact named shader bindings.
		std::vector<Binding> Bindings{};
	};
	// Complete domain document. It is not a serialized form of dense compiled indices.
	struct Document {
		// Named buffers, reusable kernel contracts, and work declarations.
		std::vector<BufferDesc> Buffers{};
		std::vector<KernelDesc> Kernels{};
		std::vector<PassDesc> Passes{};
	};
	// Stable diagnostic and type spellings.
	const char *Describe(Status status);
	const char *Describe(ElementType type);
	// Packed scalar/vector stride; zero for the reserved opaque host layout or invalid type.
	uint32_t ElementStride(ElementType type);
}
