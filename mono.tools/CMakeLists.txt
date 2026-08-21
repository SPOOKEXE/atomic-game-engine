# Developer-facing tools; nothing here ships in a game binary.

add_subdirectory(testrunner)

# Benchmark runner, enabled only for benchmark builds.
if(MONO_BUILD_BENCH)
	add_subdirectory(benchrunner)
endif()

# Documentation and binding tools.
add_subdirectory(bindings)
add_subdirectory(componentdoc)
add_subdirectory(docgen)

# Script type checker over generated declarations.
add_subdirectory(scriptcheck)

# MCP stdio-to-control-port bridge.
add_subdirectory(mcpbridge)

# Brings files into the local content store.
#
# **Client-gated, because it links `shaderc` and `MonoVendor.cmake` configures
# shaderc only when `MONO_BUILD_CLIENT` is on.** Its own `CMakeLists.txt` states
# the decision that put it there - compiling GLSL to SPIR-V at publish time
# belongs in a program that can link both the baker and the origin, and "this
# program no longer builds in a server-only configure. That is correct."
#
# What was missing is this line. The decision was recorded and the target was
# still added unconditionally, so `--preset server` and `--preset cdn` both
# configured a target whose `#include <shaderc/shaderc.hpp>` cannot resolve and
# failed at compile time - a build break rather than the deliberate absence the
# comment describes. Absent is what "does not build there" should look like,
# the same way `mono.studio` is absent above rather than stubbed.
if(MONO_BUILD_CLIENT)
	add_subdirectory(contentimport)
endif()

# Line counter.
add_subdirectory(linecount)

# Many real replication clients against one server.
#
# Unconditional, like `shadercheck` and unlike `contentimport`: it links the
# transport and the replication layer and no device at all, so it builds in a
# `server` preset - which is the preset somebody measuring a server would use.
add_subdirectory(loadtest)

# Compiled SPIR-V against the binding contract every backend derives from it.
#
# Unconditional, unlike `contentimport` above: it reads `.spv` files and links no
# shader compiler, so it builds in a `server` preset that configures none. What
# a server preset does not have is any shader for it to read, and the recipe
# rather than this line is where that is said out loud.
add_subdirectory(shadercheck)

# SPIR-V to MSL, for the shaders the build compiles.
#
# **Client-gated where `shadercheck` above is not, and the pair is the split
# worth seeing together.** Checking a compiled module needs nothing but the
# module; translating one needs SPIRV-Cross, which `MonoVendor.cmake` configures
# only when `MONO_BUILD_CLIENT` is on. So a server preset still checks every
# shader it is given and translates none, which is right - it has none.
if(MONO_BUILD_CLIENT)
	add_subdirectory(shadercross)
endif()

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
