# L10. The example scene, and the systems that move it.
#
# Above `script` at L9, because it loads one; above `scene` at L7, because what
# a script builds is made of those components.
#
# **`shared`, and that is the whole reason this module exists.** The ring scene
# lived in `mono.client/src/Demo.cpp` and could therefore only be built by a
# client - but a scene is not a client-tier idea. A server authors the same
# world and replicates it, and the unified harness runs both halves against one.
# Moving it here is what makes `--script path.luau` mean the same thing in all
# three programs instead of three near-copies drifting apart.
#
# What stays out is the part that genuinely is per-program: a client adds a
# camera and a draw list, a server adds neither, and neither of those belongs to
# the scene.
#
# The demo sources live under `assets/scripts`; authored worlds live under
# `assets/worlds`. Both are staged into the assets tree for `DemosLoader`.
mono_add_library(examples
	TIER shared
	DEPS
		Engine::core
		Engine::ecs
		Engine::scene

		# L7, `shared`. **The other class tree.** `Interface.luau` is built
		# entirely out of `ScreenGui` and `Frame`, so `LoadScene` has to be able
		# to register the 2D tree as well as the 3D one - a loader that knew
		# only `Part` refused a shipped example with "not a registered class".
		Engine::gui

		# L8, `shared`. **The third class tree, for `gui`'s reason exactly.**
		# `Magic.luau` builds a `ParticleEmitter` per authored emitter and
		# `Particles.luau` is nothing but emitters, beams and trails, so
		# `LoadScene` has to register them - without it a shipped example threw
		# "'ParticleEmitter' is not a registered class" out of its heartbeat in
		# every program but the client, which registered them on its own path.
		Engine::effects
		Engine::graph

		Engine::script

		# L11. **Because a scene starts its own scripts.** `LoadScene` opens a VM
		# for the language the file's extension names, and `MakeRuntime` is the one
		# place both adapters are named - which is why this module sits at L12 since
		# v0.19 rather than beside `script`.
		Engine::scripthost
)

# Staged rather than referenced in place: a program started from anywhere finds
# them, and `--assets` moves them with everything else. Validate the whole
# source tree so a non-Luau file cannot silently miss the staging globs.
file(GLOB_RECURSE example_script_assets CONFIGURE_DEPENDS
	"${CMAKE_CURRENT_SOURCE_DIR}/assets/scripts/*")
foreach(asset IN LISTS example_script_assets)
	get_filename_component(asset_extension "${asset}" LAST_EXT)
	if(NOT asset_extension STREQUAL ".luau")
		message(FATAL_ERROR "examples: demo script asset ${asset} must be Luau")
	endif()
endforeach()

# Reconfiguration removes outputs from older demo catalogs that staged JS twins.
file(GLOB_RECURSE staged_javascript_demos
	"${MONO_STAGE_ROOT}/assets/examples/scripts/*.js"
	"${MONO_STAGE_ROOT}/assets/examples/scripts/*.js.map")
if(staged_javascript_demos)
	file(REMOVE ${staged_javascript_demos})
endif()

file(GLOB example_scenes CONFIGURE_DEPENDS
	"${CMAKE_CURRENT_SOURCE_DIR}/assets/scripts/*.luau")

# Client camera programs retain their template path alongside each builder.
file(GLOB_RECURSE demo_camera_scripts CONFIGURE_DEPENDS
	"${CMAKE_CURRENT_SOURCE_DIR}/assets/scripts/client/DemoCameras/*.luau")
foreach(companion IN LISTS demo_camera_scripts)
	file(RELATIVE_PATH companion_relative "${CMAKE_CURRENT_SOURCE_DIR}/assets/scripts" "${companion}")
	set(companion_staged "${MONO_STAGE_ROOT}/assets/examples/scripts/${companion_relative}")
	get_filename_component(companion_directory "${companion_staged}" DIRECTORY)
	add_custom_command(
		OUTPUT "${companion_staged}"
		COMMAND ${CMAKE_COMMAND} -E make_directory "${companion_directory}"
		COMMAND ${CMAKE_COMMAND} -E copy_if_different "${companion}" "${companion_staged}"
		DEPENDS "${companion}"
		COMMENT "Staging demo camera ${companion_relative}"
		VERBATIM)
	list(APPEND staged_scenes "${companion_staged}")
endforeach()

foreach(scene IN LISTS example_scenes)
	get_filename_component(scene_name "${scene}" NAME)
	add_custom_command(
		OUTPUT "${MONO_STAGE_ROOT}/assets/examples/scripts/${scene_name}"
		COMMAND ${CMAKE_COMMAND} -E make_directory "${MONO_STAGE_ROOT}/assets/examples/scripts"
		COMMAND ${CMAKE_COMMAND} -E copy_if_different "${scene}" "${MONO_STAGE_ROOT}/assets/examples/scripts/${scene_name}"
		DEPENDS "${scene}"
		COMMENT "Staging example scene ${scene_name}"
		VERBATIM)
	list(APPEND staged_scenes "${MONO_STAGE_ROOT}/assets/examples/scripts/${scene_name}")
endforeach()

# The Luau modules a scene owns, staged under that scene's own name.
#
# **One copy in the repository and a copy per scene in the stage**, which is the
# split that keeps both halves honest. `MagicCore` is byte-identical to the Rojo
# project it came from and three scenes use it, so three source copies
# would be three things to keep in step and they would stop agreeing the first
# time one was edited in a hurry. What the *runtime* wants is the opposite: a
# scene's modules under that scene's `Script`, so a world that never opens
# `Magic.luau` has no trace of `MagicCore` in it and a brand-new game's tree is
# empty rather than carrying a demo's.
#
# These used to stage into one `assets/lib` mirrored into every world's
# `ReplicatedStorage`, which made them a property of the host rather than of the
# scene - see `examples::MountSceneLibraries`.
#
# **Recursive, and the tree shape is the point.** `script::MountModuleTree`
# mirrors a directory into `ModuleScript` instances following Rojo's rule, so
# `MagicCore/Presets/init.luau` has to still be inside `Presets` when it lands -
# flattening the way the scene glob does would leave `require` unable to find a
# single sibling.
#
# `<scene>/<library>`, one pair per line. A script demo with no modules is
# absent.
set(scene_libraries
	Magic/MagicCore
	Magic/MagicRuntime
	Magic/TerrainCore
	Magic/TerrainRuntime
	MagicTests/MagicCore
	MagicTests/MagicTests
	MagicTests/TerrainCore
	MagicTests/TerrainRuntime
	Libraries/MagicCore
	Libraries/TerrainCore)

foreach(pair IN LISTS scene_libraries)
	string(REPLACE "/" ";" parts "${pair}")
	list(GET parts 0 scene_name)
	list(GET parts 1 library_name)

	file(GLOB_RECURSE library_files CONFIGURE_DEPENDS
		"${CMAKE_CURRENT_SOURCE_DIR}/assets/scripts/libraries/${library_name}/*.luau")

	if(NOT library_files)
		message(FATAL_ERROR "examples: ${scene_name} names assets/scripts/libraries/${library_name}, which holds no .luau files")
	endif()

	foreach(library IN LISTS library_files)
		file(RELATIVE_PATH library_relative "${CMAKE_CURRENT_SOURCE_DIR}/assets/scripts/libraries" "${library}")
		set(library_staged "${MONO_STAGE_ROOT}/assets/examples/scripts/${scene_name}/${library_relative}")
		get_filename_component(library_directory "${library_staged}" DIRECTORY)

		add_custom_command(
			OUTPUT "${library_staged}"
			COMMAND ${CMAKE_COMMAND} -E make_directory "${library_directory}"
			COMMAND ${CMAKE_COMMAND} -E copy_if_different "${library}" "${library_staged}"
			DEPENDS "${library}"
			COMMENT "Staging ${scene_name} module ${library_relative}"
			VERBATIM)
		list(APPEND staged_scenes "${library_staged}")
	endforeach()
endforeach()

file(GLOB example_worlds CONFIGURE_DEPENDS
	"${CMAKE_CURRENT_SOURCE_DIR}/assets/worlds/*.aworld")

foreach(world IN LISTS example_worlds)
	get_filename_component(world_name "${world}" NAME)
	set(world_staged "${MONO_STAGE_ROOT}/assets/examples/worlds/${world_name}")
	add_custom_command(
		OUTPUT "${world_staged}"
		COMMAND ${CMAKE_COMMAND} -E make_directory "${MONO_STAGE_ROOT}/assets/examples/worlds"
		COMMAND ${CMAKE_COMMAND} -E copy_if_different "${world}" "${world_staged}"
		DEPENDS "${world}"
		COMMENT "Staging example world ${world_name}"
		VERBATIM)
	list(APPEND staged_scenes "${world_staged}")
endforeach()

# Ambient demo audio is ordinary staged content. Its location is the published
# SoundId path, so `cdn --publish` can carry it from a clean build without a
# scene-specific path rewrite.
file(GLOB example_audio CONFIGURE_DEPENDS
	"${CMAKE_CURRENT_SOURCE_DIR}/assets/audio/*.wav")

foreach(audio IN LISTS example_audio)
	get_filename_component(audio_name "${audio}" NAME)
	set(audio_staged "${MONO_STAGE_ROOT}/assets/audio/${audio_name}")
	add_custom_command(
		OUTPUT "${audio_staged}"
		COMMAND ${CMAKE_COMMAND} -E make_directory "${MONO_STAGE_ROOT}/assets/audio"
		COMMAND ${CMAKE_COMMAND} -E copy_if_different "${audio}" "${audio_staged}"
		DEPENDS "${audio}"
		COMMENT "Staging example audio ${audio_name}"
		VERBATIM)
	list(APPEND staged_scenes "${audio_staged}")
endforeach()

# A packaged example texture is an ordinary baked asset, not a developer-local
# CDN import. The authored RGBA8 atlas keeps soft alpha edges; the explicit
# layout records animation metadata that a still PNG cannot carry itself.
set(flipbook_source "${CMAKE_CURRENT_SOURCE_DIR}/assets/effects/fox_dance.png")
set(flipbook_staged "${MONO_STAGE_ROOT}/assets/examples/effects/fox_dance.atex")
add_custom_command(
	OUTPUT "${flipbook_staged}"
	COMMAND ${CMAKE_COMMAND} -E make_directory "${MONO_STAGE_ROOT}/assets/examples/effects"
	COMMAND $<TARGET_FILE:assetc>
		--input "${CMAKE_CURRENT_SOURCE_DIR}/assets/effects"
		--output "${MONO_STAGE_ROOT}/assets/examples/effects"
		--flipbook-side 8
		--flipbook-frames 64
		--flipbook-fps 24
		--quiet
	DEPENDS assetc "${flipbook_source}"
	COMMENT "Baking packaged particle flipbook"
	VERBATIM)
list(APPEND staged_scenes "${flipbook_staged}")

file(GLOB example_pipelines CONFIGURE_DEPENDS
	"${CMAKE_CURRENT_SOURCE_DIR}/assets/pipelines/*.pipeline")

foreach(pipeline IN LISTS example_pipelines)
	get_filename_component(pipeline_name "${pipeline}" NAME)
	set(pipeline_staged "${MONO_STAGE_ROOT}/assets/examples/pipelines/${pipeline_name}")
	add_custom_command(
		OUTPUT "${pipeline_staged}"
		COMMAND ${CMAKE_COMMAND} -E make_directory "${MONO_STAGE_ROOT}/assets/examples/pipelines"
		COMMAND ${CMAKE_COMMAND} -E copy_if_different "${pipeline}" "${pipeline_staged}"
		DEPENDS "${pipeline}"
		COMMENT "Staging example render pipeline ${pipeline_name}"
		VERBATIM)
	list(APPEND staged_scenes "${pipeline_staged}")
endforeach()

if(staged_scenes)
	add_custom_target(example_scenes ALL DEPENDS ${staged_scenes})
	add_dependencies(engine_examples example_scenes)
endif()
