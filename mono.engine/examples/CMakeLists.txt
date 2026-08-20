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
# The `.luau` and `.ts` files in this directory are the scenes themselves. They
# are staged into the assets tree so that `ExamplePath` resolves from any
# working directory.
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

		Engine::script
)

# Staged rather than referenced in place: a program started from anywhere finds
# them, and `--assets` moves them with everything else.
file(GLOB example_scenes CONFIGURE_DEPENDS
	"${CMAKE_CURRENT_SOURCE_DIR}/*.luau"
	"${CMAKE_CURRENT_SOURCE_DIR}/*.js"
	"${CMAKE_CURRENT_SOURCE_DIR}/*.ts")

# **A `.ts` file is staged as stripped JavaScript, not copied.**
#
# `script/Runtime.hpp` has always said a `.ts` file "is expected to have been
# type-stripped already - nothing in the C++ build compiles TypeScript, and
# nothing should: the engine loads what a toolchain emitted". Nothing emitted
# it. `tsconfig.json` sets `noEmit` and `just typecheck` runs `tsc --noEmit`, so
# every `.ts` example was copied verbatim and QuickJS refused it at the first
# type annotation - `Mirrors-1-world.ts` since the day it was written.
#
# This is the toolchain half, and it stays a toolchain: `tsc` transpiles, the
# build copies the result. `--isolatedModules` transpiles file by file with no
# type checking, which is the right split - `just typecheck` is where types are
# checked, and doing it twice would make a build fail for a reason a typechecker
# already reported better.
#
# **A missing `tsc` is not fatal.** A checkout without `bun install` still
# builds and runs every Luau example; what is lost is the JavaScript twins, and
# the configure says so once rather than failing over a dependency that is only
# needed to author in the second language.
set(_mono_tsc "${CMAKE_SOURCE_DIR}/node_modules/.bin/tsc")
if(NOT EXISTS "${_mono_tsc}")
	set(_mono_tsc "")
	message(STATUS "examples: no node_modules/.bin/tsc - .ts scenes will not be staged")
endif()

foreach(scene IN LISTS example_scenes)
	get_filename_component(scene_name "${scene}" NAME)
	get_filename_component(scene_extension "${scene}" LAST_EXT)

	if(scene_extension STREQUAL ".ts")
		if(NOT _mono_tsc)
			continue()
		endif()

		# Emitted under the same stem with a `.js` suffix, because that is what
		# was actually produced - naming the output `.ts` would put stripped
		# JavaScript in a file whose extension claims otherwise, and the next
		# person to open it would be reading a lie.
		get_filename_component(scene_stem "${scene}" NAME_WE)
		set(staged "${MONO_STAGE_ROOT}/assets/examples/${scene_stem}.js")
		set(staged_map "${MONO_STAGE_ROOT}/assets/examples/${scene_stem}.js.map")

		add_custom_command(
			OUTPUT "${staged}" "${staged_map}"
			COMMAND ${CMAKE_COMMAND} -E make_directory "${MONO_STAGE_ROOT}/assets/examples"
			COMMAND "${_mono_tsc}"
				--target ES2020
				--module ESNext
				--isolatedModules
				--skipLibCheck

				# **`--sourceMap`, so a thrown error can name the line the author
				# wrote.** Stripping types does not preserve line numbers - a
				# multi-line annotation collapses - so without this every stack
				# frame from a `.ts` scene points into generated JavaScript the
				# author never saw. `script::SourceMap` reads the `.js.map` this
				# emits and `JavaScriptRuntime`'s exception text rewrites the
				# frames through it.
				#
				# `--sourceRoot` is deliberately absent: the map's `sources` are
				# relative to the map, the map sits beside the `.js` in the stage,
				# and a root would make them relative to somewhere neither file is.
				--sourceMap

				# **`--ignoreConfig`, because naming a file on the command line
				# and having a `tsconfig.json` present is an error in tsc 7.**
				# The config is for the editor and for `just typecheck`; this
				# invocation is a transpile of one file and wants none of it.
				--ignoreConfig

				# **`--noResolve` and `--noCheck`, because this is a transpile.**
				# Without the config there is no `engine.d.ts` on the include
				# path, so every global the bindings declare - `game`, `Color3`,
				# `Instance` - reports as an unknown name. Those are *type*
				# errors and this step emits regardless, so they were noise on a
				# build that had already succeeded: the stripped file was
				# correct and the log said otherwise.
				#
				# Types are checked by `just typecheck`, against the config, with
				# the declarations loaded. Checking them here too would be two
				# answers to one question, and the worse one.
				--noResolve
				--noCheck
				--outDir "${MONO_STAGE_ROOT}/assets/examples"
				"${scene}"
			DEPENDS "${scene}"
			COMMENT "Transpiling example scene ${scene_name}"
			VERBATIM)

		list(APPEND staged_scenes "${staged}" "${staged_map}")
		continue()
	endif()

	add_custom_command(
		OUTPUT "${MONO_STAGE_ROOT}/assets/examples/${scene_name}"
		COMMAND ${CMAKE_COMMAND} -E make_directory "${MONO_STAGE_ROOT}/assets/examples"
		COMMAND ${CMAKE_COMMAND} -E copy_if_different "${scene}" "${MONO_STAGE_ROOT}/assets/examples/${scene_name}"
		DEPENDS "${scene}"
		COMMENT "Staging example scene ${scene_name}"
		VERBATIM)
	list(APPEND staged_scenes "${MONO_STAGE_ROOT}/assets/examples/${scene_name}")
endforeach()

# The Luau modules a scene owns, staged under that scene's own name.
#
# **One copy in the repository and a copy per scene in the stage**, which is the
# split that keeps both halves honest. `MagicCore` is byte-identical to the Rojo
# project it came from and three scenes use it, so three directories in `lib/`
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
# `<scene>/<library>`, one pair per line. A scene that needs no modules is
# absent, which is every scene but three.
set(scene_libraries
	Magic/MagicCore
	Magic/MagicRuntime
	Magic/TerrainCore
	Magic/TerrainRuntime
	MagicTests/MagicCore
	MagicTests/MagicTests
	MagicTests/TerrainCore
	Libraries/MagicCore
	Libraries/TerrainCore)

foreach(pair IN LISTS scene_libraries)
	string(REPLACE "/" ";" parts "${pair}")
	list(GET parts 0 scene_name)
	list(GET parts 1 library_name)

	file(GLOB_RECURSE library_files CONFIGURE_DEPENDS
		"${CMAKE_CURRENT_SOURCE_DIR}/lib/${library_name}/*.luau")

	if(NOT library_files)
		message(FATAL_ERROR "examples: ${scene_name} names lib/${library_name}, which holds no .luau files")
	endif()

	foreach(library IN LISTS library_files)
		file(RELATIVE_PATH library_relative "${CMAKE_CURRENT_SOURCE_DIR}/lib" "${library}")
		set(library_staged "${MONO_STAGE_ROOT}/assets/examples/${scene_name}/${library_relative}")
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

if(staged_scenes)
	add_custom_target(example_scenes ALL DEPENDS ${staged_scenes})
	add_dependencies(engine_examples example_scenes)
endif()
