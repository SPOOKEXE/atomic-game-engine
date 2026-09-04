# atomic-game-engine
#
# This file holds options, tiers and add_subdirectory. Nothing compiles here.
# Where a file goes and what it may depend on is docs/CODE_ARCH.md; how a module
# is declared is mono.build/MonoLibrary.cmake.

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

# The severity a build compiles log statements in at.
#
# **A statement below this floor is removed by the preprocessor, not skipped at
# runtime.** `ENGINE_TRACE` under a `debug` floor expands to a `sizeof` of a
# call that never happens: the arguments are still type-checked and the format
# string is still verified, and no instruction is emitted - so the eight trace
# sites in the tree cost a shipped build nothing at all, and neither does the
# ninth somebody adds in a loop.
#
# `trace` everywhere except `release` and `bench`, which compile at `debug`.
# Not `info`, because a release build somebody is debugging in the field should
# still be able to answer `--log net=debug` without being rebuilt.
#
# Public on `Engine::core`, like `MONO_HEAP_PROFILE` and for a weaker version of
# its reason: the floor only affects call sites, so a mismatch is confusing
# rather than corrupting - but "which statements does this build contain" should
# have one answer per program.
set(MONO_LOG_LEVEL "trace" CACHE STRING "Lowest severity compiled into log statements")
set_property(CACHE MONO_LOG_LEVEL PROPERTY STRINGS trace debug info warning error off)

if(MONO_LOG_LEVEL STREQUAL "trace")
	set(MONO_LOG_LEVEL_NUMBER 0)
elseif(MONO_LOG_LEVEL STREQUAL "debug")
	set(MONO_LOG_LEVEL_NUMBER 1)
elseif(MONO_LOG_LEVEL STREQUAL "info")
	set(MONO_LOG_LEVEL_NUMBER 2)
elseif(MONO_LOG_LEVEL STREQUAL "warning")
	set(MONO_LOG_LEVEL_NUMBER 3)
elseif(MONO_LOG_LEVEL STREQUAL "error")
	set(MONO_LOG_LEVEL_NUMBER 4)
elseif(MONO_LOG_LEVEL STREQUAL "off")
	set(MONO_LOG_LEVEL_NUMBER 5)
else()
	message(FATAL_ERROR
		"MONO_LOG_LEVEL is '${MONO_LOG_LEVEL}'. It is trace, debug, info, warning, error or off.")
endif()

# `ENGINE_ASSERT` - core/Assert.hpp.
#
# On by default and off in `release`, exactly as the heap profiler is arranged
# and for the same argument: a developer build should check its invariants
# without being reconfigured first, and a shipped build should not pay for a
# check on a path that is hot. `ENGINE_UNREACHABLE` and `ENGINE_ENSURE` are
# unaffected - they are compiled in whatever this says, because neither has
# anywhere to carry on to and both are on cold paths.
option(MONO_ASSERTS "Check ENGINE_ASSERT conditions" ON)

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

# What `MONO_OPTIMISE` means, when it is on.
#
# **Three levels, and the middle one exists for the shipped diagnostic build.**
# `release` wants 3 for the reason the branch below argues at length. `dist-dev`
# wants 1: it is the build somebody runs when the shipped one misbehaves, so it
# keeps the heap profiler and the frame pointer, and an optimiser that inlines
# less leaves a stack that a report can name functions from. `-O0` is not the
# alternative - a build nobody can play is a build nobody reports from.
#
# A string rather than a second option, because "which level" has more than two
# answers and a `MONO_OPTIMISE_LIGHTLY` boolean beside `MONO_OPTIMISE` would be
# two switches that can disagree.
set(MONO_OPTIMISE_LEVEL "3" CACHE STRING "Optimisation level for first-party targets when MONO_OPTIMISE is on")
set_property(CACHE MONO_OPTIMISE_LEVEL PROPERTY STRINGS 1 2 3)

# How much debug information first-party objects carry.
#
# **`line-tables` is `-g1`, and it is a third of the compile off the heaviest
# translation units for something no preset that sets it was using.** Measured
# on this machine against the eight heaviest first-party sources, `release`
# flags, ccache bypassed, best of three, at a load average of 11:
#
#   | debug info | CPU     | objects |
#   |------------|---------|---------|
#   | `-g` full  | 60.3 s  | 96.2 MB |
#   | `-g1`      | 41.6 s  | 20.6 MB |
#   | `-g0`      | 40.4 s  |  6.0 MB |
#
# **The third row is why there is no `none`.** Dropping debug information
# altogether buys 1.2 CPU-seconds more, 2.8%, and costs every backtrace its
# file and line. `-g1` keeps function descriptions and the line-number table
# and drops only the local-variable and type information a live debugger needs,
# so a core file from a shipped build still symbolicates.
#
# **`full` in `dev`, and that is the whole of the preset split.** `dev` is where
# a debugger is attached and where `-g1` would take away exactly what a debugger
# reads; it is also `-O0`, where the same measurement is 15% rather than 31%. So
# `release` and `ci` set `line-tables` - one is shipped and one is read as a log
# - and `dist-dev` sets it back to `full`, because that preset exists to be the
# build somebody diagnoses.
#
# **Nothing here on MSVC, and not by oversight.** MSVC has no line-tables-only
# mode, so there is no flag for the middle value to map to. The related MSVC
# knob is a different one: ccache caches an MSVC compile only where debug info
# is `/Z7` rather than `/Zi`, and `.github/workflows/build.yml` records that the
# Windows build compiles and has never shipped - so that flag belongs to
# whoever first ships it, beside the cache they turn on at the same time.
#
# Applied through MONO_COMPILE_OPTIONS below, which is first-party only, so
# vendored code keeps whatever the build type gave it. Its `-g1` lands after the
# build type's `-g` on the command line and the last one wins.
# Compile each first-party target as a handful of concatenated translation units
# rather than one per source file.
#
# **The largest remaining build-time lever, and it is a trade rather than a
# win.** A unity build pays the header cost once per batch instead of once per
# file, and 72% of this repository's first-party compile cost is the frontend.
# What it costs back is incremental rebuild: touching one file rebuilds its
# whole batch, so a preset a developer edits in should not have it. Hence
# `release` and `ci`, which build everything once, and not `dev`, `server` or
# `cdn`, which are the presets an incremental rebuild happens in.
#
# Measured on this machine over the 45 library targets, `Debug` flags, ccache
# bypassed, twelve concurrent compiles at a load average of 15 to 22:
#
#   |                     | units | wall   | CPU     |
#   |---------------------|-------|--------|---------|
#   | one file per unit   |   469 | 49.3 s | 486.1 s |
#   | eight files per unit|    81 | 22.3 s | 201.1 s |
#
# **59% off CPU and 55% off wall.** The wall figure is the one that depends on
# the machine: 81 units across 24 cores leaves a longer tail than 469 do, and an
# earlier run at ten concurrent compiles gave 36% of CPU rather than 59%. CPU is
# what a CI runner with two cores actually pays.
#
# **It also changes what a translation unit is, which is a behaviour change in
# exactly one way.** A `static` or anonymous-namespace name that was private to
# one file becomes visible to the seven beside it, so two files in a batch that
# each declared a local `Trimmed` now declare it twice. Every such collision in
# this tree was resolved by renaming one of the two, never by making something
# non-static: the whole point of the name being private is that nothing outside
# the file may reach it, and widening the linkage to satisfy the build would
# trade a compile error for a linker one nobody would look for.
option(MONO_UNITY_BUILD "Compile first-party targets as concatenated translation units" OFF)

# How many source files land in one concatenated unit.
#
# Eight is CMake's own default and is kept. A larger batch shares more headers
# and costs more memory per compiler process, and it also coarsens the rebuild -
# a batch is the unit that is redone when one file in it changes.
set(MONO_UNITY_BATCH "8" CACHE STRING "Source files per concatenated translation unit")

# A precompiled header of the standard library, for the presets that do not get
# a unity build.
#
# **The same job as the unity build and a smaller share of it, so the two do not
# both run.** Measured at 10.4% of first-party compile CPU across three modules,
# against 59% for the unity build measured the same way - which ranks it below
# rather than above, the opposite of the usual intuition about precompiled
# headers and the reason the number was taken rather than assumed.
# `mono.build/MonoLibrary.cmake` carries the table and the header list.
#
# On by default, so `dev`, `server` and `cdn` get it and `release` and `ci` get
# the unity build instead. Nothing to configure per preset: the choice follows
# from `MONO_UNITY_BUILD`.
#
# ccache handles a PCH compile only where `sloppiness` permits it, which the
# launcher below already sets for the two vendored targets that carry one.
option(MONO_PCH "Precompile the standard headers, where there is no unity build" ON)

set(MONO_DEBUG_INFO "full" CACHE STRING "Debug information in first-party objects: full or line-tables")
set_property(CACHE MONO_DEBUG_INFO PROPERTY STRINGS full line-tables)
if(NOT MONO_DEBUG_INFO MATCHES "^(full|line-tables)$")
	message(FATAL_ERROR
		"MONO_DEBUG_INFO is '${MONO_DEBUG_INFO}'. It is full or line-tables.")
endif()

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
# directory that is runnable as it stands.
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
	elseif(NOT MONO_OPTIMISE_LEVEL STREQUAL "3")
		# MSVC has no scale to map a level onto: /O2 is its maximum and the
		# build type already supplies it, so anything below the top is /O1.
		list(APPEND MONO_COMPILE_OPTIONS /O1)
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
		list(APPEND MONO_COMPILE_OPTIONS -O${MONO_OPTIMISE_LEVEL})

		# Kept below the top level, where the point of the build is a report
		# somebody can act on. -O3 omits it and the argument above is about
		# speed; a level chosen for diagnosis is choosing the other side of
		# that trade.
		if(NOT MONO_OPTIMISE_LEVEL STREQUAL "3")
			list(APPEND MONO_COMPILE_OPTIONS -fno-omit-frame-pointer)
		endif()
	endif()

	# Last, so it overrides the build type's own -g. See MONO_DEBUG_INFO above.
	if(MONO_DEBUG_INFO STREQUAL "line-tables")
		list(APPEND MONO_COMPILE_OPTIONS -g1)
	endif()
endif()

# **NOMINMAX tree-wide, because unity builds make per-file guards meaningless.**
# windows.h defines min and max as function-like macros unless NOMINMAX is set
# before its first include, and under MONO_UNITY_BUILD which file includes
# windows.h first is batch order rather than authorship - parallel's
# ThreadAffinity.cpp carried a correct per-file guard and still failed on MSVC
# whenever Capture.cpp landed in the same concatenated unit ahead of it.
# Applied beside MONO_COMPILE_OPTIONS, so first-party targets only; no
# first-party source uses the raw macros.
if(WIN32)
	set(MONO_COMPILE_DEFINITIONS NOMINMAX WIN32_LEAN_AND_MEAN)
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
# `CCACHE_BASEDIR` is the source directory, so that one preset's objects are
# another's cache hits. ccache hashes the working directory by default, and
# every preset builds in a directory of its own - without this, six presets
# store the same vendor translation unit six times and a capped cache spends
# itself evicting entries another preset is about to ask for again.
# Relativised paths are rewritten to the asking directory on a hit, which is
# what keeps debug information and depfiles correct across preset boundaries.
# A different machine checks out elsewhere and hashes differently, so entries
# are shared between presets and never between machines.
#
# `CCACHE_NOHASHDIR=true` beside it, because relativising is not the same as
# ignoring: the working directory still enters the hash as a relative path,
# and every preset builds in a directory of its own. Without this, the base
# directory only shares a cache with a checkout in the same place, and two
# presets in one checkout still miss each other on every translation unit.
# Sources and include lines are absolute and identical between presets here,
# so the directory a build was launched from carries no information the hash
# would miss. The two exceptions keep missing honestly: a preset-specific
# generated `-I` dir names a different directory, and GCC writes
# nondeterministic bytes into every `.gch`, so precompiled-header users never
# share across presets whatever is set here.
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
	# **`NO_CACHE`, and this is the line that stops the whole feature from being
	# a trap.** `find_program` normally writes its answer into `CMakeCache.txt`
	# and never looks again, so the obvious sequence - clone, build, read the
	# message below saying to install ccache, install it, build again - would
	# hand back the cached "not found" and no speedup, with nothing to say why.
	# Re-running the search each configure costs a PATH walk on a step that
	# takes about a second, which is not a price worth a failure mode nobody
	# could diagnose.
	find_program(MONO_COMPILER_CACHE NAMES ccache sccache NO_CACHE)
	if(MONO_COMPILER_CACHE)
		if(MONO_COMPILER_CACHE MATCHES "ccache")
		set(MONO_CACHE_LAUNCHER
			"${CMAKE_COMMAND}" -E env
			"CCACHE_SLOPPINESS=pch_defines,time_macros"
			"CCACHE_BASEDIR=${CMAKE_SOURCE_DIR}"
			"CCACHE_NOHASHDIR=true"
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
			"turns a 3508 CPU-second clean build into 197. Re-run the configure afterwards and "
			"it is picked up; no need to delete anything. Configure with -DMONO_CCACHE=OFF to "
			"stop asking.")
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
#
# **The library is built whenever the server is, and the program is what
# MONO_BUILD_CDN gates.** Since v0.9 a server can attach an origin and serve its
# own disk, the local store, so `Mono::cdn` is on `mono.server`'s link
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
if(MONO_OPTIMISE)
	message(STATUS "  optimised  ON (-O${MONO_OPTIMISE_LEVEL})")
else()
	message(STATUS "  optimised  OFF")
endif()
message(STATUS "  debuginfo  ${MONO_DEBUG_INFO}")
if(MONO_UNITY_BUILD)
	message(STATUS "  headers    unity build, ${MONO_UNITY_BATCH} sources per unit")
elseif(MONO_PCH)
	message(STATUS "  headers    precompiled")
else()
	message(STATUS "  headers    once per source file")
endif()
message(STATUS "")
