#pragma once

// A shader an author wrote, as an instance in a world.
//
// **This is the class `render/ShaderCompiler.hpp` was written for and that did
// not exist.** That header names "a `ShaderScript` whose revision changed" as
// the reason a *runtime* compiler ships at all, and `render/AGENTS.md` repeats
// it — and until this file there was no class, no property and no path from a
// world to a compile. `docs/retired/DEFERRED.md` D00110 is the entry that says so, and
// it also says why the shaders came second: a shader reaches the GPU only by
// being named, so the thing that names one has to exist first.
//
// ## What a `ShaderScript` is
//
// `Instance.new("ShaderScript")` holding GLSL in `Source`, exactly as a
// `Script` holds Luau. It is an `Instance` and not a `PVInstance`, for
// `Attachment`'s reason: it has no place in the world, and a `Transform` on
// this row would be a second opinion about where the thing that uses it is.
//
// **Fragment stage only.** `ShaderSource` carries the argument: a vertex shader
// would have to agree with the renderer's private instance layout, and
// `render/AGENTS.md` says that layout is private and stays private.
//
// ## How one is selected
//
// `Material.Shader` names it — a string, rule 4, so it survives a save file and
// a wire. The name is resolved twice over, and the order is the whole design:
//
// 1. a `ShaderScript` in this world with that instance name, whose GLSL is
//    compiled while the engine runs, and
// 2. failing that, a shader this engine ships, which was compiled by `glslc`
//    during the build.
//
// **That order is what makes an author's shader an override rather than a
// separate mechanism.** A world that names `toon` and holds no `ShaderScript`
// draws the built-in one; the same world with a `ShaderScript` called `toon`
// under it draws that instead, and nothing else changes. The resolution itself
// is `render::ShaderLibrary`'s, because step 2 needs a staged file and a
// headless host has neither.
//
// ## Nothing here compiles anything
//
// `scene` is `shared`. This module holds the text, hands out the names, and
// says which shaders a world's materials ask for; `render` at L12 is what turns
// one into SPIR-V. Same split `Visual::Mesh` has against `MeshTable`.
//
// @tier L7 · shared

#include <engine/core/Name.hpp>
#include <engine/ecs/Classes.hpp>
#include <engine/ecs/Entity.hpp>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace engine::ecs {
	class Store;
}

namespace engine::scene {

	// One shader script's text and how many times it has been written.
	//
	// **A revision beside the code rather than a hash of it.** A library
	// deciding whether to recompile compares an integer per script per frame;
	// hashing the source would read every byte of every shader in the world to
	// answer the same question, and the answer is one nobody disputes — the
	// setter is the only thing that writes either field.
	//
	// @since v0.15
	struct ShaderText {
		// The GLSL, or empty when nothing in this world holds that name.
		std::string Code;

		// The script's revision, or zero when there is no script.
		uint32_t Revision = 0;

		// Whether a script was found at all.
		//
		// **Separate from `Code` being empty**, because a `ShaderScript` an
		// author has created and not yet typed into is a real state and is not
		// the same as no script: the first says "compile nothing and say why",
		// the second says "look for a built-in of that name".
		bool Found = false;
	};

	// The `ShaderScript` class id, registering the tree if nobody has yet.
	//
	// @return The class id.
	ecs::ClassId ShaderScriptClass();

	// The shader script in this world with that instance name.
	//
	// **Names are not unique in an instance tree**, so the first one found in
	// entity order wins and that order is stable — the same rule `ScriptsIn`
	// applies to its own walk. Two shader scripts sharing a name is an authoring
	// mistake this does not try to diagnose.
	//
	// @param store The world.
	// @param name  The script's instance name.
	// @return The instance, or `ecs::NULL_ENTITY`.
	ecs::Entity ShaderScriptNamed(ecs::Store &store, const core::Name &name);

	// The GLSL a world holds under a name, and its revision.
	//
	// **Never creates anything and never compiles anything.** This is what a
	// client-tier library calls once per demanded shader per frame, so it is a
	// lookup and a copy of a string that a compile would have copied anyway.
	//
	// **A mutable `Store &` for a read**, which every walk here takes: `Each` is
	// the only iteration this storage has and it is not `const`. Nothing below
	// writes a row.
	//
	// @param store The world.
	// @param name  The script's instance name.
	// @return The text, with `Found` false when this world holds no such script.
	ShaderText ShaderTextOf(ecs::Store &store, const core::Name &name);

	// Writes GLSL onto a shader script and bumps its revision.
	//
	// **The revision moves here and nowhere else**, which is what lets a
	// consumer trust it: a caller that wrote `ShaderSource::Code` directly would
	// leave the counter saying the shader had not changed, and the frame would
	// go on drawing the last compile. The `Source` property goes through this.
	//
	// @param store  The world.
	// @param script The shader script.
	// @param code   The GLSL.
	// @return `false` when the entity holds no `ShaderSource`.
	bool SetShaderSource(ecs::Store &store, ecs::Entity script, std::string_view code);

	// Every shader the materials in this world name, without duplicates.
	//
	// A mutable `Store &` for a read, for `ShaderTextOf`'s reason.
	//
	// **Walked from the materials rather than from the scripts**, and that is
	// the difference between what a world *holds* and what it *asks for*: a
	// `ShaderScript` nobody selected is text somebody is still writing, and
	// compiling it every frame would charge the frame for an editor's open
	// buffer. A built-in name appears here too, because a material may name one
	// and no world contains those.
	//
	// @param store The world.
	// @param out   Filled with the names, sorted by id. Cleared first.
	// @return How many distinct shaders are named.
	size_t DemandedShaders(ecs::Store &store, std::vector<core::Name> &out);
}
