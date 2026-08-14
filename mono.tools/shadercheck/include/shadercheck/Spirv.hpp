#pragma once

// What a compiled SPIR-V module declares, read out of the binary itself.
//
// **A reader, not a compiler.** It walks the instruction stream once and keeps
// the four things a graphics API has to agree with the shader about: which
// entry point, at which stage, which capabilities, and which resources at which
// descriptor set and binding. Everything else in the module — the code, the
// types, the debug names of locals — is stepped over by word count.
//
//     Module module = ReflectBytes(bytes);
//     if (!module.Parsed()) { ... module.Error ... }
//
// **Why this exists rather than a call into SPIRV-Tools or SPIRV-Cross.**
// Neither is vendored, and the question being asked is small: this is the
// header layout and five opcodes, and it has to build in every preset,
// including the ones that configure no shader compiler at all. `docs/DEFERRED.md`
// D00001 carries the larger question of translating these modules to MSL, which
// does need a library this repository does not have yet.
//
// The numeric constants are the SPIR-V specification's own, and are spelled out
// in `Spirv.cpp` rather than included from `spirv-headers`: that header lives
// under `mono.vendor/shaderc/third_party/`, which a server preset never checks
// out, and a tool that cannot build without a shader compiler would defeat the
// point of reading the binary instead of running one.
//
// @tier L0 · shared

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace shadercheck {

	// The pipeline stage an entry point runs at.
	//
	// `Unsupported` is a value rather than a parse failure because a geometry or
	// tessellation module is perfectly valid SPIR-V — it is SDL's GPU API that
	// has no such stage, and Metal that has neither. That is a contract finding
	// with a name in it, not a corrupt file.
	enum class Stage { Vertex, Fragment, Compute, Unsupported };

	// What a bound resource is.
	//
	// **The order of these enumerators is the order SDL's contract lists them
	// in**, and `Contract.cpp` compares them with `<`. Reordering them silently
	// changes which shaders are accepted, so they are not alphabetical and must
	// not be made so.
	enum class ResourceKind { SampledTexture, StorageTexture, StorageBuffer, UniformBuffer };

	// One `OpVariable` that a pipeline has to bind something to.
	//
	// `HasSet` and `HasBinding` are separate from the values because an
	// undecorated resource is the failure worth naming: GLSL without an explicit
	// `layout(set = ..., binding = ...)` compiles, and every consumer then
	// invents a slot of its own.
	struct Resource {
		// The `OpName` the compiler recorded, or `id <n>` for a module stripped
		// of its debug names. Never empty, so a finding always names something.
		std::string Name;
		// What a pipeline has to bind here.
		ResourceKind Kind = ResourceKind::UniformBuffer;
		// The `DescriptorSet` decoration. Meaningless unless `HasSet`.
		uint32_t Set = 0;
		// The `Binding` decoration. Meaningless unless `HasBinding`.
		uint32_t Binding = 0;
		// Whether the module decorated this variable with a descriptor set.
		bool HasSet = false;
		// Whether the module decorated this variable with a binding.
		bool HasBinding = false;
	};

	// One compiled module, reduced to what the contract check needs.
	struct Module {
		// Empty when the module parsed. Anything else is a container that is not
		// SPIR-V, or is truncated, and no other field is meaningful.
		std::string Error;

		// The header's version word, as the specification packs it.
		uint32_t Version = 0;

		// The first entry point's name. `EntryPointCount` is kept separately
		// because "more than one" is itself a finding — a module with two entry
		// points has no single name for a caller to pass.
		std::string EntryPointName;
		// The stage the first entry point runs at.
		Stage EntryStage = Stage::Unsupported;
		// How many entry points the module declares.
		size_t EntryPointCount = 0;

		// Every `OpCapability`, in declaration order.
		std::vector<uint32_t> Capabilities;
		// Every bound resource, sorted by descriptor set and then by binding.
		std::vector<Resource> Resources;

		// Whether the bytes were a module at all. Nothing else is meaningful
		// when this is false.
		bool Parsed() const {
			return Error.empty();
		}
	};

	// Reflect a module already in words. The test suites build these by hand,
	// which is why this overload is the one the work happens in.
	Module Reflect(std::span<const uint32_t> words);

	// Reflect the bytes of a `.spv` file. A byte-reversed module — SPIR-V may be
	// stored either way round and the magic number says which — is swapped
	// rather than rejected.
	Module ReflectBytes(std::span<const std::byte> bytes);

	// The word a finding uses for a stage: "vertex", "fragment", "compute".
	std::string_view StageName(Stage stage);

	// The words a finding uses for a resource kind: "sampled texture",
	// "storage texture", "storage buffer", "uniform buffer".
	std::string_view KindName(ResourceKind kind);

	// The specification's name for a capability, or `capability <n>` for one
	// this file has no name for. A number alone tells a reader nothing about
	// what their shader did to earn it.
	std::string CapabilityName(uint32_t capability);
}
