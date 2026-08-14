#pragma once

// The translated shader, read back against the module it came from.
//
// **The point of this file is that it is not the translator.**
// `mono.tools/shadercross` runs SPIRV-Cross over every `.spv` the build
// compiles and writes an `.msl` beside it; a step that emits a file nobody
// checks is what `docs/DEFERRED.md` D00001 already recorded once, under the
// words "wired in CMake and untested". So the emitted text is read here by
// something that links no translator and derives its expectations from the
// SPIR-V, and a disagreement between the two ends is a failed check rather than
// a shared mistake.
//
// It asks four things, and the third is the one that has already caught
// something: the file is structurally well-formed MSL; it declares exactly one
// entry point, named `main0` and qualified for the stage the module runs at;
// every `[[texture]]`, `[[buffer]]` and `[[sampler]]` index in that entry
// point's signature is the one `MetalIndices` derives from SDL's documented
// order; and every resource named in the signature is a resource the module
// declares.
//
// **What it cannot claim.** It is not a Metal compiler and there is none on
// Linux, so "structurally well-formed" means balanced and prefaced and no more
// - a type error inside a function body passes here. `Apple`'s `metal` is the
// only thing that settles that, and D00001 says so rather than this file
// pretending otherwise.
//
// @tier L0 · shared

#include <shadercheck/Contract.hpp>
#include <shadercheck/Spirv.hpp>
#include <string_view>
#include <vector>

namespace shadercheck {

	// One translated module against the SPIR-V it was translated from.
	//
	// @param module The reflection of the `.spv`, already parsed.
	// @param msl    The whole text of the `.msl` written beside it.
	// @return Everything wrong with the pair, empty when they agree.
	std::vector<Finding> CheckMsl(const Module &module, std::string_view msl);
}
