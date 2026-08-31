#pragma once

// GLSL to SPIR-V, in process, while the engine is running.
//
// The built-in shaders are compiled by `glslc` during the build, and that is
// the right place for them: a shader that ships with the engine should fail the
// build rather than the frame. This is for the ones that cannot be - a
// `ShaderScript` whose revision changed, a swapped antialias pass, a shader
// permutation the graph solved for. None of those exist at build time.
//
// No shaderc type appears here. The wrapper takes a string and hands back a
// vector of words, which is the whole reason to have one - `render`'s public
// surface must not make every consumer acquire a compiler API.
//
// **SPIR-V is the intermediate and not always the end.** SDL's Metal backend
// takes Metal Shading Language and never SPIR-V, so on that device the words
// this produces go through `engine::msl::Translate` before
// `SDL_CreateGPUShader` sees them - the same function the build runs over the
// built-in shaders. That step is a module of its own rather than a second half
// bolted on here, because a build tool needs it too and must not link a
// renderer to get it.
//
// @tier L12 · client

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace engine::render {

	// Stage whose GLSL rules and SPIR-V execution model apply to a compilation.
	//
	// @client
	enum class ShaderStage : uint8_t {
		// Vertex-processing stage.
		Vertex,

		// Fragment-processing stage.
		Fragment,

		// Compute-dispatch stage.
		Compute,
	};

	// One descriptor-backed resource declared by a compiled shader.
	enum class ShaderResourceKind : uint8_t {
		// Combined sampled image and sampler.
		SampledTexture,

		// Texture and sampler declared separately.
		SeparateTexture,
		Sampler,

		// Read-write image.
		StorageTexture,

		// Descriptor-backed data blocks.
		UniformBuffer,
		StorageBuffer,

		// Small pipeline-layout data block.
		PushConstants,
	};

	// Stable labels for report and editor surfaces.
	//@{
	const char *Describe(ShaderResourceKind kind);
	const char *Describe(ShaderStage stage);
	std::string ShaderCapabilityName(uint32_t capability);
	//@}

	// One reflected descriptor or push-constant block.
	struct ShaderResourceEstimate {
		// Debug name retained by the compiler, or `id N` when stripped.
		std::string Name;

		// What a pipeline must bind.
		ShaderResourceKind Kind = ShaderResourceKind::UniformBuffer;

		// Descriptor location. Push constants use zero for both.
		//@{
		uint32_t Set = 0;
		uint32_t Binding = 0;
		//@}

		// Statically declared bytes. Zero means dimensions are runtime-owned.
		uint64_t MinimumBytes = 0;
	};

	// Static requirements and cost indicators reflected from one SPIR-V module.
	//
	// Instruction figures are counts in the module, not predicted GPU cycles.
	// They are deliberately named estimates in Studio because occupancy, cache
	// behavior, divergence, and invocation count belong to a real workload.
	struct ShaderCapabilities {
		// Entry-point stage.
		ShaderStage Stage = ShaderStage::Fragment;

		// Every descriptor and push-constant block.
		std::vector<ShaderResourceEstimate> Resources;

		// Numeric SPIR-V capabilities in declaration order.
		std::vector<uint32_t> RequiredCapabilities;

		// Module residency and minimum buffer payload.
		//@{
		uint64_t SpirVBytes = 0;
		uint64_t DeclaredBufferBytes = 0;
		//@}

		// Static instruction counts by broad cost family.
		//@{
		uint32_t Instructions = 0;
		uint32_t ArithmeticInstructions = 0;
		uint32_t TextureInstructions = 0;
		uint32_t MemoryInstructions = 0;
		uint32_t ControlFlowInstructions = 0;
		//@}

		// Entry-point interface variable counts.
		//@{
		uint32_t Inputs = 0;
		uint32_t Outputs = 0;
		//@}

		// Compute local size. One in every dimension for non-compute stages.
		//@{
		uint32_t WorkgroupX = 1;
		uint32_t WorkgroupY = 1;
		uint32_t WorkgroupZ = 1;
		//@}
	};

	// Explicit transforms in their compile order.
	enum class ShaderOptimizationKind : uint8_t {
		ConstantFolding,
		CommonSubexpressionElimination,
	};

	// Returns the stable diagnostic name of an optimizer stage.
	const char *Describe(ShaderOptimizationKind kind);

	// The observable effect of one optimizer stage.
	struct ShaderOptimizationStep {
		// Which stage ran.
		ShaderOptimizationKind Kind = ShaderOptimizationKind::ConstantFolding;

		// Static instruction counts around the stage.
		//@{
		uint32_t BeforeInstructions = 0;
		uint32_t AfterInstructions = 0;
		//@}

		// Whether any module word changed, including id compaction.
		bool Changed = false;
	};

	// Reflects a compiled module without compiling or modifying it.
	ShaderCapabilities InspectShaderCapabilities(std::span<const uint32_t> spirv);

	// Owned output and diagnostics from one runtime shader compilation.
	//
	// `Failed` is the authoritative status. Successful output and diagnostic
	// strings remain valid independently of the compiler that produced them.
	//
	// @client
	struct ShaderCompilation {
		// Empty when Failed is true. A caller must check `Failed` rather than
		// the emptiness of this: a shader that legitimately compiles to nothing
		// is not a thing, but relying on that is how a stub gets mistaken for
		// a compiler.
		// The words are an owned SPIR-V 1.0 module on success.
		std::vector<uint32_t> SpirV;

		// Whether compilation failed; failure always carries a non-empty Error.
		bool Failed = false;

		// The compiler's own diagnostic, with line numbers. Shown to whoever
		// authored the shader - this is the string a script reads back from
		// `CompileError`, so it is part of the surface rather than a log line.
		std::string Error;

		// Warnings on a successful compile. Not an error, and not silently
		// dropped either.
		uint32_t Warnings = 0;

		// Reflected from `SpirV` after every enabled optimization pass.
		ShaderCapabilities Capabilities;

		// Empty when optimization is disabled. Otherwise one row per explicit
		// pass in execution order, including passes that found nothing to change.
		std::vector<ShaderOptimizationStep> Optimizations;
	};

	// Compiles runtime-authored GLSL to SPIR-V without exposing shaderc types.
	//
	// Built-in shaders use `glslc` during the build so invalid engine assets fail
	// the build. This compiler is for shaders that only exist or change while
	// the client runs; failures are returned as diagnostics and are not fatal.
	//
	// @client
	class ShaderCompiler {
	  public:
		// Creates a reusable GLSL compiler targeting Vulkan 1.0 and SPIR-V 1.0.
		ShaderCompiler();

		// Releases the in-process compiler and its options.
		~ShaderCompiler();

		// Compiler instances own non-copyable shaderc state.
		ShaderCompiler(const ShaderCompiler &) = delete;

		// Compiler instances own non-copyable shaderc state.
		ShaderCompiler &operator=(const ShaderCompiler &) = delete;

		// `name` appears in diagnostics and nowhere else. Give it something a
		// person can locate - the instance path of the script, not "shader".
		// Source and name are consumed during this call and are not retained.
		//
		// @param source GLSL source bytes; null termination is not required.
		// @param stage  Shader stage used to parse and compile the source.
		// @param name   Human-locatable label included with line numbers in diagnostics.
		// @return Owned SPIR-V and compiler diagnostics; check `Failed` for status.
		ShaderCompilation
		Compile(std::string_view source, ShaderStage stage, std::string_view name = "shader");

		// Optimisation costs compile time and is worth it for anything that
		// will be used for more than a frame or two. Enabling it runs the named
		// constant-folding and common-subexpression stages recorded in the result.
		// Keep it off for keystroke previews and on when the result is cached.
		//
		// @param optimise Whether subsequent compilations use performance optimisation.
		void SetOptimise(bool optimise);

	  private:
		struct Impl;

		// Incomplete here, which is the point of the pimpl, and owned by a
		// `unique_ptr` anyway: the deleter needs the complete type at the point
		// the destructor is *defined*, not at the point it is declared. It is
		// declared above and defined in `ShaderCompiler.cpp`, where `Impl` is
		// complete - the same arrangement `Renderer.hpp` uses.
		std::unique_ptr<Impl> State;
	};
}
