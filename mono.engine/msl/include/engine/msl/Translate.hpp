#pragma once

// SPIR-V to Metal Shading Language, and the resource indices SDL expects.
//
// Every backend SDL's GPU API has takes one shader format and no other. The
// Vulkan one takes SPIR-V, which is what `glslc` produces during the build and
// what `render::ShaderCompiler` produces while the engine runs; the Metal one
// takes MSL or a `metallib` and never SPIR-V. This is the step between the two,
// and it is a module rather than a function inside `render` because it has two
// callers at two different times: `mono.tools/shadercross` translates the
// built-in shaders during the build, and `render` translates a `ShaderScript`
// while the engine runs. One implementation is what stops those two from
// disagreeing about where a texture lands.
//
// **The index assignment is the whole reason this is more than three lines.**
// SPIRV-Cross numbers resources in the order it walks the module's ids, and
// `SDL_CreateGPUShader` documents a different order: sampled textures then
// storage textures, uniform buffers then storage buffers, each in descriptor
// order. The two agree for a shader with one of everything and disagree
// silently for every shader with two — measured on `opaque.frag`, where the
// automatic assignment put the last texture in the set at `[[texture(0)]]`.
//
// No SPIRV-Cross type appears here, for `ShaderCompiler.hpp`'s reason: words in,
// a string out, and no consumer has to acquire a translator's API to call it.
//
// @tier L11 · client

#include <cstdint>
#include <span>
#include <string>

namespace engine::msl {

	// A translated module, or the reason there is not one.
	//
	// `Failed` is the authoritative status; a failure always carries a non-empty
	// `Error` and an empty `Source`.
	//
	// @client
	struct Translation {
		// The MSL text on success, and empty otherwise.
		std::string Source;

		// Whether translation failed.
		bool Failed = false;

		// SPIRV-Cross's own diagnostic. Shown to whoever authored the shader,
		// so it is part of the surface rather than a log line.
		std::string Error;
	};

	// The entry point name in the translated source.
	//
	// **MSL reserves `main`, so SPIRV-Cross emits `main0`.** A caller passing
	// this to `SDL_GPUShaderCreateInfo::entrypoint` is passing the same fact the
	// translation produced, rather than a literal that has to be kept true by
	// hand — which is what `mono.tools/shadercheck` checks the built-in shaders
	// for from the other end.
	inline constexpr const char *ENTRY_POINT = "main0";

	// Translates one compiled SPIR-V module.
	//
	// @param spirv A complete SPIR-V module, as words.
	// @return The MSL, or a diagnostic. Never throws: SPIRV-Cross reports by
	//         exception and this is where that stops.
	// @client
	Translation Translate(std::span<const uint32_t> spirv);
}
