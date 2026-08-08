# L10. A pipeline drawn as a node canvas — the Render Pipeline and Assets
# Pipeline widgets' tree, without the panel around it.
#
# **Here rather than in `graph`, `gui` or `studio`, and each refusal is the
# point.** A widget builds `gui` instances from a `graph` layout, so it needs
# both, and neither is a sensible home:
#
# - Not `graph`. It is `shared` and links no UI; giving it `gui` would put
#   presentation in the module a headless host uses for culling.
# - Not `gui`. It would have to link `graph` and `bake` to know what a pipeline
#   is, which is a layout library growing opinions about render passes.
# - Not `studio`. That is where the *panel* belongs — docking, menus, input —
#   and it is the one module a test cannot exercise. Everything below is
#   instance construction over an `ecs::Store`, which is exactly the part that
#   can be asserted against, so it is the part that must not live there.
#
# That is `scene::OrderScene`'s argument one layer up: a renderer is the one
# module a test cannot exercise, so the counts it hands to a draw call are the
# last place they should be computed.
#
# `shared`, because none of it touches a device — it writes components, and a
# headless run lays them out exactly as a windowed one does.
mono_add_library(nodeview
	TIER shared
	DEPS
		Engine::core

		# L3. A canvas is instances in a store.
		Engine::ecs

		# L7. The widget set the canvas is built out of.
		Engine::gui

		# L9. What a pipeline is, and where `PipelineView` says its nodes go.
		Engine::graph

		# L9. The other pipeline. **Safe here and not in `game`**: `bake` carries
		# the image and model decoders, so the save format depending on it would
		# pull them into `server` — see the v0.11 roadmap line. Nothing links
		# `nodeview` but an editor, which decodes assets anyway.
		Engine::bake
)
