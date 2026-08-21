# L11 [client]. A typed node graph, and the ImGui canvas over it.
#
# **No first-party dependency at all, and that is the property worth keeping.**
# `msl` is the other module shaped like this. Nothing here needs `core`: there
# is no `Name` to intern, no `Log` to write and no `Clock` to read, because a
# graph is a document and the things it needs are a string, a hash and a worker.
# Adding `Engine::core` for convenience would put the whole platform layer under
# something that currently compiles against the standard library and a widget
# toolkit, and would buy nothing back.
#
# `client` tier because the canvas is Dear ImGui. The model half genuinely is
# not presentation - `Graph.hpp` and `Types.hpp` know nothing about drawing -
# but splitting it into a `shared` model and a `client` canvas would be two
# modules to serve a server that has never wanted either.
#
# **imgui is VENDOR and not VENDOR_PUBLIC, unlike `ui`.** No public header here
# includes `imgui.h`, and no ImGui type appears in any declaration in
# `include/` - every mention is prose. `Editor.hpp` describes painting with
# `ImDrawList` and exposes none of it; `Types.hpp` carries its own `Colour`
# rather than an `ImVec4` and says why. So the toolkit stays an implementation
# detail, and a consumer that wants ImGui gets it from `Engine::ui`, which is
# where this repository puts it.
mono_add_library(nodegraph
	TIER client
	VENDOR Vendor::imgui
)
