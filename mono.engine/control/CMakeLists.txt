# L13. Being asked what this program is doing, and told what to do about it.
#
# **The Model Context Protocol surface, shared by every program that has one.**
# The editor, the server, the client, the unified harness and the content origin
# all answer the same handshake; what differs is which tools each registers.
# Before this was a module the protocol lived in `mono.studio`, and adding it to
# a second program would have meant a second copy of a JSON-RPC implementation -
# which is the duplication the root `AGENTS.md` calls the most expensive kind.
#
# `shared`, and it has to be: a headless server exposes its worlds through this
# and links no client code at all.
#
# L13 because tools may coordinate lower layers, while each optional feature
# carries the includes for the state it exposes. The transport itself stays
# dependency-light enough for a content origin with no worlds.
#
# Each product links the registry, selects an explicit feature list, and opens
# its port only when told to. `SECURITY.md` carries why listening is a decision
# rather than a default.
mono_add_library(control
	TIER shared
	DEPS
		Engine::core
		Engine::parallel
	VENDOR
		# The socket. Private: `Server.hpp` keeps asio behind a pimpl, so nothing
		# that links this module compiles a networking library.
		Vendor::asio
	VENDOR_PUBLIC
		# **Public, and it is the narrowest widening available.** `Surface.hpp`
		# declares a tool as a function returning `nlohmann::json`, so the name
		# has to be visible to every program that registers one. What it costs is
		# `json_fwd.hpp` - a forward declaration and no parser; only this
		# module's own sources include the real header.
		Vendor::json
)

# The surface suite exercises the optional universe feature. Those dependencies
# belong to the test executable rather than to every control consumer.
if(TARGET test_control)
	target_link_libraries(test_control PRIVATE Engine::ecs Engine::world)
endif()

# The checked-in target graph, compiled in.
#
# **The `resources` module's trade, applied to a different file.** A constant
# that lives in one place and is needed in another is read at configure time
# rather than typed again, so the second copy cannot drift. Here the constant is
# the whole of `expected_graph.json` - the modules, their tiers, their layers and
# their exact link sets - and the second place is a tool a model asks "may render
# link script".
#
# **Compiled in rather than opened at run time**, because the program answering
# is a dedicated server or an editor started from wherever somebody was standing.
# A tool that only worked when the current directory was the repository would
# fail exactly when a model most needs it.
set(control_graph "${CMAKE_SOURCE_DIR}/mono.tools/architecture/expected_graph.json")
if(NOT EXISTS "${control_graph}")
	message(FATAL_ERROR
		"control: ${control_graph} is missing.\n"
		"  The module graph tools answer out of that file. If it moved, update "
		"this path and mono.tools/architecture/CheckTargetGraph.cmake's caller "
		"in the same commit.")
endif()

file(READ "${control_graph}" MONO_CONTROL_GRAPH)

# Without this, editing the expectation would leave the compiled copy holding
# whatever the last configure read - the drift this exists to prevent, arriving
# by a different door.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${control_graph}")

configure_file(
	"${CMAKE_CURRENT_SOURCE_DIR}/cmake/GraphData.cpp.in"
	"${CMAKE_CURRENT_BINARY_DIR}/generated/GraphData.cpp"
	@ONLY)

target_sources(engine_control PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated/GraphData.cpp")
