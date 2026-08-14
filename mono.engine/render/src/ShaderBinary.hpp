#pragma once

// What a device wants a shader to be, asked once and carried.
//
// **The format used to be a literal, and that is the bug this file exists to
// remove.** `SDL_CreateGPUDevice(SDL_GPU_SHADERFORMAT_SPIRV, ...)` and three
// `info.format = SDL_GPU_SHADERFORMAT_SPIRV` lines said what the build happened
// to produce on the platform they were written on. SDL's Metal backend offers
// `MSL` and `METALLIB` and never SPIR-V, so on macOS the first of those returned
// null before a shader was read and the other three would have been wrong
// afterwards.
//
// Three facts move together and are therefore one type: the format enumerator
// SDL takes, which of the two staged files to open, and the entry point name —
// MSL reserves `main`, so a translated module's entry point is `main0`.
//
// Private to `render`. Nothing outside this module has to know which of the two
// a device took; `resources::ShaderForm` is the part that crosses.

#include <engine/msl/Translate.hpp>
#include <engine/resources/Shaders.hpp>

#include <SDL3/SDL_gpu.h>

namespace engine::render {

	// The shader format a device takes, and what follows from it.
	struct ShaderBinary {
		SDL_GPUShaderFormat Format = SDL_GPU_SHADERFORMAT_SPIRV;
		resources::ShaderForm Form = resources::ShaderForm::SpirV;
		const char *EntryPoint = "main";
	};

	// What this device takes, from the device rather than from the platform.
	//
	// **SPIR-V wins where a backend offers both**, which is not hypothetical:
	// MoltenVK is a Vulkan device on Apple hardware and takes SPIR-V. Preferring
	// it keeps the only path anybody has verified on the machines that can run
	// it, and leaves MSL for the backend that has no other option.
	inline ShaderBinary ShaderBinaryFor(SDL_GPUDevice *device) {
		const SDL_GPUShaderFormat formats = SDL_GetGPUShaderFormats(device);
		if ((formats & SDL_GPU_SHADERFORMAT_MSL) != 0 && (formats & SDL_GPU_SHADERFORMAT_SPIRV) == 0) {
			return ShaderBinary{SDL_GPU_SHADERFORMAT_MSL, resources::ShaderForm::Msl, msl::ENTRY_POINT};
		}
		return ShaderBinary{SDL_GPU_SHADERFORMAT_SPIRV, resources::ShaderForm::SpirV, "main"};
	}

	// The formats this build can supply, for `SDL_CreateGPUDevice`.
	//
	// Both, because the build produces both: `glslc` compiles the GLSL to SPIR-V
	// and `mono.tools/shadercross` translates every module to MSL beside it. A
	// request naming only one is a request for a device the build could have
	// served, refused before a shader is read.
	inline constexpr SDL_GPUShaderFormat SUPPORTED_SHADER_FORMATS =
		SDL_GPU_SHADERFORMAT_SPIRV | SDL_GPU_SHADERFORMAT_MSL;
}
