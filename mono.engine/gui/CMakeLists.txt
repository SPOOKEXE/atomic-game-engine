# L7. What a 2D thing in a game *is* - the widget set a game builds its own
# interface out of.
#
# `shared`, and that is the load-bearing half. A server authors a `ScreenGui`
# and replicates it, so a type only a client could name would be a type only a
# client could build one out of. That is `scene`'s argument for `Part` and it is
# the same one, one dimension down.
#
# **Not `mono.engine/ui`.** That module is Dear ImGui and is the *editor's*
# toolkit at L12 `client`; this is the engine's own and nothing a game ships
# touches that one. They coexist deliberately for a version - `ui/AGENTS.md`
# says why - and the one thing they share is the glyph atlas, which lives on the
# client side of the seam because that is where a rasteriser can exist.
#
# No vendor and no `scene`. Layout is arithmetic over `core/types`, and the one
# thing that looked like a reason to link `scene` - `NormalId` for a surface
# gui - is a six-member enum registered by name. Both modules are `shared`, so
# the tier check would not have caught the edge; AGENTS.md is where it is
# refused.
mono_add_library(gui
	TIER shared
	DEPS
		Engine::core
		Engine::ecs
)
