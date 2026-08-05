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
