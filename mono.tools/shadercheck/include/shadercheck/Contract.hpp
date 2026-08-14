#pragma once

// One compiled shader against the contract `SDL_CreateGPUShader` holds it to,
// checked on a machine that cannot run every backend it is checked for.
//
// **What this is really about is Metal.** `docs/DEFERRED.md` D00001 has carried
// "macOS builds compile SPIR-V but not MSL" since v0.1 with no trigger on it,
// because nobody has a Mac to trip it. Most of what a Mac would find is not
// about Metal at all: it is about whether the SPIR-V this engine ships carries
// the information every other backend derives its bindings from. That question
// is answerable from Linux, and this answers it.
//
// The rules are SDL's, quoted from `SDL_gpu.h`'s own documentation of
// `SDL_CreateGPUShader` and `SDL_CreateGPUComputePipeline`, and they are a
// contract rather than a convention: a resource in the wrong descriptor set
// does not bind to the wrong thing, it binds to nothing.
//
// **What this deliberately does not claim.** It does not translate anything, it
// does not prove MSL exists, and it proves nothing about a Metal device. It
// proves that the modules are well-formed, single-entry, decorated, and laid
// out the way every backend's translation reads — which is the part of D00001
// that does not need a Mac.
//
// @tier L0 · shared

#include <shadercheck/Spirv.hpp>
#include <string>
#include <string_view>
#include <vector>

namespace shadercheck {

	// The entry point `render::Renderer::LoadShader` and `render::InterfacePass`
	// pass to `SDL_CreateGPUShader` for a SPIR-V module.
	inline constexpr std::string_view SPIRV_ENTRY_POINT = "main";

	// The same entry point after translation, and the reason these two sit on
	// adjacent lines.
	//
	// **MSL reserves `main`, so SPIRV-Cross emits `main0`.** The name a caller
	// hands SDL therefore depends on the format the device took, which is one
	// fact with two spellings — kept here together so that whoever changes
	// either finds the other, rather than finding a black window.
	inline constexpr std::string_view MSL_ENTRY_POINT = "main0";

	// One thing wrong with one module, phrased for somebody reading a build log.
	struct Finding {
		// One sentence, naming the resource or the capability and why it is a
		// problem. It ends up in a build log, so it says the reason and not a
		// rule number.
		std::string Message;
	};

	// `expected` is the stage the file's name claims — `opaque.vert.spv` is a
	// vertex shader. A module whose entry point disagrees with its own filename
	// is loaded under the wrong stage by every caller that trusts the name, and
	// `render::Renderer` is one of them.
	std::vector<Finding> Check(const Module &module, Stage expected);

	// Where each resource lands once the module is translated, in the index
	// space SDL's Metal backend reads: `[[texture]]` for the two texture kinds,
	// `[[buffer]]` for the two buffer kinds, and a sampler taking the index of
	// the texture it belongs to.
	//
	// Parallel to `module.Resources`. Derived from SDL's documented ordering and
	// nothing else — this is what a Mac will look for, not evidence that a Mac
	// found it. It is worth having because the assignment is a property of the
	// SPIR-V, so it can be read, printed and diffed here rather than guessed at
	// from the far side of a platform nobody in this repository has.
	std::vector<uint32_t> MetalIndices(const std::vector<Resource> &resources);
}
