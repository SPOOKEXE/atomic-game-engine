# mono.tools — the developer-facing tools.
#
# Nothing here ships in a game binary, and nothing here is Python. The tools are
# C++ where they are programs and CMake where they read the build's own output,
# which keeps the prerequisite list at CMake, Ninja, a C++ compiler and glslc.
#
# Tests live in each tool's own tests/, the same as every other mono.X folder.

add_subdirectory(testrunner)

# The benchmark runner, over the test runner's own library. Declared only when
# there are benchmarks to run: it exists to time them, and a tool built to time
# binaries nobody produced is a longer build for nothing.
if(MONO_BUILD_BENCH)
	add_subdirectory(benchrunner)
endif()

# The documentation filter, and the `docs` target it feeds. Declared always: the
# filter is an ordinary library with ordinary tests, and only the `docs` target
# itself needs Doxygen to be installed.
add_subdirectory(bindings)
add_subdirectory(docgen)

# The script type checker, over the declarations `bindings` generates. Declared
# always and built by `just typecheck`: it is the half of the bindings contract
# that faces the scripts rather than the class table.
add_subdirectory(scriptcheck)

# The line counter. Declared always, for the same reason as docgen: it is an
# ordinary library with ordinary tests over a pure function, and nothing about
# it needs a program to be installed.
add_subdirectory(linecount)

# --- architecture ------------------------------------------------------------
#
# A CMake script rather than a compiled tool: its input is a JSON file CMake
# emitted, CMake has a JSON reader, and this way it runs against a tree that has
# been configured but not yet built.
#
# The tier rule itself is enforced at configure time by mono_check_all_tiers.
# This checks the graph against the checked-in expectation, so that an
# architectural change is a diff somebody reviews.
if(MONO_BUILD_TESTS)
	add_test(
		NAME architecture
		COMMAND "${CMAKE_COMMAND}"
			-DGRAPH=${CMAKE_BINARY_DIR}/target-graph.json
			-DEXPECTED=${CMAKE_CURRENT_SOURCE_DIR}/architecture/expected_graph.json
			-P "${CMAKE_CURRENT_SOURCE_DIR}/architecture/CheckTargetGraph.cmake"
	)
endif()
