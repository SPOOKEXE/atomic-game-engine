# atomic-game-engine
#
# This file holds options, tiers and add_subdirectory. Nothing compiles here.
# Where a file goes and what it may depend on is repo_layout.md; how a module is
# declared is mono.build/MonoLibrary.cmake.

cmake_minimum_required(VERSION 3.24)
project(atomic
	VERSION 0.4.0
	DESCRIPTION "A 3D game engine for Roblox developers"
	LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

option(MONO_BUILD_CLIENT "Build the client program"    ON)
option(MONO_BUILD_SERVER "Build the server program"    ON)
option(MONO_BUILD_CDN    "Build the content origin"    ON)
option(MONO_BUILD_TESTS  "Build the test binaries"     ON)
# Off by default. A benchmark is built to be *run*, and a build that produces
# binaries nobody times is a longer build for nothing — `just bench` turns it on
# for the preset it uses.
option(MONO_BUILD_BENCH  "Build the benchmark binaries" OFF)
option(MONO_TRACY        "Compile in the Tracy client" ON)
option(MONO_WERROR       "Treat warnings as errors"    OFF)

# Off by default, and the `release` preset is the one place it goes back on.
#
# First-party code is built at -O0 so that a profile measures what the engine
# does rather than what the optimiser rescued. An algorithm that is only fast
# at -O2 is a slow algorithm with a fast compiler, and the point of measuring
# every day is to find that out on the day it lands.
#
# This applies to our targets only. Vendored SDL and Tracy keep the
# build type's own flags, because unoptimised vendor code would dominate every
# measurement with cost we are not going to change.
option(MONO_OPTIMISE     "Optimise first-party targets" OFF)

# On: compile shaders with the glslc built from mono.vendor/shaderc. Off: find
# one on PATH.
#
# On by default because it removes glslc from the prerequisite list — a fresh
# clone needs CMake, Ninja and a C++ compiler and nothing else — and because a
# pinned compiler makes the SPIR-V reproducible. Two machines with different
# Vulkan SDKs otherwise produce different bytes from the same source, which is
# a difference nobody looks for.
#
# The cost is real: the first configure of a client preset builds glslang and
# SPIRV-Tools. Turn it off for a faster first build if you have glslc already.
option(MONO_VENDORED_GLSLC "Compile shaders with the vendored shaderc" ON)

# Libraries are intermediate; programs are products. Each program stages into a
# directory that is runnable as it stands. repo_layout.md §15.
set(MONO_STAGE_ROOT "${CMAKE_BINARY_DIR}")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${MONO_STAGE_ROOT}/lib")
set(MONO_SHADER_STAGE "${CMAKE_BINARY_DIR}/shaderstage")

# Applied by mono_add_library and mono_add_program, and therefore to first-party
# targets only. target_compile_options land after CMAKE_CXX_FLAGS_<CONFIG> on the
# command line, so the optimisation level chosen here wins over the build type's.
if(MSVC)
	set(MONO_COMPILE_OPTIONS /W4 /permissive-)
	if(MONO_WERROR)
		list(APPEND MONO_COMPILE_OPTIONS /WX)
	endif()
	if(NOT MONO_OPTIMISE)
		list(APPEND MONO_COMPILE_OPTIONS /Od)
	endif()
	# No /O3 exists. /O2 is already MSVC's maximum and the build type supplies
	# it, so the optimised branch below has no MSVC counterpart rather than a
	# missing one.
else()
	set(MONO_COMPILE_OPTIONS -Wall -Wextra -Wpedantic)
	if(MONO_WERROR)
		list(APPEND MONO_COMPILE_OPTIONS -Werror)
	endif()
	if(NOT MONO_OPTIMISE)
		list(APPEND MONO_COMPILE_OPTIONS -O0 -fno-omit-frame-pointer)
	else()
		# -O3, stated here rather than inherited from RelWithDebInfo's -O2.
		#
		# **Measured, and it is not a small difference.** The ECS iteration
		# control runs 100k rows in 38.74 us at -O2 and 16.61 at -O3, and 500k
		# in 196.38 against 85.63 — 2.4x, on the loop this engine spends most of
		# its time in. It was invisible for as long as every measurement was
		# taken at the same level as every other, and it was found sideways,
		# while disproving v0.4's vectorisable-layout item: a twelve-byte
		# component has no padding, so -O3's vectoriser fills every lane, and
		# padding it to sixteen — which that item proposed — puts a dead lane in
		# every vector and made the same loop 2.4x *slower*. The layout was
		# already right; the flag was the thing holding it back.
		#
		# **Why this is safe to turn on here and nowhere else.** -O3 is more
		# aggressive about vectorising and inlining, and the fear worth taking
		# seriously in this repository is floating point: `just determinism`
		# diffs two runs byte for byte and `just replay-check` diffs a replay
		# against the run it replayed, so a reassociated float sum would fail a
		# recipe rather than become a rumour. Both were run against this preset
		# before the flag was committed and both are byte-identical. GCC does
		# not enable -ffast-math or -funsafe-math-optimizations at any -O level,
		# so IEEE semantics are unchanged; the guarantee that moved is none.
		#
		# Note what this does NOT claim: determinism here is same-binary, as
		# ROADMAP.md v0.2 says. Two builds at different optimisation levels are
		# two different binaries and were never promised to agree.
		list(APPEND MONO_COMPILE_OPTIONS -O3)
	endif()
endif()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/mono.build")
include(MonoVendor)

# After MonoVendor, so that glslc_exe exists to be named. Only the presentation
# modules own GLSL, so a server-only configure needs no shader compiler at all.
#
# MONO_GLSLC_DEPENDS is what makes a shader rebuild when the compiler it was
# built with changes. Without it, bumping shaderc leaves every .spv stale.
set(MONO_GLSLC_DEPENDS "")
if(MONO_BUILD_CLIENT)
	if(MONO_VENDORED_GLSLC)
		set(MONO_GLSLC "$<TARGET_FILE:glslc_exe>")
		set(MONO_GLSLC_DEPENDS glslc_exe)
	else()
		find_program(MONO_GLSLC glslc REQUIRED
			DOC "GLSL to SPIR-V compiler, from shaderc")
	endif()
endif()
include(MonoLibrary)

if(MONO_BUILD_TESTS)
	enable_testing()
endif()

# The suite registry, and the two mains over it. `testmain` is behind the tests
# option; the registry itself is not, because a benchmark binary declares suites
# the same way and is discovered by the same listing.
if(MONO_BUILD_TESTS OR MONO_BUILD_BENCH)
	add_subdirectory(mono.build/testmain)
endif()

if(MONO_BUILD_BENCH)
	add_subdirectory(mono.build/benchmain)
endif()

# The engine is libraries and never a product. Programs are thin mains over
# them, and each mono.X folder carries its own tests/ — there is no central
# test directory.
add_subdirectory(mono.engine)

# Finding a peer, and being findable. Above `Engine::net` and below every
# program: the client, the server, the studio and the content origin all import
# it, and none of them is a layer of the engine — which is why it is a member
# beside them rather than a module inside `mono.engine`.
#
# Unconditional, like `mono.engine`. It is `shared` tier and links `core`, `net`
# and Crypto++, so a `server` or `cdn` preset pays a static library holding a
# socket and an HMAC and nothing that wants a window.
add_subdirectory(mono.network)

if(MONO_BUILD_CLIENT)
	add_subdirectory(mono.client)
endif()

if(MONO_BUILD_SERVER)
	add_subdirectory(mono.server)
endif()

# Both halves of replication in one process, with `net` cut out of the middle.
# A diagnostic product: it needs the client's draw seam and the server's world,
# so it configures only where both exist. Under the `server` preset there is no
# client to link and this is absent rather than stubbed.
if(MONO_BUILD_CLIENT AND MONO_BUILD_SERVER)
	add_subdirectory(mono.unified_server_client)

	# The editor. Needs the renderer to draw with and the server library to host
	# with, so like the harness above it configures only where both exist —
	# under the `server` preset there is nothing here to build.
	add_subdirectory(mono.studio)
endif()

# The content origin. `shared` tier rather than `server`, so the tier rule
# itself — not a guard here — is what keeps a graphics stack off its link line.
# repo_layout.md §11.
#
# **The library is built whenever the server is, and the program is what
# MONO_BUILD_CDN gates.** Since v0.9 a server can attach an origin and serve its
# own disk — CDN.md §6's local store — so `Mono::cdn` is on `mono.server`'s link
# line and has to exist under the `server` preset, which sets MONO_BUILD_CDN
# OFF. The alternative was a conditional dependency and an `#ifdef` around the
# attachment, which would make "does this server serve content" a build-time
# question with two code paths, and rule 6 asks for the opposite.
#
# It costs the `server` preset a `shared` library holding a hash, a socket and a
# compressor. It costs the staged tree nothing: the cdn *executable* is still
# gated, so `--preset server` stages no origin binary.
if(MONO_BUILD_CDN OR MONO_BUILD_SERVER)
	add_subdirectory(mono.cdn)
endif()

# The test runner and the architecture check. Built always, not only under
# MONO_BUILD_TESTS: the runner is a tool, and `just test` needs it to exist.
add_subdirectory(mono.tools)

# Every target now exists, so the tier rule can be checked across the whole
# graph rather than only against what had been declared at each point.
mono_check_all_tiers()

# The checked-in expectation mono.tools/architecture/ compares against.
mono_write_target_graph("${CMAKE_BINARY_DIR}/target-graph.json")

message(STATUS "")
message(STATUS "atomic ${PROJECT_VERSION}")
message(STATUS "  stage      ${MONO_STAGE_ROOT}")
message(STATUS "  programs   client=${MONO_BUILD_CLIENT} server=${MONO_BUILD_SERVER} cdn=${MONO_BUILD_CDN}")
message(STATUS "  tracy      ${MONO_TRACY}")
message(STATUS "  tests      ${MONO_BUILD_TESTS}")
message(STATUS "  benchmarks ${MONO_BUILD_BENCH}")
message(STATUS "  optimised  ${MONO_OPTIMISE}")
message(STATUS "")
