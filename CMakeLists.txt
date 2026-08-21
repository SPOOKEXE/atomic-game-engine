# atomic-game-engine
#
# This file holds options, tiers and add_subdirectory. Nothing compiles here.
# Where a file goes and what it may depend on is repo_layout.md; how a module is
# declared is mono.build/MonoLibrary.cmake.

cmake_minimum_required(VERSION 3.24)

# The version, from the one file that holds it.
#
# `VERSION` in the repository root is the source of truth, and everything else
# derives: `project(VERSION)` below, `engine::core::Version()` and therefore
# every program's `--version`, the names the release workflow gives its
# artifacts, and the tag it refuses to publish when the two disagree. A number
# spelled in two places is a number that will be bumped in one of them.
#
# Read here rather than passed to `project()` from a variable somebody sets,
# because `project()` cannot read a file and this has to run before it.
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" MONO_VERSION LIMIT_COUNT 1)
string(STRIP "${MONO_VERSION}" MONO_VERSION)
if(NOT MONO_VERSION MATCHES "^[0-9]+\\.[0-9]+\\.[0-9]+$")
	message(FATAL_ERROR
		"VERSION holds '${MONO_VERSION}', which is not major.minor.patch.\n"
		"  docs/RELEASING.md is what that file means.")
endif()

project(atomic
	VERSION ${MONO_VERSION}
	DESCRIPTION "A 3D game engine for Roblox developers"
	LANGUAGES C CXX)

# Editing VERSION has to re-run the configure, or `project(VERSION)` keeps the
# number from whenever the cache was written and `--version` reports a build
# that no longer exists. Only settable after project().
set_property(DIRECTORY APPEND
	PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/VERSION")

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
# binaries nobody times is a longer build for nothing - `just bench` turns it on
# for the preset it uses.
option(MONO_BUILD_BENCH  "Build the benchmark binaries" OFF)
option(MONO_TRACY        "Compile in the Tracy client" ON)
option(MONO_WERROR       "Treat warnings as errors"    OFF)

# The heap profiler's allocator hooks - core/HeapProfile.hpp.
#
# On by default and off in `release`, which is the opposite of how the two
# optimisation-ish switches above are arranged, and deliberately: a developer
# build should be able to answer "what is holding this memory" without being
# reconfigured first, and a shipped build should not pay a 32-byte header on
# every allocation in the process.
#
# **It cannot be a runtime switch.** A block allocated with no header and freed
# through the tracking `operator delete` would have four words of somebody
# else's memory read as its header, so the decision is made once for a whole
# program. `bench` turns it back on over `release`, because the runaway
# benchmark is the one measurement that needs both the optimiser and the hooks.
option(MONO_HEAP_PROFILE "Track every allocation and attribute it to a tag" ON)

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
# On by default because it removes glslc from the prerequisite list - a fresh
# clone needs CMake, Ninja and a C++ compiler and nothing else - and because a
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
		# in 196.38 against 85.63 - 2.4x, on the loop this engine spends most of
		# its time in. It was invisible for as long as every measurement was
		# taken at the same level as every other, and it was found sideways,
		# while disproving v0.4's vectorisable-layout item: a twelve-byte
		# component has no padding, so -O3's vectoriser fills every lane, and
		# padding it to sixteen - which that item proposed - puts a dead lane in
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

# The compiler cache, if there is one.
#
# **The largest single build-time win available, measured rather than assumed.**
# A clean build of the `dev` preset against an empty cache is 3508 CPU-seconds;
# the same build in the same directory against a warm one is **197**, on 1894 of
# 1894 direct hits. **A factor of eighteen**, for a tool nobody has to think
# about again. Wall clock fell from 186 s to 21.5 s across those runs, but the
# machine was differently loaded and CPU-seconds is the figure that does not
# depend on that. `docs/ARCH_REVIEW.md` §E carries the rest.
#
# **Set before the first `add_subdirectory` on purpose.** A compiler launcher is
# read when a target is created, so a line further down would cover the engine
# and miss the vendor tree - which is 2373 of 4206 CPU-seconds and therefore
# most of what a cache is for.
#
# **`CCACHE_SLOPPINESS`, because otherwise a sixth of the build is skipped
# silently.** SDL and glslang both call `target_precompile_headers` in their own
# CMake, and ccache refuses to cache a compile that uses one unless sloppiness
# says it may - 331 of 1896 calls, reported as "Could not use precompiled
# header" and easy to never notice. Setting it took the uncacheable count to
# zero and a warm build from 310 CPU-seconds to 197, and all 43 suites pass on a
# build where every object came out of the cache.
#
# `time_macros` is the half of that setting worth justifying, and it is safe
# here for a checkable reason: no `__DATE__` or `__TIME__` appears anywhere in
# this repository, first-party or in the vendored sources of the two targets
# that carry a PCH. There is no timestamp to go stale.
#
# Two calls per build still report as errors, and they are the two PCH
# *generation* steps - ccache cannot parse a header as a source, so it passes
# them to the compiler untouched. Correct, and 0.11% of the build.
#
# Passed through `cmake -E env` rather than left to each developer's
# `ccache.conf`, so the setting travels with the build. A machine-local config
# would make the cache behave differently depending on who ran it, which is the
# third category of rule `AGENTS.md` rule 6 refuses to allow. `sccache` reads no
# such variable and is wrapped plainly.
#
# **Absent is not an error.** Nobody should have to install a tool to build this
# repository, and a configure that failed on a missing cache would be a worse
# trade than a slower build. But absence is *reported*, because a cache that
# quietly stopped being used is indistinguishable from one that never was - the
# same argument `just typecheck` makes about a check that passes by having
# nothing to do.
option(MONO_CCACHE "Use ccache or sccache when one is installed" ON)

if(MONO_CCACHE)
	find_program(MONO_COMPILER_CACHE NAMES ccache sccache)
	if(MONO_COMPILER_CACHE)
		if(MONO_COMPILER_CACHE MATCHES "ccache")
			set(MONO_CACHE_LAUNCHER
				"${CMAKE_COMMAND}" -E env
				"CCACHE_SLOPPINESS=pch_defines,time_macros"
				"${MONO_COMPILER_CACHE}")
		else()
			set(MONO_CACHE_LAUNCHER "${MONO_COMPILER_CACHE}")
		endif()

		set(CMAKE_C_COMPILER_LAUNCHER   ${MONO_CACHE_LAUNCHER})
		set(CMAKE_CXX_COMPILER_LAUNCHER ${MONO_CACHE_LAUNCHER})
		message(STATUS "compiler cache: ${MONO_COMPILER_CACHE}")
	else()
		# Named rather than hinted at, because "install ccache" is not obvious
		# to somebody who has never seen a build take three minutes without one.
		message(STATUS
			"compiler cache: none found. `sudo apt install ccache` (or `brew install ccache`) "
			"is worth roughly 150 s of a 172.6 s clean build. Configure with -DMONO_CCACHE=OFF "
			"to stop asking.")
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

	# **A macOS client was stopped here and at v0.15 is warned instead.** The
	# refusal was right while this build produced SPIR-V and nothing else: SDL's
	# Metal backend takes MSL or a metallib and never SPIR-V, so a client
	# configured, compiled, linked, staged, started and died inside
	# `SDL_CreateGPUDevice`, having been knowable before the first object.
	#
	# What changed is that the translation runs. `mono.tools/shadercross` writes
	# an `.msl` beside every `.spv` the build compiles, `Engine::msl` does the
	# same for a `ShaderScript` inside the running client, `just shader-check`
	# reads every emitted module back against the SPIR-V it came from, and the
	# format handed to SDL is asked of the device rather than written down.
	#
	# **A warning that can fire is worth more than a refusal that cannot.**
	# `docs/DEFERRED.md` D00001's reopen trigger is somebody configuring a client
	# on Darwin, and a `FATAL_ERROR` there yields no information - the first
	# person to try is now the only source of the answers that are left, so this
	# asks them for the right ones instead of turning them away.
	if(APPLE)
		message(WARNING
			"No client has ever been built or run on macOS. This configures, and "
			"beyond that nothing is known.\n"
			"  Every shader is translated to MSL during the build and checked by "
			"`just shader-check` - but no Metal compiler and no Metal device has "
			"seen one, because neither exists on the machines this was written on.\n"
			"  If it fails, the three guesses in the order they were made: the "
			"argument-buffer default against what SDL's Metal backend binds; the "
			"depth range, which is the classic difference and is silent; and a "
			"ShaderScript, which goes through the runtime translator nothing on "
			"Linux can exercise end to end.\n"
			"  docs/DEFERRED.md D00001 is the entry. Say what happened, either way.")
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
# them, and each mono.X folder carries its own tests/ - there is no central
# test directory.
add_subdirectory(mono.engine)

# Finding a peer, and being findable. Above `Engine::net` and below every
# program: the client, the server, the studio and the content origin all import
# it, and none of them is a layer of the engine - which is why it is a member
# beside them rather than a module inside `mono.engine`.
#
# Unconditional, like `mono.engine`. It is `shared` tier and links `core`, `net`
# and Crypto++, so a `server` or `cdn` preset pays a static library holding a
# socket and an HMAC and nothing that wants a window.
add_subdirectory(mono.network)

# What a person's Discord profile says while an Atomic program is running. Above
# `Engine::core` and below every program: the studio, the client, the server and
# the content origin each own one, and none of them is a layer of the engine -
# which is why it is a member beside them rather than a module inside
# `mono.engine`.
#
# Unconditional, for `mono.network`'s reason. It is `shared` tier and links
# `core` and a header-only JSON parser, so a `server` or `cdn` preset pays a
# static library holding a unix socket and a string formatter and nothing that
# wants a window.
add_subdirectory(mono.discord)

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
	add_subdirectory(mono.unified_tests)

	# The editor. Needs the renderer to draw with and the server library to host
	# with, so like the harness above it configures only where both exist -
	# under the `server` preset there is nothing here to build.
	add_subdirectory(mono.studio)
endif()

# The front door. `MONO_BUILD_CLIENT` alone, and outside the block above on
# purpose: it links a renderer and Dear ImGui but neither the client library nor
# the server one, so it needs a graphics stack and nothing else. A tree with a
# client and no server still gets a launcher - with the Host mode greyed out and
# saying why, which is what `Descriptions::Failure` is for.
if(MONO_BUILD_CLIENT)
	add_subdirectory(mono.launcher)
endif()

# The content origin. `shared` tier rather than `server`, so the tier rule
# itself - not a guard here - is what keeps a graphics stack off its link line.
# repo_layout.md §11.
#
# **The library is built whenever the server is, and the program is what
# MONO_BUILD_CDN gates.** Since v0.9 a server can attach an origin and serve its
# own disk - CDN.md §6's local store - so `Mono::cdn` is on `mono.server`'s link
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

# And so the shader stage can be compared with the modules that still own
# shaders. Here rather than in `mono_add_library` because the case it catches is
# a module that owns *none* - which is a module `_mono_add_shaders` returns early
# for and therefore never revisits.
mono_prune_shader_stage()

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
