# mono.launcher - the front door. A library plus a thin executable.
#
# **The one thing to notice here is what is absent.** This program starts the
# client, the server, the origin and the editor, and it links none of them. It
# spawns the staged binaries through `Engine::parallel`, which is why it is an
# ordinary `client`-tier target with no `ALLOW_TIER_ESCAPE` - the only program in
# the tree that drives all four and the only one that needed no escape to do it.
#
# Linking them instead would have been the largest escape in the repository, it
# would have been unbuildable under the `server` and `cdn` presets, and a fault
# in any of the four would have taken the launcher with it. See
# `launcher/Supervisor.hpp`.
#
# `client` tier and `MONO_BUILD_CLIENT` only, for `mono.studio`'s reason: it
# links a renderer and Dear ImGui, and under the `server` preset there is no
# graphics stack to link. It is absent there rather than stubbed.

mono_add_library(launcher
	TIER client
	NAMESPACE Mono
	DEPS
		Engine::core

		# L2, `shared`. `parallel::Process` supervises the child and
		# `parallel::Capture` is what asks each program `--describe`. Both are
		# the whole of this program's relationship with the four it starts.
		Engine::parallel

		# L12, `client`. The window, the device and the frame. A launcher draws
		# no world, but `Renderer::Render` is what carries an interface to a
		# swapchain and `ui::Interface` is a `render::FrameOverlayHook`.
		Engine::render

		# L12, `client`. Dear ImGui, and the three widgets that moved here at
		# v0.18 so that this program and the editor share one file browser
		# rather than owning one each - `ui::Browse`, `ui::TextField` and
		# `ui::FilePrompt`.
		Engine::ui

	# The window and the event pump. Same forward-declared use the renderer
	# makes: `SDL_Window` is a forward declaration in `Launcher.hpp` and no SDL
	# type reaches any other public header here.
	VENDOR SDL3::SDL3

	# Reading one `--describe` object. **Private**, because no launcher header
	# names an nlohmann type - `Description.hpp` is plain structs, and the
	# parser lives in the one `.cpp` that needs it. The emitting half is
	# hand-written in `core::Arguments` so that L0 stays free of a JSON library.
	VENDOR Vendor::json
)

mono_add_program(launcher
	TIER client
	SOURCE app/main.cpp
	DEPS
		Mono::launcher
		Engine::core
)
