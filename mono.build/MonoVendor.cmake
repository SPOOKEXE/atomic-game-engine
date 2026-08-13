# MonoVendor.cmake — third-party configuration, in one place.
#
# Vendor options are set here rather than in the root CMakeLists so that the
# root file stays what §3 says it is: options, tiers and add_subdirectory.

include_guard(GLOBAL)

set(MONO_VENDOR "${CMAKE_SOURCE_DIR}/mono.vendor")

# --- third-party code is optimised even in a debug build ---------------------
#
# **First-party code builds at -O0 on purpose and third-party code has no such
# reason.** `AGENTS.md` argues the first half: a profile should measure what
# this engine does rather than what the optimiser rescued, and an algorithm that
# is only fast at -O2 is a slow algorithm with a fast compiler. Neither sentence
# is about SDL.
#
# What it cost, measured on the editor: `SDL_SubmitGPUCommandBuffer` is where the
# Vulkan backend turns a recorded command buffer into queue submissions —
# barrier tracking, descriptor and pipeline hashing, the pending-destroy sweep —
# and at -O0 that is the single most expensive thing in a frame. The `submit`
# span read **17 ms in `dev` against a p50 of 0.2 ms in `release`** on the same
# scene and the same machine. An editor nobody can drag a splitter in is one
# nobody develops in, and every `dev` profile taken there says "the renderer is
# slow" about somebody else's `-O0`.
#
# **Set before the first `add_subdirectory` and never restored**, because every
# vendor tree below is added from this file: the variables are inherited by each
# subdirectory scope and first-party targets get their flags from
# `MONO_COMPILE_OPTIONS`, which is applied per target and wins by coming later
# on the command line. Debug information is deliberately kept — a stack through
# SDL should still name its frames.
#
# Off with `-DMONO_OPTIMISE_VENDOR=OFF`, which is what to pass when the thing
# being debugged *is* a vendored library.
option(MONO_OPTIMISE_VENDOR "Build third-party code optimised, whatever the build type" ON)

if(MONO_OPTIMISE_VENDOR AND NOT MSVC)
	set(CMAKE_C_FLAGS_DEBUG "${CMAKE_C_FLAGS_DEBUG} -O2")
	set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} -O2")
elseif(MONO_OPTIMISE_VENDOR AND MSVC)
	# `/Od` is in the Debug defaults and the later flag wins, so this is an
	# append rather than a replacement here too.
	set(CMAKE_C_FLAGS_DEBUG "${CMAKE_C_FLAGS_DEBUG} /O2")
	set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} /O2")
endif()

if(NOT EXISTS "${MONO_VENDOR}/glm/CMakeLists.txt")
	message(FATAL_ERROR
		"mono.vendor/ is empty. Run `just setup`, or:\n"
		"  git submodule update --init --recursive --depth 1")
endif()

# --- SDL3 -------------------------------------------------------------------
# Only when a client-tier program is being built. A server or delivery-service
# configure has to work on a machine with no Vulkan SDK and no SDL development
# packages at all, and the only way to be sure of that is to not configure them.
#
# Shared rather than static, so that a driver update does not mean relinking the
# engine, and because the Android and Apple packaging paths both expect it.
if(MONO_BUILD_CLIENT)
	if(NOT EXISTS "${MONO_VENDOR}/sdl/CMakeLists.txt")
		message(FATAL_ERROR "mono.vendor/sdl is missing. Run `just setup`.")
	endif()

	set(SDL_SHARED      ON  CACHE BOOL "" FORCE)
	set(SDL_STATIC      OFF CACHE BOOL "" FORCE)
	set(SDL_TEST        OFF CACHE BOOL "" FORCE)
	set(SDL_TESTS       OFF CACHE BOOL "" FORCE)
	set(SDL_EXAMPLES    OFF CACHE BOOL "" FORCE)
	set(SDL_INSTALL     OFF CACHE BOOL "" FORCE)
	set(SDL_DISABLE_PCH ON  CACHE BOOL "" FORCE)
	add_subdirectory("${MONO_VENDOR}/sdl" EXCLUDE_FROM_ALL)
endif()

# --- glm --------------------------------------------------------------------
set(GLM_ENABLE_CXX_20 ON CACHE BOOL "" FORCE)
set(GLM_BUILD_TESTS   OFF CACHE BOOL "" FORCE)
set(GLM_BUILD_INSTALL OFF CACHE BOOL "" FORCE)
add_subdirectory("${MONO_VENDOR}/glm" EXCLUDE_FROM_ALL)

# --- spdlog -----------------------------------------------------------------
set(SPDLOG_BUILD_EXAMPLE OFF CACHE BOOL "" FORCE)
set(SPDLOG_INSTALL       OFF CACHE BOOL "" FORCE)
add_subdirectory("${MONO_VENDOR}/spdlog" EXCLUDE_FROM_ALL)

# --- Tracy ------------------------------------------------------------------
# On-demand: the client collects nothing until a profiler attaches, so leaving
# it compiled in for ordinary builds costs almost nothing. Localhost-only,
# because "compiled in by default" and "listening on a socket" must not be the
# same decision.
set(TRACY_ENABLE         ${MONO_TRACY} CACHE BOOL "" FORCE)
set(TRACY_ON_DEMAND      ON  CACHE BOOL "" FORCE)
set(TRACY_ONLY_LOCALHOST ON  CACHE BOOL "" FORCE)
set(TRACY_NO_BROADCAST   ON  CACHE BOOL "" FORCE)
add_subdirectory("${MONO_VENDOR}/tracy" EXCLUDE_FROM_ALL)

# --- asio ---------------------------------------------------------------------
# Standalone asio, not Boost.Asio: the same author, the same code, without
# pulling Boost in for one header set.
#
# Header-only, and it ships no CMakeLists, so the target is declared here. Zero
# configure cost and zero build cost until something links it.
#
# **`asio/include`, not `asio/asio/include`, and that is a Windows fix.**
# Upstream keeps the headers at its repository root, in `include/`. The `asio/`
# directory sitting beside them is a compatibility shim for the older layout,
# and the `include` and `src` entries inside it are symlinks back up at the real
# ones — `git ls-files -s` reports them as mode 120000.
#
# Git on Windows does not create symlinks. `core.symlinks` defaults to off
# there, because creating one needs Developer Mode or an elevated shell, and
# with it off git writes a small text file holding the target path instead. So
# on a stock Windows clone `mono.vendor/asio/asio/include` is a ten-byte *file*,
# the include path resolves to nothing, and every translation unit in `net`
# stops at
#
#   fatal error C1083: Cannot open include file: 'asio/io_context.hpp'
#
# with the header on disk one directory up. Nothing warns first: a SYSTEM
# INTERFACE include directory is not checked for existence at configure time,
# so this surfaces at the first compile and looks like a missing submodule.
#
# Reaching past the shim to the real directory fixes it for everyone and asks
# nothing of anybody's git config. asio and zstd and blake3 are the only vendors
# carrying symlinks at all, and the other two are licence files and CLI test
# fixtures that no include path goes through.
#
# The guard is here for the same reason it is on every other vendor: an
# uninitialised submodule should stop the configure that can explain it rather
# than the compiler, which cannot.
if(NOT EXISTS "${MONO_VENDOR}/asio/include/asio.hpp")
	message(FATAL_ERROR "mono.vendor/asio is missing. Run `just setup`.")
endif()
add_library(vendor_asio INTERFACE)
add_library(Vendor::asio ALIAS vendor_asio)
target_include_directories(vendor_asio SYSTEM INTERFACE "${MONO_VENDOR}/asio/include")
# Without this asio looks for Boost. It is the difference between the two
# distributions and the only reason this is not a bare include path.
target_compile_definitions(vendor_asio INTERFACE ASIO_STANDALONE ASIO_NO_DEPRECATED)
find_package(Threads REQUIRED)
target_link_libraries(vendor_asio INTERFACE Threads::Threads)

# --- nlohmann/json ------------------------------------------------------------
# JSON, for the one thing in this repository that speaks it: `mono.studio`'s
# control server answers Model Context Protocol, which is JSON-RPC 2.0.
#
# Header-only and declared here rather than added as a subdirectory, for asio's
# reason — upstream's CMakeLists builds tests and a package config nobody here
# wants, and the single header is the whole library.
#
# **The single_include copy, not include/.** Upstream ships both; the amalgamated
# one is one file and one include path, and there is nothing to choose between
# them beyond that.
#
# `shared` in engine terms, but only `mono.studio` links it and that is on
# purpose: a wire format is binary, a save file is XML, and JSON belongs to the
# one surface that talks to something outside this repository.
add_library(vendor_json INTERFACE)
add_library(Vendor::json ALIAS vendor_json)
target_include_directories(vendor_json SYSTEM INTERFACE "${MONO_VENDOR}/json/single_include")

# --- toml++ -------------------------------------------------------------------
# TOML, for the one thing in this repository that reads it: `mono.studio`'s Rojo
# sync maps a `*.toml` to a `ModuleScript`, which `rojo.space/docs/v7/sync-details`
# lists beside the `*.json` case. `D00104` carried it as deferred for exactly one
# reason — the mapping was free and the parser was not vendored.
#
# Header-only, MIT, and declared here rather than added as a subdirectory for
# json's reason: upstream's CMakeLists builds tests, examples and an install
# package nobody here wants.
#
# **`include/`, not the amalgamated `toml.hpp` at the repository root**, which is
# the opposite of the choice made for json one section up and is deliberate. The
# amalgamation would put a bare `toml.hpp` on an include path, and a header that
# generic is one any other vendor or system directory can shadow. The modular
# copy is reached as `<toml++/toml.hpp>` and cannot be.
if(NOT EXISTS "${MONO_VENDOR}/tomlplusplus/include/toml++/toml.hpp")
	message(FATAL_ERROR "mono.vendor/tomlplusplus is missing. Run `just setup`.")
endif()
add_library(vendor_tomlplusplus INTERFACE)
add_library(Vendor::tomlplusplus ALIAS vendor_tomlplusplus)
target_include_directories(vendor_tomlplusplus SYSTEM INTERFACE "${MONO_VENDOR}/tomlplusplus/include")
# **Without this the parser throws and `parse_result` is an alias for `table`,**
# so there is no error to read and a malformed file becomes an exception crossing
# a sync that is built to report and carry on. Off makes `parse_result` the
# result type with `error()` on it, which is the same shape `ReadJsonFile` gets
# from nlohmann's `allow_exceptions = false` one function above its caller.
# asio's `ASIO_STANDALONE` is here for the same class of reason.
target_compile_definitions(vendor_tomlplusplus INTERFACE TOML_EXCEPTIONS=0)

# --- minimp3 ------------------------------------------------------------------
# MP3 decoding, for `Engine::audio`. Two headers, no build system, CC0.
#
# **The licence is why this one exists at all.** `audio/AGENTS.md` said MP3 was
# "a vendored codec and a licence decision" and left the gap honest rather than
# listing an extension it could not decode; CC0 is what makes the decision cheap
# — no attribution obligation, no patent grant to read, nothing that reaches a
# shipped game. A codec is usually where that question ends the conversation.
#
# Header-only and declared here for asio's reason: upstream ships a test
# harness, a player and a fuzzer, none of which anybody here wants configured.
#
# **`MINIMP3_ONLY_MP3` and `MINIMP3_NO_SIMD` are not set, and that is
# deliberate.** The first would drop MP1 and MP2 layer support, which costs
# nothing to keep and is what a file with a `.mp3` extension sometimes actually
# is; the second would turn off the SSE and NEON paths that make a five-minute
# track decode in well under a second. `MINIMP3_IMPLEMENTATION` is defined in
# exactly one translation unit — `audio/src/Mp3.cpp` — because these headers are
# a single-header library and defining it twice is a duplicate-symbol link
# error.
#
# `client` in engine terms: `audio` is L12 and only the client links it.
add_library(vendor_minimp3 INTERFACE)
add_library(Vendor::minimp3 ALIAS vendor_minimp3)
target_include_directories(vendor_minimp3 SYSTEM INTERFACE "${MONO_VENDOR}/minimp3")

# --- Dear ImGui -----------------------------------------------------------
# Editor and tooling UI, not game UI. `ui` at L12 is a retained-mode tree that a
# game author builds interfaces with; this is what mono.studio's shell, docking
# and inspectors are made of, and what a debug window would use.
#
# Not the F3/F5 panels. Those draw pixels into a CPU buffer on purpose, so they
# keep working when the renderer is the thing being debugged — see
# mono.engine/render/AGENTS.md.
#
# Client tier: it needs SDL3 and a GPU backend.
if(MONO_BUILD_CLIENT)
	# imgui ships no CMakeLists either, and the backends are deliberately not
	# a library — you compile the two that match your platform. SDL3 for the
	# window and events, SDL3 GPU for drawing, which is the same pair the
	# renderer already uses.
	add_library(vendor_imgui STATIC EXCLUDE_FROM_ALL
		"${MONO_VENDOR}/imgui/imgui.cpp"
		"${MONO_VENDOR}/imgui/imgui_draw.cpp"
		"${MONO_VENDOR}/imgui/imgui_tables.cpp"
		"${MONO_VENDOR}/imgui/imgui_widgets.cpp"
		"${MONO_VENDOR}/imgui/backends/imgui_impl_sdl3.cpp"
		"${MONO_VENDOR}/imgui/backends/imgui_impl_sdlgpu3.cpp"
	)
	add_library(Vendor::imgui ALIAS vendor_imgui)

	# SYSTEM on the includes so that imgui's own warnings are not ours: the
	# `ci` preset builds first-party code with -Werror and a vendored header
	# must never be able to fail it.
	target_include_directories(vendor_imgui SYSTEM PUBLIC
		"${MONO_VENDOR}/imgui"
		"${MONO_VENDOR}/imgui/backends")
	target_link_libraries(vendor_imgui PUBLIC SDL3::SDL3)
	target_compile_features(vendor_imgui PUBLIC cxx_std_20)

	# imgui_demo.cpp is deliberately absent. It is a third of the library by
	# size, it is a showcase rather than a dependency, and adding it later is
	# one line if somebody wants the demo window while building an inspector.
endif()

# --- Catch2 -----------------------------------------------------------------
if(MONO_BUILD_TESTS)
	set(CATCH_INSTALL_DOCS     OFF CACHE BOOL "" FORCE)
	set(CATCH_INSTALL_EXTRAS   OFF CACHE BOOL "" FORCE)
	set(CATCH_BUILD_TESTING    OFF CACHE BOOL "" FORCE)
	add_subdirectory("${MONO_VENDOR}/catch2" EXCLUDE_FROM_ALL)
endif()

# --- shaderc ----------------------------------------------------------------
# GLSL to SPIR-V. Two things come out of it, used at different times, and both
# are wanted:
#
#   glslc      the command-line driver. Compiles the engine's own GLSL during
#              the build, replacing the system glslc that was a prerequisite
#              until now. A shader shipped with the engine fails the build
#              rather than the frame.
#   libshaderc the library, linked into the client by `render`. This is the
#              one that matters for the graph renderer: a `ShaderScript` whose
#              revision changed, a swapped antialias pass, a shader permutation
#              — none of them exist at build time, so none can be compiled
#              ahead of it.
#
# Keeping both is deliberate rather than redundant. Build-time compilation
# fails the build on a bad built-in shader, which is where that belongs;
# runtime compilation returns an error string to whoever authored the shader,
# which is where *that* belongs.
#
# Client-tier, and gated on nothing else. libshaderc is linked into the client
# for the runtime half above, so MONO_VENDORED_GLSLC only decides which glslc
# compiles the built-in shaders — not whether shaderc is configured at all.
#
# A server configure still gets none of it: no server-tier target owns GLSL or
# compiles one.
if(MONO_BUILD_CLIENT)
	if(NOT EXISTS "${MONO_VENDOR}/shaderc/CMakeLists.txt")
		message(FATAL_ERROR "mono.vendor/shaderc is missing. Run `just setup`.")
	endif()
	# shaderc pins glslang, SPIRV-Tools and SPIRV-Headers in its own DEPS file
	# rather than as submodules, so `git submodule update` clones shaderc and
	# leaves third_party/ empty. `just setup` runs git-sync-deps; this is here
	# so a half-set-up tree says so instead of failing further in.
	if(NOT EXISTS "${MONO_VENDOR}/shaderc/third_party/glslang/CMakeLists.txt")
		message(FATAL_ERROR
			"mono.vendor/shaderc/third_party is not populated. Run `just setup`, or:\n"
			"  python3 mono.vendor/shaderc/utils/git-sync-deps")
	endif()

	set(SHADERC_SKIP_TESTS            ON  CACHE BOOL "" FORCE)
	set(SHADERC_SKIP_EXAMPLES         ON  CACHE BOOL "" FORCE)
	# glslc, the command-line driver, is the point of vendoring this today —
	# it is what compiles the engine's own GLSL, and building it here means a
	# fresh clone needs no shader compiler installed. libshaderc comes along
	# with it and is what the cooker will link when there is one.
	set(SHADERC_SKIP_EXECUTABLES      OFF CACHE BOOL "" FORCE)
	set(SHADERC_SKIP_COPYRIGHT_CHECK  ON  CACHE BOOL "" FORCE)
	set(SHADERC_SKIP_INSTALL          ON  CACHE BOOL "" FORCE)
	# The engine authors GLSL. glslang has deprecated its HLSL front end and
	# will remove it (KhronosGroup/glslang#4210), so leaving it on buys a
	# deprecation warning now and a broken bump later, for a language nothing
	# here writes.
	set(SHADERC_ENABLE_HLSL           OFF CACHE BOOL "" FORCE)
	set(ENABLE_HLSL                   OFF CACHE BOOL "" FORCE)
	# Their code, built by whatever compiler we happen to have. MONO_WERROR
	# governs first-party targets and should not be extended to a vendored one:
	# a new warning in a future GCC must not turn into a failed engine build.
	set(SHADERC_ENABLE_WERROR_COMPILE OFF CACHE BOOL "" FORCE)

	# SPIRV-Headers, SPIRV-Tools and glslang are added here rather than by
	# shaderc's own third_party/CMakeLists.txt, which guards SPIRV-Tools and
	# glslang with `if (NOT TARGET ...)` precisely so a parent can supply them.
	#
	# Not a preference -- a workaround. shaderc does
	#     set(GLSLANG_ENABLE_INSTALL $<NOT:${SKIP_GLSLANG_INSTALL}>)
	# which stores the literal string "$<NOT:ON>". if() does not evaluate
	# generator expressions, and any non-empty string that is not one of CMake's
	# false constants is true -- so glslang's install rules cannot be switched
	# off through shaderc whatever SHADERC_SKIP_INSTALL says. With SPIRV-Tools'
	# install skipped and glslang's stuck on, generation then fails with
	#     install(EXPORT "glslang-targets") includes target "SPIRV" which
	#     requires target "SPIRV-Tools-opt" that is not in any export set.
	# Setting GLSLANG_ENABLE_INSTALL in the cache does not help either: that
	# plain set() creates a normal variable which shadows the cache entry for
	# glslang's scope, and CMP0077 makes its option() honour the normal one.
	set(SPIRV_HEADERS_SKIP_EXAMPLES ON CACHE BOOL "" FORCE)
	set(SPIRV_HEADERS_SKIP_INSTALL  ON CACHE BOOL "" FORCE)
	add_subdirectory("${MONO_VENDOR}/shaderc/third_party/spirv-headers" EXCLUDE_FROM_ALL)
	# spirv-headers, unlike the other two, is added by shaderc with no
	# `if (NOT TARGET ...)` guard, so it would be added twice and CMake stops
	# with "binary directory is already used to build a source directory".
	# SHADERC_SPIRV_HEADERS_DIR is a cache STRING shaderc reads in exactly one
	# place -- the `if (IS_DIRECTORY ...)` around that add -- so blanking it is
	# the supported way to say "already handled".
	set(SHADERC_SPIRV_HEADERS_DIR "" CACHE STRING "" FORCE)

	set(SPIRV_SKIP_TESTS         ON CACHE BOOL "" FORCE)
	set(SPIRV_SKIP_EXECUTABLES   ON CACHE BOOL "" FORCE)
	set(SKIP_SPIRV_TOOLS_INSTALL ON CACHE BOOL "" FORCE)
	set(SPIRV_TOOLS_BUILD_STATIC ON CACHE BOOL "" FORCE)
	set(SPIRV-Headers_SOURCE_DIR "${MONO_VENDOR}/shaderc/third_party/spirv-headers" CACHE PATH "" FORCE)
	add_subdirectory("${MONO_VENDOR}/shaderc/third_party/spirv-tools" EXCLUDE_FROM_ALL)

	set(GLSLANG_ENABLE_INSTALL  OFF CACHE BOOL "" FORCE)
	set(GLSLANG_TESTS           OFF CACHE BOOL "" FORCE)
	set(ENABLE_GLSLANG_BINARIES OFF CACHE BOOL "" FORCE)
	add_subdirectory("${MONO_VENDOR}/shaderc/third_party/glslang" EXCLUDE_FROM_ALL)

	add_subdirectory("${MONO_VENDOR}/shaderc" EXCLUDE_FROM_ALL)
endif()

# --- Crypto++ ---------------------------------------------------------------
# Two submodules, and the second one is the build system. `.gitmodules` has the
# long form of why; the short form is that weidai11/cryptopp ships a GNUmakefile
# and no CMakeLists, and that declaring the target here — the asio and imgui
# answer — does not survive contact with 202 translation units, a per-file ISA
# flag matrix across x86, ARM and POWER, and a link order that is load-bearing
# for static initialisation. abdes/cryptopp-cmake is the wrapper the Crypto++
# wiki points at. Vendoring it costs one more submodule and one more notice, and
# buys not maintaining a copy of upstream's makefile logic.
#
# Not gated on a tier, and deliberately so. Crypto++ is portable C++ with no
# platform dependency, so it is available to client-tier and server-tier targets
# alike — asio's situation, and for the same reason. Being configured here says
# nothing about who may link it; that stays a tier question, decided by whatever
# module first lists it.
#
# Nothing links it yet. EXCLUDE_FROM_ALL keeps all 202 objects out of an
# ordinary build until something asks for Vendor::cryptopp, so the cost today is
# roughly a second of configure time for the ISA probes.
if(NOT EXISTS "${MONO_VENDOR}/cryptopp-cmake/CMakeLists.txt")
	message(FATAL_ERROR "mono.vendor/cryptopp-cmake is missing. Run `just setup`.")
endif()
if(NOT EXISTS "${MONO_VENDOR}/cryptopp/cryptlib.cpp")
	message(FATAL_ERROR "mono.vendor/cryptopp is missing. Run `just setup`.")
endif()

# The line that keeps a configure offline. Unset, cryptopp-cmake runs its own
# FetchContent against github and ignores the pinned submodule entirely, which
# would make a build both network-dependent and unreproducible.
set(CRYPTOPP_SOURCES       "${MONO_VENDOR}/cryptopp" CACHE PATH "" FORCE)
set(CRYPTOPP_BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(CRYPTOPP_INSTALL       OFF CACHE BOOL "" FORCE)
set(CRYPTOPP_BUILD_SHARED  OFF CACHE BOOL "" FORCE)
# Same reason as CRYPTOPP_SOURCES. USE_CCACHE makes cryptopp-cmake pull in
# CPM.cmake, which downloads *itself* at configure time before it does anything
# else. ccache still works through CMAKE_CXX_COMPILER_LAUNCHER if wanted.
set(USE_CCACHE             OFF CACHE BOOL "" FORCE)
add_subdirectory("${MONO_VENDOR}/cryptopp-cmake" EXCLUDE_FROM_ALL)

# cryptopp-cmake marks its include directories PUBLIC but not SYSTEM, and the
# `ci` preset builds first-party code with -Werror — a warning in a vendored
# header must never be able to fail our build. add_subdirectory(... SYSTEM)
# would say this in one word, but it is CMake 3.25 and the root file targets
# 3.24, so the property is set directly instead.
#
# Note what the second of those directories is: upstream puts the *parent* of
# the source dir on the include path so that <cryptopp/sha.h> resolves. Here
# that parent is mono.vendor/ itself, so anything linking this target can also
# reach <sdl/...> and <glm/...> by accident. Write <cryptopp/sha.h> and treat
# the bare <sha.h> spelling as unavailable.
get_target_property(_cryptopp_includes cryptopp INTERFACE_INCLUDE_DIRECTORIES)
set_target_properties(cryptopp PROPERTIES
	INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${_cryptopp_includes}")
unset(_cryptopp_includes)

# Vendor:: to match asio and imgui. cryptopp::cryptopp is upstream's own alias
# and keeps working; this is the spelling the rest of the tree should use.
add_library(Vendor::cryptopp ALIAS cryptopp)

# --- BLAKE3 -----------------------------------------------------------------
# The content hash. `assets` at L8 keys chunks, assets, bundles and the manifest
# on it, and CDN.md §2 is built on the property that makes it the right choice
# rather than merely a fast one: BLAKE3 is natively tree-structured, so
# verifying one chunk of a large asset does not require the whole file. That is
# exactly the origin's access pattern, and it is what lets a client verify a
# stream as it lands instead of buffering an asset to check it.
#
# Crypto++ is already here and ships BLAKE2b, which would serve as a leaf hash.
# Taking it would not have been the same decision. Content addressing is the one
# place a hash is compared against an attacker's rather than against accident,
# and changing the algorithm afterwards rehashes every byte anyone has stored —
# so it is picked once, on purpose, and DATATYPES_LIBRARIES.md §1.1 picks this.
#
# Only the upstream `c/` directory is built. The Rust crate above it is the
# reference implementation and is not vendored — one implementation of a format
# is the rule, and the C one is what a game binary can link.
#
# Its CMakeLists enables ASM itself and dispatches SIMD at run time, so a single
# build runs on every x86-64 machine rather than being tuned to the one that
# compiled it. Not gated on a tier: portable C with no platform dependency, like
# asio and Crypto++. Who may link it stays a tier question, decided by whichever
# module first lists it.
if(NOT EXISTS "${MONO_VENDOR}/blake3/c/CMakeLists.txt")
	message(FATAL_ERROR "mono.vendor/blake3 is missing. Run `just setup`.")
endif()

# Upstream defaults this on and would otherwise fetch oneTBB from GitHub at
# configure time — the same offline-and-reproducible line CRYPTOPP_SOURCES is.
# The parallel hashing it buys is not wanted here anyway: chunking is already
# fanned out a chunk at a time by Jobs::For, and two layers of parallelism over
# one file is how a job system gets oversubscribed.
set(BLAKE3_USE_TBB   OFF CACHE BOOL "" FORCE)
set(BLAKE3_FETCH_TBB OFF CACHE BOOL "" FORCE)
add_subdirectory("${MONO_VENDOR}/blake3/c" EXCLUDE_FROM_ALL)

# Same reason as Crypto++: the `ci` preset builds first-party code with -Werror
# and a warning in a vendored header must never be able to fail our build.
get_target_property(_blake3_includes blake3 INTERFACE_INCLUDE_DIRECTORIES)
set_target_properties(blake3 PROPERTIES
	INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${_blake3_includes}")
unset(_blake3_includes)

# Vendor:: to match the rest. BLAKE3::blake3 is upstream's own alias and keeps
# working; this is the spelling the rest of the tree should use.
add_library(Vendor::blake3 ALIAS blake3)

# --- Zstandard --------------------------------------------------------------
# Compression for delivery groups, and only for delivery groups.
#
# CDN.md §5 puts it at one level and no other: **per group, never per file and
# never per manifest.** Per file loses the cross-file redundancy that is most of
# the ratio on many small assets; per manifest defeats range requests, partial
# fetch and the whole hash tree. Chunks stay uncompressed at rest so that dedup
# works on them, and a group is the compressed thing in flight.
#
# The dictionary support is the reason this is Zstd rather than anything else
# already here. A game's content is thousands of small files that share a great
# deal — the same vertex layouts, the same material fields, the same strings —
# and a trained dictionary is what turns that into a ratio rather than into
# thousands of independently incompressible blobs.
#
# **BSD-3-Clause, not GPLv2.** Upstream is dual-licensed and ships both texts:
# LICENSE is the BSD one and COPYING is GPLv2. We take BSD. That is not a
# preference — GPLv2 would be incompatible with shipping this in a game binary
# under MPL-2.0, and the choice is recorded in THIRD_PARTY_NOTICES.md rather
# than left for somebody to infer from two files in a submodule.
if(NOT EXISTS "${MONO_VENDOR}/zstd/build/cmake/CMakeLists.txt")
	message(FATAL_ERROR "mono.vendor/zstd is missing. Run `just setup`.")
endif()

# Static only, no programs, no tests. `zstd` the command-line tool is not
# something this repository ships or uses, and building it would put a binary in
# the tree that nothing depends on.
set(ZSTD_BUILD_STATIC    ON  CACHE BOOL "" FORCE)
set(ZSTD_BUILD_SHARED    OFF CACHE BOOL "" FORCE)
set(ZSTD_BUILD_PROGRAMS  OFF CACHE BOOL "" FORCE)
set(ZSTD_BUILD_TESTS     OFF CACHE BOOL "" FORCE)
set(ZSTD_BUILD_CONTRIB   OFF CACHE BOOL "" FORCE)

# Off, and this one is a decision rather than a default.
#
# Legacy support decodes frames from Zstd 0.x — formats that predate the
# stabilised one and that nothing here could ever have written. It costs a large
# amount of extra decoder surface, and that surface parses bytes an origin
# supplied. `repo_layout.md` §1 says anyone can run a server, so a decoder we
# cannot ever need is attack surface we should not carry.
set(ZSTD_LEGACY_SUPPORT  OFF CACHE BOOL "" FORCE)

# Off deliberately, and it is the same argument `parallel` already makes.
#
# Zstd's multithreading spawns its own worker threads per compression context.
# The origin already fans out over *groups* with its own pool, and two layers of
# parallelism over one workload is how a job system gets oversubscribed — the
# reasoning BLAKE3_USE_TBB is off for, one library up.
set(ZSTD_MULTITHREAD_SUPPORT OFF CACHE BOOL "" FORCE)

add_subdirectory("${MONO_VENDOR}/zstd/build/cmake" EXCLUDE_FROM_ALL)

# Same reason as Crypto++ and BLAKE3: the `ci` preset builds first-party code
# with -Werror, and a warning in a vendored header must never fail our build.
get_target_property(_zstd_includes libzstd_static INTERFACE_INCLUDE_DIRECTORIES)
if(_zstd_includes)
	set_target_properties(libzstd_static PROPERTIES
		INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${_zstd_includes}")
endif()
unset(_zstd_includes)

# Vendor:: to match the rest.
add_library(Vendor::zstd ALIAS libzstd_static)

# --- Luau -------------------------------------------------------------------
# The script VM, vendored ahead of its consumer.
#
# `v05.md` open question 3 asked which VM and said the answer should be taken
# before the bindings manifest generates declaration files, because a `.d.ts`
# and a Luau type file are written against a value model rather than in the
# abstract. This is that answer. **Nothing links it yet** — v0.6 is where a
# script runs — and EXCLUDE_FROM_ALL is what makes vendoring it early cost
# nothing until then.
#
# MIT, twice over: Roblox's own text in LICENSE.txt and Lua.org's in
# lua_LICENSE.txt, since Luau is a fork of Lua 5.1. Both are permissive and both
# ship with the submodule.
if(NOT EXISTS "${MONO_VENDOR}/luau/CMakeLists.txt")
	message(FATAL_ERROR "mono.vendor/luau is missing. Run `just setup`.")
endif()

# Upstream defaults the CLI and the tests ON, which would put `luau`,
# `luau-analyze` and a test binary in the build for a dependency nothing links.
# Same line ZSTD_BUILD_PROGRAMS is off on.
set(LUAU_BUILD_CLI    OFF CACHE BOOL "" FORCE)
set(LUAU_BUILD_TESTS  OFF CACHE BOOL "" FORCE)
set(LUAU_BUILD_WEB    OFF CACHE BOOL "" FORCE)
set(LUAU_BUILD_SHARED OFF CACHE BOOL "" FORCE)

# Off, and not merely inherited: upstream's own default is already OFF, but this
# repository builds first-party code with -Werror under the `ci` preset and a
# vendored tree that promotes its own warnings is a build we cannot fix without
# a fork. Stated rather than assumed, because a default is not a decision.
set(LUAU_WERROR OFF CACHE BOOL "" FORCE)

add_subdirectory("${MONO_VENDOR}/luau" EXCLUDE_FROM_ALL)

# Same reason as Crypto++, BLAKE3 and Zstd.
foreach(_luau_target Luau.VM Luau.Compiler Luau.Ast Luau.Common Luau.Analysis Luau.Config Luau.EqSat)
	if(TARGET ${_luau_target})
		get_target_property(_luau_includes ${_luau_target} INTERFACE_INCLUDE_DIRECTORIES)
		if(_luau_includes)
			set_target_properties(${_luau_target} PROPERTIES
				INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${_luau_includes}")
		endif()
		unset(_luau_includes)
	endif()
endforeach()
unset(_luau_target)

# Two aliases, not eleven, and the omissions are the decision here.
#
# `Luau.VM` executes bytecode and `Luau.Compiler` produces it from source; those
# are what running a game script needs. The rest stay unaliased until something
# asks:
#
# - **`Luau.CodeGen` is the native-code backend, and it is off the table until
#   determinism is measured rather than assumed.** This repository diffs two runs
#   byte for byte — `just determinism` and `just replay-check` — and a JIT is a
#   second execution path for the same script. That is exactly the test v0.4
#   stated before changing to `-O3`, and it has to be stated again before a
#   second backend, not after.
# - **`Luau.Require` resolves `require` against a filesystem.** A game's scripts
#   arrive through `assets` and the CDN, content-addressed and signed. A resolver
#   that reads paths would be a second way in that none of that covers.
add_library(Vendor::luau_vm ALIAS Luau.VM)
add_library(Vendor::luau_compiler ALIAS Luau.Compiler)

# **`Luau.Analysis` is the type checker, and the tool this waited for now
# exists.** The note here said it would get an alias when something wanted to
# check the generated declaration files; `mono.tools/scriptcheck` is that thing,
# and `just typecheck` is what runs it.
#
# **Not linked by `Engine::script`, and that separation is the point.** Nothing a
# game binary contains type-checks anything — a shipped runtime compiles bytecode
# and runs it. This is a build-time consumer only, which is why the alias sits
# beside the two the runtime uses rather than among them.
#
# Upstream's `luau-analyze` CLI would have been the obvious answer and cannot be:
# it has no flag for loading a definition file, so it can only check against the
# built-in globals. Every `Instance`, `Vector3` and `workspace` in this engine
# would read as an unknown global, which is the one thing the check exists to
# catch. Forty lines against a fork.
add_library(Vendor::luau_analysis ALIAS Luau.Analysis)

# --- QuickJS-ng -------------------------------------------------------------
# The second script VM, and the JavaScript/TypeScript half of the two-language
# choice `v05.md` §5.7 records.
#
# Vendored alongside Luau rather than instead of it. `v05.md` argued for one
# runtime and the decision went the other way deliberately — two languages, two
# VMs, one binding surface — so what matters here is that the *engine* keeps one
# tick and one determinism story across both. Three properties of this engine
# are what make that possible, and each is an API rather than a hope:
#
# - **The host drives the microtask queue.** `JS_ExecutePendingJob` and
#   `JS_IsJobPending` are called by us, in a drain loop, at a point we choose.
#   That is the whole reason a JS engine can live under `world::Driver` at all:
#   promise reactions run at the barrier, in a deterministic order, rather than
#   whenever a runtime that thinks it owns the event loop decides to run them.
#   An engine without this API would have to be rejected on rule 5 alone.
# - **The host drives collection.** `JS_SetGCThreshold` and `JS_RunGC` mean the
#   collector can be taken off automatic and run at a fixed point in the tick,
#   so GC timing cannot differ between two runs of one recording.
# - **The host can stop a script.** `JS_SetInterruptHandler`, `JS_SetMemoryLimit`
#   and `JS_SetMaxStackSize` are what bound untrusted user code, which is the
#   situation `repo_layout.md` §1 puts this engine in.
#
# No JIT: it is a bytecode interpreter, which is the same reason `Luau.CodeGen`
# is left unaliased one block up. MIT, Bellard and Gordon, continued by the ng
# fork after the two projects merged efforts. No external dependencies.
#
# **Nothing links it yet** — v0.6 is where a script runs — and EXCLUDE_FROM_ALL
# is what keeps that free.
if(NOT EXISTS "${MONO_VENDOR}/quickjs/CMakeLists.txt")
	message(FATAL_ERROR "mono.vendor/quickjs is missing. Run `just setup`.")
endif()

# Off, and this is the load-bearing one rather than a tidy-up.
#
# `quickjs-libc` is upstream's `std` and `os` modules: file I/O, process spawn,
# `setTimeout` against a wall clock. Every part of that is either a capability a
# game script must not have or a source of non-determinism a recording cannot
# replay — a script that sleeps on real time is precisely the desync rule 5
# names. The engine supplies what a script may reach through its own bindings,
# and this is the line where the alternative is refused rather than sandboxed
# later.
set(QJS_BUILD_LIBC OFF CACHE BOOL "" FORCE)

# No CLI, no examples, no install rules, static only. Same line
# ZSTD_BUILD_PROGRAMS and LUAU_BUILD_CLI are off on.
set(QJS_BUILD_EXAMPLES   OFF CACHE BOOL "" FORCE)
set(QJS_BUILD_CLI_STATIC OFF CACHE BOOL "" FORCE)
set(QJS_ENABLE_INSTALL   OFF CACHE BOOL "" FORCE)
set(QJS_BUILD_WERROR     OFF CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS    OFF CACHE BOOL "" FORCE)

add_subdirectory("${MONO_VENDOR}/quickjs" EXCLUDE_FROM_ALL)

# Same reason as Crypto++, BLAKE3, Zstd and Luau.
get_target_property(_qjs_includes qjs INTERFACE_INCLUDE_DIRECTORIES)
if(_qjs_includes)
	set_target_properties(qjs PROPERTIES
		INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${_qjs_includes}")
endif()
unset(_qjs_includes)

# Vendor:: to match the rest. `qjs` is upstream's own target name and keeps
# working; this is the spelling the rest of the tree should use.
add_library(Vendor::quickjs ALIAS qjs)
