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
// SDL takes, which of the two staged files to open, and the entry point name -
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

	// The forced Vulkan backend always consumes SPIR-V, including through
	// MoltenVK on Apple.
	inline ShaderBinary ShaderBinaryFor(SDL_GPUDevice *device) {
		(void)device;
		return ShaderBinary{SDL_GPU_SHADERFORMAT_SPIRV, resources::ShaderForm::SpirV, "main"};
	}

	// The formats this build can supply, for `SDL_CreateGPUDevice`.
	//
	inline constexpr SDL_GPUShaderFormat SUPPORTED_SHADER_FORMATS = SDL_GPU_SHADERFORMAT_SPIRV;
}
