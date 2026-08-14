#pragma once

// Where the engine's built-in shaders are, once they have been staged.
//
// The GLSL lives in this module's `shaders/` directory, compiles to SPIR-V at
// build time and stages into `<program>/shaders/resources/`. Nothing here opens
// a file or talks to a device: this is the one place that knows the name the
// build stages under, so that a consumer spells a shader's file name and
// nothing else.
//
// **Two forms of every shader are staged, and the caller says which it wants.**
// The build writes `opaque.frag.spv` and, beside it, the `opaque.frag.msl` that
// `mono.tools/shadercross` translated — because SDL's Vulkan backend takes one
// and its Metal backend takes the other, and which is loaded is a property of
// the device rather than of the build.
//
// @tier L11 - client

#include <filesystem>
#include <string_view>

namespace engine::resources {

	// The name the build stages this module's SPIR-V under.
	//
	// Spelled once. `_mono_add_shaders` derives the staged directory from the
	// module name, so a rename of the module is a rename of a runtime path, and
	// a second spelling of it would be the one that was not changed with it.
	inline constexpr std::string_view SHADER_MODULE = "resources";

	// The compiled forms a shader is staged in.
	//
	// The values are the file suffixes, because that is what they are — a
	// caller passes one to `Shader` rather than translating an enumerator into
	// a string of its own.
	enum class ShaderForm {
		// SPIR-V, for SDL's Vulkan backend. Compiled by `glslc`.
		SpirV,

		// Metal Shading Language, for SDL's Metal backend. Translated from the
		// SPIR-V by `mono.tools/shadercross` during the same build.
		Msl,
	};

	// The staged shader for a built-in, in one of its compiled forms.
	//
	// @param name The GLSL file name, such as `opaque.vert`. The suffix the
	//        build appends is added here, so no caller carries it.
	// @param form Which compiled form to open. Ask the device rather than the
	//        platform: `SDL_GetGPUShaderFormats` is what answers it.
	// @return `<assets>/shaders/resources/<name>.<suffix>`. Not checked for
	//         existence — a missing built-in is a build that did not stage, and
	//         the caller that opens it is the one that can say what it wanted.
	//
	// No default. Which form a caller wants is the question this overload exists
	// to make somebody answer, and a default would answer it with the one that
	// happens to work on the platform this was written on.
	std::filesystem::path Shader(std::string_view name, ShaderForm form);
}
