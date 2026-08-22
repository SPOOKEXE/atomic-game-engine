# L11 [client]. The engine's built-in GLSL, compiled from this module's shaders/.

# The light cap is declared once, in C++, and the shaders are told what it is.
#
# `MAX_SCENE_LIGHTS` sizes `LightUniforms` and `MAX_LIGHTS` bounds the loop in
# `opaque.frag`; the two must agree and nothing used to check that they did.
# Reading the value here rather than typing it again is what makes disagreement
# impossible instead of merely detectable - the shader has no literal left to
# drift.
#
# **The number lives with the renderer and the shader lives here**, so this
# reaches across for it. That is deliberate rather than sloppy: the value sizes
# a uniform struct `render` binds, so moving the declaration into this module
# would put it away from the code that has to agree with it - and the reach is a
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
# holding the value read at the last configure - which is the drift this is
# here to prevent, arriving by a different door.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${lights_header}")

# The instance row's word count, declared once in C++ and given to the shader.
#
# `occlusion-cull.comp` copies a survivor's instance row into the late buffer
# word by word, so it has to know how many words a row is - and unlike the two
# other consumers of that stride, no C++ compiler can check this one. A row that
# grew would keep building and would copy eight ninths of each survivor, which
# draws as geometry smeared across the scene.
#
# **Named `GPU_INSTANCE_WORDS` and not `INSTANCE_WORDS`, and the build found out
# why.** A define here is passed to *every* shader in the module, and
# `particle-step.comp` already declares `const uint INSTANCE_WORDS = 7u` for its
# own row - so the short name substituted an integer into that declaration and
# stopped the compile. A module-wide define shares a namespace with every
# identifier in every shader beside it, so it is spelled like the C++ constant it
# carries rather than like the thing it means locally.
#
# **The private header rather than a public one, and that is the honest source.**
# `render/AGENTS.md` keeps the device layout private, so promoting the number to
# `Renderer.hpp` just so this file could read it would be moving a fact away from
# the struct it describes in order to satisfy a build step. This is a file read
# at configure time and not a link edge, exactly as the light cap above is, so
# the module graph does not see it either way.
set(instance_header "${CMAKE_SOURCE_DIR}/mono.engine/render/src/InstancePacking.hpp")
file(STRINGS "${instance_header}" instance_words_line
	REGEX "inline constexpr size_t GPU_INSTANCE_WORDS")
if(NOT instance_words_line MATCHES "=[ \t]*([0-9]+)")
	message(FATAL_ERROR
		"resources: could not read GPU_INSTANCE_WORDS from ${instance_header}.\n"
		"  occlusion-cull.comp is given the instance stride from there. If that "
		"declaration moved or changed shape, update this regex in the same commit.")
endif()
set(instance_words "${CMAKE_MATCH_1}")

# The light cap's reason, for the same failure arriving by the same door: without
# this, editing the header recompiles the C++ and leaves the shader holding the
# value read at the last configure.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${instance_header}")

mono_add_library(resources
	SHADER_DEFINES
		MAX_LIGHTS=${max_scene_lights}
		GPU_INSTANCE_WORDS=${instance_words}

	TIER client
	DEPS Engine::core
)
