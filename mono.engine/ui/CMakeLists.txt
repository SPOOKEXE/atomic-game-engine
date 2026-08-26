# L12 [client]. The editor's toolkit, and the target that makes Dear ImGui
# public. `Mono::nodegraph`, over in `mono.studio/`, vendors it too but
# privately - no public header there names an ImGui type, so this is still the
# only place a consumer gets `<imgui.h>` from.
#
# **`render` deliberately does not depend on this and never should.** The edge
# runs the other way: this module implements `render::FrameOverlayHook`, which
# is two virtual calls and an opaque handle, so a shipped client links a
# renderer that knows an interface layer *could* exist and pays nothing for the
# one it does not have. Reversing that edge would put an atlas, three pipelines
# and a font into every game binary to be initialised and never drawn.
#
# imgui is VENDOR_PUBLIC because a consumer builds panels: `mono.studio` calls
# `ImGui::Begin` directly, which is the whole point of vendoring a widget
# toolkit rather than wrapping one. Wrapping every widget this editor needs
# would be a second UI library maintained by us, which is the thing vendoring
# was supposed to avoid.

mono_add_library(ui
	TIER client
	DEPS
		Engine::core

		# L12, `client`. `render::FrameOverlayHook` is the base class this
		# module implements and `render::Renderer::Backend` is where the device
		# and the swapchain format come from. Sideways within a layer rather
		# than upward, and the tier check agrees,
		# because both are `client`.
		Engine::render

		# L7, `shared`, added at v0.8 for `GuiPainter`. **Downward, and the
		# direction is the whole of why it is allowed**: `gui` is the engine's
		# own widget tree and knows nothing about this module, exactly as
		# `render` knows nothing about it. What arrives here is a flat
		# `gui::DrawList` - no store, no tree, no class table - which is what
		# lets a second backend draw the same list without either learning
		# about the other.
		#
		# It does not make this module a game's UI layer. `ui/AGENTS.md`'s rule
		# stands: nothing in `mono.engine` may depend on `ui`, and `mono.client`
		# still does not link it.
		Engine::gui

	# SDL_Window and SDL_Event appear in the public header as forward
	# declarations only, exactly as `render` declares SDL_Window. The GPU types
	# are behind the pimpl.
	VENDOR SDL3::SDL3

	# Public: `Interface.hpp` exposes no imgui type, but `mono.studio` includes
	# <imgui.h> to build its panels and gets it from here rather than from a
	# second declaration of the same target.
	VENDOR_PUBLIC Vendor::imgui
)
