# L12 [client]. Shaders are compiled from this module's shaders/ directory.

# The light cap is declared once, in C++, and the shaders are told what it is.
#
# `MAX_SCENE_LIGHTS` sizes `LightUniforms` and `MAX_LIGHTS` bounds the loop in
# `opaque.frag`; the two must agree and nothing used to check that they did.
# Reading the value here rather than typing it again is what makes disagreement
# impossible instead of merely detectable — the shader has no literal left to
# drift.
set(lights_header "${CMAKE_CURRENT_SOURCE_DIR}/include/engine/render/Renderer.hpp")
file(STRINGS "${lights_header}" lights_line
	REGEX "inline constexpr size_t MAX_SCENE_LIGHTS")
if(NOT lights_line MATCHES "=[ \t]*([0-9]+)")
	message(FATAL_ERROR
		"render: could not read MAX_SCENE_LIGHTS from ${lights_header}.\n"
		"  The shaders are given the light cap from there. If that declaration "
		"moved or changed shape, update this regex in the same commit.")
endif()
set(max_scene_lights "${CMAKE_MATCH_1}")

# Without this, editing the header re-compiles the C++ and leaves the shaders
# holding the value read at the last configure — which is the drift this is
# here to prevent, arriving by a different door.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${lights_header}")

mono_add_library(render
	SHADER_DEFINES MAX_LIGHTS=${max_scene_lights}

	TIER client
	DEPS
		Engine::core

		# Scene data and culling decisions are shared with headless hosts.
		Engine::graph

		# Shared interface draw data; the client does not link Engine::ui.
		Engine::gui

		Engine::scene

		# `render::ParticleBatch` carries a span of `effects::ParticleInstance`,
		# which is the whole point of the type: the particle stream crosses from
		# simulation to presentation without being copied or converted. That makes
		# it a public dependency in the same way `assets` is — the payload type has
		# to propagate to whoever fills a batch.
		#
		# L8 is below L12 and `effects` is `shared`, so the edge goes downward and
		# the tier check has nothing to say about it.
		Engine::effects

		# Public table APIs use asset data types.
		Engine::assets

		Engine::assets

	# Keep GPU and shader compiler types private to implementation files.
	VENDOR SDL3::SDL3 shaderc

	# Atlas headers only; no imgui library is linked.
	VENDOR_INCLUDES ${CMAKE_SOURCE_DIR}/mono.vendor/imgui
)
