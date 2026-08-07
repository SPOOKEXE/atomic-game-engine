# mono.studio — the editor. A library plus a thin executable.
#
# **The second declared user of the tier escape D00008 reserved**, and the first
# one that is a product rather than a diagnostic. A `client`-tier target may not
# link `Mono::server`; this one must, because "Play" hosts a server and draws its
# world in one process — which is precisely the arrangement the rule exists to
# make deliberate rather than accidental.
#
# `mono.unified_server_client` took the escape first and its CMakeLists explains
# why an escape with a user is different from one without. This is the second,
# and both are named rather than inferred.
#
# Configured only where both halves exist, for the same reason the unified
# harness is: under the `server` preset there is no renderer to link and this is
# absent rather than stubbed.

mono_add_library(studio
	TIER client
	NAMESPACE Mono
	DEPS
		Engine::core
		Engine::parallel
		Engine::ecs
		Engine::world
		Engine::scene
		Engine::script

		# L13, `shared`. The Model Context Protocol surface: the socket, the
		# handshake and the tools any program with worlds can answer. This
		# module adds the editor's own on top.
		Engine::control

		# L10, `shared`. What a game file is. The studio writes one and the
		# server and the client read one, which is why the format is a module
		# rather than something this program owns.
		Engine::game

		# The example scenes, for `ExamplePath` alone — a new place comes up
		# with the mirror script in it, and finding that file is the one thing
		# this needs. **Not `LoadScene`**: the editor installs the example as a
		# `Script` instance and lets Play run it, rather than building its
		# geometry behind an author's back.
		Engine::examples

		# L12, `client`. The renderer, and the interface layer that records into
		# its frames. `Engine::ui` is where Dear ImGui lives; nothing else in
		# the repository links it.
		Engine::render
		Engine::ui

		# L10, `shared`. The content store this editor manages and lists.
		# **Named here rather than taken transitively through `Mono::server`**,
		# which is where it was arriving from: a dependency a file includes is
		# one its build file should say out loud, and `src/Assets.cpp` includes
		# `cdn/LocalStore.hpp` directly.
		Mono::cdn

		# The baker, so the asset picker can turn a raw file into something a
		# runtime reads without republishing a store. **The same composition
		# `contentimport` makes**, and for the same reason: baking cannot live in
		# `Mono::cdn` — its short link row is the point of that member and a
		# baker is exactly the interpretation an origin must not do — so it
		# happens in the programs that can link both.
		Tool::assetc

		# L9, `shared`. Image decoding, for the asset previews.
		#
		# **A decoder in an editor is not the thing `assets::Texture` argues
		# against.** That argument is about a *runtime*: a shipped game must not
		# carry a Huffman tree to read a PNG, because publishing already turned
		# it into pixels. This is authoring software showing somebody a picture
		# of a file they have not published yet — the same reason `assetc` links
		# it — and the alternative is a store browser whose rows are all blank
		# until a publish has happened.
		Engine::bake

		# L7, `shared`. The engine's own widget tree, which the viewport panels
		# compile and paint over the world image. Named here rather than taken
		# transitively through `Engine::ui`, because this target includes
		# `engine/gui/Compile.hpp` in its own header — a dependency a file
		# includes is one its build file should say out loud.
		Engine::gui

		# The client's presentation seam: `client::InstallPresentation` and
		# `client::DrawList`. **Reused rather than reimplemented** — a second
		# draw-list collector would be a second answer to what a world hands its
		# renderer, and the two would diverge the first time one was optimised.
		Mono::client

		# Hosting a server in this process is what "Play" means. The escape
		# below.
		Mono::server
	ALLOW_TIER_ESCAPE Mono::server
)

mono_add_program(studio
	TIER client
	SOURCE app/main.cpp
	DEPS
		Mono::studio
		Engine::core
)

# --- the studio's own panels, authored in TypeScript --------------------------
#
# **Staged as stripped JavaScript, exactly as the examples are**, and for the
# reason `script/Runtime.hpp` has always given: the engine loads what a toolchain
# emitted and does not compile TypeScript itself. `mono.engine/examples`'s own
# staging carries the argument at length; this is the second caller of it.
#
# These are not yet the editor's panels. `mono.studio` keeps Dear ImGui until the
# engine's tree can draw a property grid, because an editor half on each is two
# widget sets — so what is staged here is the proof that the tree *can*, which is
# what has to be true before any of them move.
file(GLOB studio_panels CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/panels/*.ts")

set(_mono_panel_tsc "${CMAKE_SOURCE_DIR}/node_modules/.bin/tsc")
if(NOT EXISTS "${_mono_panel_tsc}")
	set(_mono_panel_tsc "")
	message(STATUS "studio: no node_modules/.bin/tsc — TypeScript panels will not be staged")
endif()

if(studio_panels AND _mono_panel_tsc)
	set(staged_panels "")
	foreach(panel IN LISTS studio_panels)
		get_filename_component(panel_stem "${panel}" NAME_WE)
		set(staged "${MONO_STAGE_ROOT}/assets/panels/${panel_stem}.js")

		add_custom_command(
			OUTPUT "${staged}"
			COMMAND ${CMAKE_COMMAND} -E make_directory "${MONO_STAGE_ROOT}/assets/panels"
			COMMAND "${_mono_panel_tsc}"
				--target ES2020
				--module ESNext
				--isolatedModules
				--skipLibCheck
				--ignoreConfig
				--noResolve
				--noCheck
				--outDir "${MONO_STAGE_ROOT}/assets/panels"
				"${panel}"
			DEPENDS "${panel}"
			COMMENT "Transpiling studio panel ${panel_stem}.ts"
			VERBATIM)

		list(APPEND staged_panels "${staged}")
	endforeach()

	add_custom_target(studio_typescript_panels ALL DEPENDS ${staged_panels})
	add_dependencies(studio studio_typescript_panels)
endif()
