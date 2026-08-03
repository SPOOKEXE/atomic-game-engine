# L10. The example scene, and the systems that move it.
#
# Above `script` at L9, because it loads one; above `scene` at L7, because what
# a script builds is made of those components.
#
# **`shared`, and that is the whole reason this module exists.** The ring scene
# lived in `mono.client/src/Demo.cpp` and could therefore only be built by a
# client — but a scene is not a client-tier idea. A server authors the same
# world and replicates it, and the unified harness runs both halves against one.
# Moving it here is what makes `--script path.luau` mean the same thing in all
# three programs instead of three near-copies drifting apart.
#
# What stays out is the part that genuinely is per-program: a client adds a
# camera and a draw list, a server adds neither, and neither of those belongs to
# the scene.
#
# The `.luau` and `.ts` files in this directory are the scenes themselves. They
# are staged into the assets tree so that `ExamplePath` resolves from any
# working directory.
mono_add_library(examples
	TIER shared
	DEPS
		Engine::core
		Engine::ecs
		Engine::scene
		Engine::script
)

# Staged rather than referenced in place: a program started from anywhere finds
# them, and `--assets` moves them with everything else.
file(GLOB example_scenes CONFIGURE_DEPENDS
	"${CMAKE_CURRENT_SOURCE_DIR}/*.luau"
	"${CMAKE_CURRENT_SOURCE_DIR}/*.js"
	"${CMAKE_CURRENT_SOURCE_DIR}/*.ts")

foreach(scene IN LISTS example_scenes)
	get_filename_component(scene_name "${scene}" NAME)
	add_custom_command(
		OUTPUT "${MONO_STAGE_ROOT}/assets/examples/${scene_name}"
		COMMAND ${CMAKE_COMMAND} -E make_directory "${MONO_STAGE_ROOT}/assets/examples"
		COMMAND ${CMAKE_COMMAND} -E copy_if_different "${scene}" "${MONO_STAGE_ROOT}/assets/examples/${scene_name}"
		DEPENDS "${scene}"
		COMMENT "Staging example scene ${scene_name}"
		VERBATIM)
	list(APPEND staged_scenes "${MONO_STAGE_ROOT}/assets/examples/${scene_name}")
endforeach()

if(staged_scenes)
	add_custom_target(example_scenes ALL DEPENDS ${staged_scenes})
	add_dependencies(engine_examples example_scenes)
endif()
