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
	Engine::gui
	Engine::scene

	# **Because `effects` registers three more classes a script can name.**
	# `ParticleEmitter`, `Beam` and `Trail` are as much of the scripting surface
	# as `Part` is, and the same argument the `script` row below makes applies
	# unchanged: a manifest generated without them passes its own drift check
	# while describing less than the tree has.
	Engine::effects

	# **Because `script` registers classes a script can name.** `Script`,
	# `LocalScript` and `LuaSourceContainer` are as much of the scripting surface
	# as `Part` is, and a manifest generated without them would pass its own
	# drift check while describing less than half the tree — which is the one
	# failure mode this tool exists to make impossible.
	Engine::script)
target_compile_options(bindings PRIVATE ${MONO_COMPILE_OPTIONS})

set_target_properties(bindings PROPERTIES
	RUNTIME_OUTPUT_DIRECTORY "${MONO_STAGE_ROOT}/tools")
