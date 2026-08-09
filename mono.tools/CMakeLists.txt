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

# Brings files into the local content store.
#
# **Client-gated, because it links `shaderc` and `MonoVendor.cmake` configures
# shaderc only when `MONO_BUILD_CLIENT` is on.** Its own `CMakeLists.txt` states
# the decision that put it there — compiling GLSL to SPIR-V at publish time
# belongs in a program that can link both the baker and the origin, and "this
# program no longer builds in a server-only configure. That is correct."
#
# What was missing is this line. The decision was recorded and the target was
# still added unconditionally, so `--preset server` and `--preset cdn` both
# configured a target whose `#include <shaderc/shaderc.hpp>` cannot resolve and
# failed at compile time — a build break rather than the deliberate absence the
# comment describes. Absent is what "does not build there" should look like,
# the same way `mono.studio` is absent above rather than stubbed.
if(MONO_BUILD_CLIENT)
	add_subdirectory(contentimport)
endif()

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
