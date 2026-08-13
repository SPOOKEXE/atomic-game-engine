#pragma once

// Where the engine's built-in shaders are, once they have been staged.
//
// The GLSL lives in this module's `shaders/` directory, compiles to SPIR-V at
// build time and stages into `<program>/shaders/resources/`. Nothing here opens
// a file or talks to a device: this is the one place that knows the name the
// build stages under, so that a consumer spells a shader's file name and
// nothing else.
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

	// The staged SPIR-V for a built-in shader.
	//
	// @param name The GLSL file name, such as `opaque.vert`. The `.spv` the
	//        compiler appends is added here, so no caller carries it.
	// @return `<assets>/shaders/resources/<name>.spv`. Not checked for
	//         existence — a missing built-in is a build that did not stage, and
	//         the caller that opens it is the one that can say what it wanted.
	std::filesystem::path Shader(std::string_view name);
}
