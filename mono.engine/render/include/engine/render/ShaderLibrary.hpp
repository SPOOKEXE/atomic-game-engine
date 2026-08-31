#pragma once

// From a name a world holds, to SPIR-V.
//
// **This is the consumer `ShaderCompiler` was written for.** That header exists
// because a `ShaderScript`'s revision can change while the engine runs, and
// until this file nothing in the engine ever asked it to compile anything -
// `docs/retired/DEFERRED.md` D00110 is the entry that records the gap, and the order it
// insists on: the thing that names a shader has to exist before the shaders do,
// or a library of defaults is a directory nothing loads.
//
// ## The resolution order, which is the whole design
//
// A `Material` names a shader - `scene::MaterialRef::Shader` - and that one
// name resolves three ways:
//
// | what the world holds | what the library gets |
// | --- | --- |
// | a `ShaderScript` of that name | its GLSL, compiled here, now |
// | no script, and the engine ships one | the staged SPIR-V `glslc` built |
// | neither | no module and a diagnostic saying so |
//
// **A script overrides a built-in rather than sitting beside it**, which is
// what makes the engine's own shaders defaults instead of a second mechanism: a
// world naming `toon` and holding no script draws the built-in, and the same
// world with a `ShaderScript` called `toon` draws that. Nothing else changes.
//
// **The third row is a diagnostic and not silence**, for `MissingTexture`'s
// reason one layer along: a misspelled shader and a part somebody deliberately
// left on the engine's default look identical from the frame, so the one that
// is a mistake has to say so somewhere.
//
// ## What this does not do
//
// **It never touches a device.** It holds words, and turning words into an
// `SDL_GPUShader` and a pipeline is `Renderer::AddShader`'s job - which is what
// lets the whole route above be tested without a GPU, as `render/AGENTS.md`
// requires of anything that can be.
//
// **It compiles what a world asks for, not what it holds.** A `ShaderScript`
// nobody selected is text somebody is still writing; compiling it every frame
// would charge the frame for an editor's open buffer.
//
// @tier L12 · client

#include <engine/core/Name.hpp>
#include <engine/render/ShaderCompiler.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace engine::ecs {
	class Store;
}

namespace engine::render {

	// What one name resolved to.
	//
	// @since v0.15
	struct ShaderModule {
		// The SPIR-V words, or empty when the resolution failed.
		//
		// **Check `Error` rather than this**, which is `ShaderCompilation`'s
		// rule for the same reason: relying on emptiness is how a stub gets
		// mistaken for a compiler.
		std::vector<uint32_t> SpirV;

		// The compiler's diagnostic, or empty on success.
		//
		// Non-empty for a shader that failed to compile *and* for a name
		// nothing in the world or the engine holds - both are things an author
		// wants to read, and both leave `SpirV` empty.
		std::string Error;

		// The `ShaderSource::Revision` these words were built from.
		//
		// Zero for a built-in, which has no script and cannot change while the
		// engine runs.
		uint32_t Revision = 0;

		// Whether this came from the engine's staged SPIR-V rather than from a
		// script in the world.
		bool BuiltIn = false;

		// Static requirements and cost indicators reflected from the compiled
		// module. Present for authored and built-in shaders alike.
		ShaderCapabilities Capabilities;

		// Explicit optimization passes applied to an authored module.
		std::vector<ShaderOptimizationStep> Optimizations;
	};

	// The shaders this engine ships, by the name a material selects them with.
	//
	// **The list is what makes those files reachable.** `resources/AGENTS.md`
	// refuses a default nothing consumes, and a `.frag` staged into
	// `shaders/resources/` that no name resolves to is exactly that - so a
	// shader added to that directory is added here in the same change or it is
	// not added at all.
	//
	// The file is the name plus `.frag`; the stage is fixed for the reason
	// `scene::ShaderSource` gives.
	//
	// @return The names, in declaration order.
	std::span<const std::string_view> BuiltInShaderNames();

	// Whether the engine ships a shader under this name.
	//
	// @param name The name a material selects.
	// @return `true` when `BuiltInShaderNames` contains it.
	bool IsBuiltInShader(std::string_view name);

	// Every shader a world's materials name, resolved and kept.
	//
	// **One per client rather than one per world**, because it is a cache over
	// process-wide names and the compiler inside it is expensive to build.
	// `Refresh` takes the world, so a client with several passes them in turn.
	//
	// @since v0.15
	class ShaderLibrary {
	  public:
		// Builds a library with its own runtime compiler.
		ShaderLibrary();

		// Releases the compiler and every module.
		~ShaderLibrary();

		// Compiler state is non-copyable.
		ShaderLibrary(const ShaderLibrary &) = delete;

		// Compiler state is non-copyable.
		ShaderLibrary &operator=(const ShaderLibrary &) = delete;

		// Resolves everything this world's materials name.
		//
		// **Idempotent, and that is the property the frame loop depends on.** A
		// script whose revision has not moved is not recompiled and a built-in
		// is never reloaded, so a steady world costs one walk over its
		// materials and an integer compare per distinct shader.
		//
		// Names nothing asks for any more are dropped, so this holds a picture
		// of what the world wants rather than of everything it ever wanted.
		//
		// @param store The world.
		// @return How many modules changed - compiled, loaded, or dropped.
		//         Zero is the steady state, and is what a caller checks before
		//         handing anything to a device.
		size_t Refresh(ecs::Store &store);

		// The module a name resolved to, or null.
		//
		// **Null means "nothing has asked for it"**, which is not the same as a
		// module carrying an `Error` - that one was asked for and could not be
		// produced.
		//
		// @param name The shader's name.
		// @return The module, valid until the next `Refresh`.
		const ShaderModule *Find(const core::Name &name) const;

		// The names whose modules changed during the last `Refresh`.
		//
		// **What a caller hands to a device.** A renderer rebuilds a pipeline
		// for these and leaves the rest alone; walking every module every frame
		// would rebuild every pipeline every frame.
		//
		// A dropped name appears here too, and `Find` answers null for it -
		// which is how a caller knows to release whatever it built.
		//
		// @return The names, valid until the next `Refresh`.
		std::span<const core::Name> Changed() const;

		// How many modules the library holds.
		//
		// @return The count.
		size_t Size() const;

	  private:
		struct Impl;

		// Incomplete here so `shaderc` stays out of this header, which is
		// `ShaderCompiler`'s rule and the reason it has a pimpl of its own.
		std::unique_ptr<Impl> State;
	};
}
