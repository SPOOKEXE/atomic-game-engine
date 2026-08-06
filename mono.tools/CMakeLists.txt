# Developer-facing tools; nothing here ships in a game binary.

add_subdirectory(testrunner)

# Benchmark runner, enabled only for benchmark builds.
if(MONO_BUILD_BENCH)
	add_subdirectory(benchrunner)
endif()

# Documentation and binding tools.
add_subdirectory(bindings)
add_subdirectory(docgen)

# Script type checker over generated declarations.
add_subdirectory(scriptcheck)

# MCP stdio-to-control-port bridge.
add_subdirectory(mcpbridge)

# Line counter.
add_subdirectory(linecount)

# Asset compiler; shared-tier and device-free.
add_subdirectory(assetc)

# The tier rule is enforced at configure time; this compares the graph with its
# checked-in architectural expectation.
if(MONO_BUILD_TESTS)
	add_test(
		NAME architecture
		COMMAND "${CMAKE_COMMAND}"
			-DGRAPH=${CMAKE_BINARY_DIR}/target-graph.json
			-DEXPECTED=${CMAKE_CURRENT_SOURCE_DIR}/architecture/expected_graph.json
			-P "${CMAKE_CURRENT_SOURCE_DIR}/architecture/CheckTargetGraph.cmake"
	)
endif()
