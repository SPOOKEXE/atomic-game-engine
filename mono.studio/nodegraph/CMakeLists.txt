# The editor's node graph, and the ImGui canvas over it. `client` tier, and no
# layer at all.
#
# **It sits in `mono.studio/` because exactly one thing links it: the editor.**
# It was `mono.engine/nodegraph` from v0.18.0 until v0.19, where a `client`-tier
# target carrying Dear ImGui and four concurrency headers was one of two modules
# in the engine that no program but the studio could reach. Root `AGENTS.md`
# says `mono.engine` ships nothing on its own; a widget library for one editor
# is not a layer of the engine, and the move says so in the tree rather than in
# a comment.
#
# **No layer, and that is the enforcement.** `expected_graph.json` gives it none,
# which puts it in the program band - and `CheckTargetGraph.cmake` refuses any
# edge from something that has a layer to something that has not. So no engine
# module can start linking this without the architecture check naming the edge.
#
# **No first-party dependency at all, and that is the property worth keeping.**
# `Engine::msl` is the other target shaped like this. Nothing here needs `core`:
# there is no `Name` to intern, no `Log` to write and no `Clock` to read, because
# a graph is a document and the things it needs are a string, a hash and a
# worker. Adding `Engine::core` for convenience would put the whole platform
# layer under something that currently compiles against the standard library and
# a widget toolkit, and would buy nothing back.
#
# `client` tier because the canvas is Dear ImGui. The model half genuinely is
# not presentation - `Graph.hpp` and `Types.hpp` know nothing about drawing -
# but splitting it into a `shared` model and a `client` canvas would be two
# targets to serve a server that has never wanted either.
#
# **imgui is VENDOR and not VENDOR_PUBLIC, unlike `ui`.** No public header here
# includes `imgui.h`, and no ImGui type appears in any declaration in
# `include/` - every mention is prose. `Editor.hpp` describes painting with
# `ImDrawList` and exposes none of it; `Types.hpp` carries its own `Colour`
# rather than an `ImVec4` and says why. So the toolkit stays an implementation
# detail, and a consumer that wants ImGui gets it from `Engine::ui`, which is
# where this repository puts it.
#
# `NAMESPACE Mono`, so the alias is `Mono::nodegraph` and reads as a product's
# library rather than a layer of the engine. The headers are `<nodegraph/...>`
# for the same reason: the `engine/` prefix names a role this no longer has.
mono_add_library(nodegraph
	TIER client
	NAMESPACE Mono
	VENDOR Vendor::imgui
)
