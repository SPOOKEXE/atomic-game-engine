#pragma once

// GLSL the runtime compiler needs as text, written by mono.build/EmbedShaders.cmake
// from the same files the build compiles. Includes are already expanded.

#include <string_view>

namespace engine::render {
	extern const std::string_view SURFACE_SHADING_GLSL;
	extern const std::string_view ENGINE_BRDF_GLSL;
	extern const std::string_view ENGINE_SURFACE_GLSL;
	extern const std::string_view ENGINE_SURFACE_MAIN_GLSL;
}
