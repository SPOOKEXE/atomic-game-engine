# The bindings manifest generator, and the declaration files it emits.
#
# **The first tool that links past `core`.** Every other tool here is a thin
# executable over `core` or over `Tool::testrunner`; this one needs the class
# table populated, which means linking the modules that populate it. A manifest
# generated against an empty table would be a valid file describing nothing, and
# its own drift check would pass forever.
#
# Worth knowing: `expected_graph.json` does **not** carry this row. The graph
# check tracks `mono_add_library` modules and `mono_add_program` programs, and a
# bare `add_executable` tool is neither — `benchrunner` is invisible to it for
# the same reason. So this link row is reviewed here and nowhere else, which is
# an argument for keeping it short rather than for widening it quietly.
add_executable(bindings app/main.cpp)
target_link_libraries(bindings PRIVATE
	Engine::core
	Engine::ecs
	Engine::scene)
target_compile_options(bindings PRIVATE ${MONO_COMPILE_OPTIONS})

set_target_properties(bindings PROPERTIES
	RUNTIME_OUTPUT_DIRECTORY "${MONO_STAGE_ROOT}/tools")
