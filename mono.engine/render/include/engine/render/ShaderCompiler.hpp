#pragma once

// GLSL to SPIR-V, in process, while the engine is running.
//
// The built-in shaders are compiled by `glslc` during the build, and that is
// the right place for them: a shader that ships with the engine should fail the
// build rather than the frame. This is for the ones that cannot be — a
// `ShaderScript` whose revision changed, a swapped antialias pass, a shader
// permutation the graph solved for. None of those exist at build time.
//
// No shaderc type appears here. The wrapper takes a string and hands back a
// vector of words, which is the whole reason to have one — `render`'s public
// surface must not make every consumer acquire a compiler API.
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
		// authored the shader — this is the string a script reads back from
		// `CompileError`, so it is part of the surface rather than a log line.
		std::string Error;

		// Warnings on a successful compile. Not an error, and not silently
		// dropped either.
		uint32_t Warnings = 0;
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
		// person can locate — the instance path of the script, not "shader".
		// Source and name are consumed during this call and are not retained.
		//
		// @param source GLSL source bytes; null termination is not required.
		// @param stage  Shader stage used to parse and compile the source.
		// @param name   Human-locatable label included with line numbers in diagnostics.
		// @return Owned SPIR-V and compiler diagnostics; check `Failed` for status.
		ShaderCompilation
		Compile(std::string_view source, ShaderStage stage, std::string_view name = "shader");

		// Optimisation costs compile time and is worth it for anything that
		// will be used for more than a frame or two. Off while a user is
		// editing, on when the result is cached.
		//
		// @param optimise Whether subsequent compilations use performance optimisation.
		void SetOptimise(bool optimise);

	  private:
		struct Impl;

		// Incomplete here, which is the point of the pimpl, and owned by a
		// `unique_ptr` anyway: the deleter needs the complete type at the point
		// the destructor is *defined*, not at the point it is declared. It is
		// declared above and defined in `ShaderCompiler.cpp`, where `Impl` is
		// complete — the same arrangement `Renderer.hpp` uses.
		std::unique_ptr<Impl> State;
	};
}
