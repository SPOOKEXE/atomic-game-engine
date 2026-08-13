# L11 [client]. The engine's built-in GLSL, compiled from this module's shaders/.

# The light cap is declared once, in C++, and the shaders are told what it is.
#
# `MAX_SCENE_LIGHTS` sizes `LightUniforms` and `MAX_LIGHTS` bounds the loop in
# `opaque.frag`; the two must agree and nothing used to check that they did.
# Reading the value here rather than typing it again is what makes disagreement
# impossible instead of merely detectable — the shader has no literal left to
# drift.
#
# **The number lives with the renderer and the shader lives here**, so this
# reaches across for it. That is deliberate rather than sloppy: the value sizes
# a uniform struct `render` binds, so moving the declaration into this module
# would put it away from the code that has to agree with it — and the reach is a
# file read at configure time, not a link edge, so the dependency still points
# the way the tier check sees it.
set(lights_header "${CMAKE_SOURCE_DIR}/mono.engine/render/include/engine/render/Renderer.hpp")
file(STRINGS "${lights_header}" lights_line
	REGEX "inline constexpr size_t MAX_SCENE_LIGHTS")
if(NOT lights_line MATCHES "=[ \t]*([0-9]+)")
	message(FATAL_ERROR
		"resources: could not read MAX_SCENE_LIGHTS from ${lights_header}.\n"
		"  The shaders are given the light cap from there. If that declaration "
		"moved or changed shape, update this regex in the same commit.")
endif()
set(max_scene_lights "${CMAKE_MATCH_1}")

# Without this, editing the header re-compiles the C++ and leaves the shaders
# holding the value read at the last configure — which is the drift this is
# here to prevent, arriving by a different door.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${lights_header}")

mono_add_library(resources
	SHADER_DEFINES MAX_LIGHTS=${max_scene_lights}

	TIER client
	DEPS Engine::core
)
