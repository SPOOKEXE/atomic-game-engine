# MonoVendor.cmake - third-party configuration, in one place.
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
# Vulkan backend turns a recorded command buffer into queue submissions -
# barrier tracking, descriptor and pipeline hashing, the pending-destroy sweep -
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
# on the command line. Debug information is deliberately kept - a stack through
# SDL should still name its frames.
#
# Off with `-DMONO_OPTIMISE_VENDOR=OFF`, which is what to pass when the thing
# being debugged *is* a vendored library.
option(MONO_OPTIMISE_VENDOR "Build third-party code optimised, whatever the build type" ON)

if(MONO_OPTIMISE_VENDOR AND NOT MSVC)
	# An append is enough here: GCC and Clang honour the last `-O` on the line.
	set(CMAKE_C_FLAGS_DEBUG "${CMAKE_C_FLAGS_DEBUG} -O2")
	set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} -O2")
elseif(MONO_OPTIMISE_VENDOR AND MSVC)
	# **A replacement, and it used to be an append. That one word broke every
	# Windows build this repository has ever done.**
	#
	# MSVC's Debug defaults are `/Zi /Ob0 /Od /RTC1`, and `/RTC1` with `/O2` is
	# not merely redundant - the compiler refuses the command line outright:
	#
	#   cl : Command line error D8016 : '/RTC1' and '/O2' are incompatible
	#
	# **And it is not only Debug builds that carry these.** `try_compile` runs
	# with no build type, so CMake falls back to the Debug flags for it - the
	# `-MDd` on those command lines is the giveaway. So an appended `/O2` made
	# every feature check in every vendored project fail, in a `release` build
	# that never compiles a Debug object: SDL alone reported 77 checks failed and
	# none succeeded, including `SDL_CPU_X64` on an x64 machine. What reached a
	# human was a `#error` about a joystick define, sixty files downstream.
	#
	# It stayed invisible because the branch above is the one that runs here. GCC
	# takes the last `-O` and carries on, so the same idea is correct on Linux
	# and fatal on Windows, and only one of the two is built every day.
	foreach(lang C CXX)
		set(flags "${CMAKE_${lang}_FLAGS_DEBUG}")
		string(REGEX REPLACE "/RTC[1csu]+" "" flags "${flags}")
		string(REPLACE "/Od" "" flags "${flags}")
		string(REPLACE "/Ob0" "" flags "${flags}")
		string(REGEX REPLACE " +" " " flags "${flags}")
		string(STRIP "${flags}" flags)

		# `/Ob1` rather than leaving inlining at the default, to match what
		# `/O2` expects now that `/Ob0` is gone.
		set(CMAKE_${lang}_FLAGS_DEBUG "${flags} /O2 /Ob1")
	endforeach()
	unset(flags)
	unset(lang)
endif()

if(NOT EXISTS "${MONO_VENDOR}/glm/CMakeLists.txt")
	message(FATAL_ERROR
		"mono.vendor/ is empty. Run `just setup`, or:\n"
		"  git submodule update --init --recursive --depth 1")
endif()

# --- vendor includes are SYSTEM includes ------------------------------------

# Re-declare a vendored target's public include directories as SYSTEM ones, so
# that a warning raised inside a header we do not own cannot fail our build.
#
# **The `ci` preset compiles first-party code with `-Werror`.** A vendor bump
# that introduces one `-Wdeprecated-declarations` in a header 500 translation
# units include is then a red build in code nobody here can fix without a fork,
# and the fix under time pressure is always to weaken the warning set for
# everybody. `-isystem` is the flag that says "not ours"; this is how a target
# gets it.
#
# **What it is not: a build-time optimisation.** Measured, because §E2 item 7
# left it unmeasured and the intuition is that skipping diagnostics inside
# enormous template headers must be faster. It is not. The eight heaviest
# first-party translation units, `release` flags, ccache bypassed, every
# `-I` naming `mono.vendor` rewritten to `-isystem`, best of three and run
# twice each way: 57.84 and 58.15 CPU-seconds plain against 61.91 and 58.01
# SYSTEM. That is one noise band, not an effect. Applied for the warning
# argument alone.
#
# `INTERFACE_SYSTEM_INCLUDE_DIRECTORIES` rather than `add_subdirectory(...
# SYSTEM)`, which says the same thing in one word and is CMake 3.25; the root
# file asks for 3.24. A target that does not exist, or that declares no include
# directories of its own, is skipped rather than an error - the vendor set
# differs by preset, and a `server` configure has no SDL at all.
function(mono_vendor_system)
	foreach(target IN LISTS ARGN)
		if(NOT TARGET ${target})
			continue()
		endif()
		# An alias cannot carry a property, so resolve to what it names. Passing
		# an alias here is the natural mistake, because an alias is the spelling
		# every link line uses.
		get_target_property(aliased ${target} ALIASED_TARGET)
		if(aliased)
			set(target ${aliased})
		endif()
		get_target_property(includes ${target} INTERFACE_INCLUDE_DIRECTORIES)
		if(includes)
			set_property(TARGET ${target}
				APPEND PROPERTY INTERFACE_SYSTEM_INCLUDE_DIRECTORIES ${includes})
		endif()
	endforeach()
endfunction()

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

	# **SDL's 2D renderer is not built, and nothing here has ever used it.**
	# `SDL_Renderer` is the sprite-and-rectangle API; this engine draws through
	# `SDL_GPU`, which is a separate subsystem depending only on `SDL_VIDEO`, and
	# the Dear ImGui backend we compile is `imgui_impl_sdlgpu3` rather than
	# `imgui_impl_sdlrenderer3`. A repository-wide search for `SDL_Renderer`,
	# `SDL_CreateRenderer` and `SDL_RenderPresent` across every first-party
	# module finds nothing.
	#
	# Turning it off removes every render driver with it - D3D9, D3D11, D3D12,
	# Metal, Vulkan, OpenGL and OpenGL ES - which is a smaller library for
	# nothing given up. It also removes `src/render/opengles2/SDL_render_gles2.c`,
	# which is where the Windows release build stopped: MSVC's C compiler rejects
	# it with a run of `C2065: 'tex_coord': undeclared identifier`, in a file
	# nothing in this engine would have called into.
	set(SDL_RENDER      OFF CACHE BOOL "" FORCE)

	set(SDL_SHARED      ON  CACHE BOOL "" FORCE)
	set(SDL_STATIC      OFF CACHE BOOL "" FORCE)
	set(SDL_TESTS       OFF CACHE BOOL "" FORCE)
	set(SDL_EXAMPLES    OFF CACHE BOOL "" FORCE)
	set(SDL_INSTALL     OFF CACHE BOOL "" FORCE)

	# **`SDL_TEST_LIBRARY`, and it used to be spelled `SDL_TEST`.** Upstream
	# renamed it and the old name went on being set here, which is the quiet
	# kind of stale: a `set(... CACHE ... FORCE)` on a variable no project reads
	# is not an error, so nothing said anything and `libSDL3_test.a` was built by
	# every client configure. Nothing in this repository links it - it is the
	# harness SDL's own test programs use, and `SDL_TESTS` above already declines
	# those.
	#
	# It defaults ON, so the rename silently turned it back on rather than
	# leaving it where it was put.
	set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)

	# `SDL_DISABLE_PCH` was here and is gone rather than corrected, because there
	# is nothing to correct it to: this SDL calls `target_precompile_headers`
	# unconditionally and offers no option over it. A setting that reads as "we
	# turned the precompiled header off" while the compile line carries `/Yu` is
	# worse than no setting at all.

	add_subdirectory("${MONO_VENDOR}/sdl" EXCLUDE_FROM_ALL)

	# SDL declares its include directories PUBLIC and not SYSTEM, and 149
	# first-party translation units carry them. mono_vendor_system above.
	mono_vendor_system(SDL3-shared SDL3-static SDL3_Headers)

	# SDL's Vulkan backend loads MoltenVK dynamically on Apple. The source is a
	# pinned submodule and the matching upstream release artifact is fetched by
	# `just setup` or CI with its published SHA-256 before configure.
	if(APPLE)
		set(MONO_MOLTENVK_ROOT
			"${CMAKE_SOURCE_DIR}/.cache/vendor/moltenvk/1.4.2/MoltenVK"
			CACHE PATH "Extracted MoltenVK 1.4.2 release root")
		set(moltenvk_library
			"${MONO_MOLTENVK_ROOT}/MoltenVK/dynamic/dylib/macOS/libMoltenVK.dylib")
		if(NOT EXISTS "${moltenvk_library}")
			message(FATAL_ERROR
				"MoltenVK 1.4.2 is missing. Run scripts/fetch-moltenvk.sh before configuring.")
		endif()
		add_library(MoltenVK-runtime SHARED IMPORTED GLOBAL)
		add_library(MoltenVK::Runtime ALIAS MoltenVK-runtime)
		set_target_properties(MoltenVK-runtime PROPERTIES IMPORTED_LOCATION "${moltenvk_library}")
	endif()
endif()

# --- glm --------------------------------------------------------------------
set(GLM_ENABLE_CXX_20 ON CACHE BOOL "" FORCE)
set(GLM_BUILD_TESTS   OFF CACHE BOOL "" FORCE)
set(GLM_BUILD_INSTALL OFF CACHE BOOL "" FORCE)
add_subdirectory("${MONO_VENDOR}/glm" EXCLUDE_FROM_ALL)

# Every first-party translation unit that links `core` carries this one - 504 of
# them. mono_vendor_system above.
mono_vendor_system(glm glm-header-only)

# --- spdlog -----------------------------------------------------------------
set(SPDLOG_BUILD_EXAMPLE OFF CACHE BOOL "" FORCE)
set(SPDLOG_INSTALL       OFF CACHE BOOL "" FORCE)
add_subdirectory("${MONO_VENDOR}/spdlog" EXCLUDE_FROM_ALL)

# The other of the two that reach all 504. mono_vendor_system above.
mono_vendor_system(spdlog spdlog_header_only)

# --- SQLite -----------------------------------------------------------------
# SQLiteCpp carries the released SQLite amalgamation in-tree. Only its sqlite3
# target is configured: the engine owns its adapter and needs no second C++ API
# over the C boundary.
set(SQLITE_OMIT_LOAD_EXTENSION ON CACHE BOOL "" FORCE)
add_subdirectory("${MONO_VENDOR}/sqlitecpp/sqlite3" EXCLUDE_FROM_ALL)
mono_vendor_system(sqlite3 SQLite::SQLite3)

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
# ones - `git ls-files -s` reports them as mode 120000.
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

# **The Windows socket libraries, named rather than left to the compiler.**
# asio's IOCP backend calls `AcceptEx` and `GetAcceptExSockaddrs`, which live in
# mswsock, and the rest of Winsock in ws2_32. asio asks for both with
# `#pragma comment(lib, ...)`, which is an MSVC extension - GCC parses it and
# does nothing. So a mingw-w64 build compiles all 964 objects and then fails at
# the link of `server.exe` with two undefined references and nothing naming a
# missing library.
#
# Costs MSVC nothing: it links the same two either way, and a library named
# twice is linked once.
if(WIN32)
	target_link_libraries(vendor_asio INTERFACE ws2_32 mswsock)
endif()

# --- nlohmann/json ------------------------------------------------------------
# JSON, for the one thing in this repository that speaks it: `mono.studio`'s
# control server answers Model Context Protocol, which is JSON-RPC 2.0.
#
# Header-only and declared here rather than added as a subdirectory, for asio's
# reason - upstream's CMakeLists builds tests and a package config nobody here
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
# reason - the mapping was free and the parser was not vendored.
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
# - no attribution obligation, no patent grant to read, nothing that reaches a
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
# exactly one translation unit - `audio/src/Mp3.cpp` - because these headers are
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
# keep working when the renderer is the thing being debugged - see
# mono.engine/render/AGENTS.md.
#
# Client tier: it needs SDL3 and a GPU backend.
if(MONO_BUILD_CLIENT)
	# imgui ships no CMakeLists either, and the backends are deliberately not
	# a library - you compile the two that match your platform. SDL3 for the
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

	# On 73 test binaries, and the only vendor left arriving as a plain -I.
	# mono_vendor_system above.
	mono_vendor_system(Catch2 Catch2WithMain)
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
#              - none of them exist at build time, so none can be compiled
#              ahead of it.
#
# Keeping both is deliberate rather than redundant. Build-time compilation
# fails the build on a bad built-in shader, which is where that belongs;
# runtime compilation returns an error string to whoever authored the shader,
# which is where *that* belongs.
#
# Client-tier, and gated on nothing else. libshaderc is linked into the client
# for the runtime half above, so MONO_VENDORED_GLSLC only decides which glslc
# compiles the built-in shaders - not whether shaderc is configured at all.
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
	# glslc, the command-line driver, is the point of vendoring this today -
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

	# **The dynamic C runtime, because everything else here uses it.**
	#
	# shaderc defaults this off, and off means it sets `CMAKE_MSVC_RUNTIME_LIBRARY`
	# to the *static* runtime for its own targets. glslang is added by this file
	# a few lines below rather than by shaderc, so it takes CMake's default and
	# gets the dynamic one - and linking `glslc` then fails with a screenful of
	#
	#   error LNK2038: mismatch detected for 'RuntimeLibrary': value
	#   'MD_DynamicRelease' doesn't match value 'MT_StaticRelease' in main.cc.obj
	#
	# one line per object in glslang. Two runtimes in one process is the thing
	# LNK2038 exists to refuse, so this is a real conflict rather than a warning
	# to suppress.
	#
	# Dynamic rather than making glslang static, because dynamic is what the rest
	# of this build already is: SDL is a shared library here, and MSVC's default
	# for everything else is `/MD`. Aligning the odd one out is one line;
	# aligning everything else to it is not.
	#
	# MSVC-only in effect - the option does nothing on any other toolchain.
	set(SHADERC_ENABLE_SHARED_CRT     ON  CACHE BOOL "" FORCE)

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

	# mono_vendor_system above. glslang and SPIRV-Tools are swept too: they are
	# not on a first-party include line today, but they are one `#include` away
	# from being, and a vendored tree of this size is the last place a warning
	# should be allowed to reach `-Werror` from.
	mono_vendor_system(shaderc shaderc_static shaderc_util
		glslang SPIRV SPIRV-Tools-static SPIRV-Headers)
endif()

# --- SPIRV-Cross -------------------------------------------------------------
# SPIR-V to MSL. The other end of shaderc: glslc produces SPIR-V and SDL's Metal
# backend takes MSL or a metallib and never SPIR-V, so this is what stands
# between the two on that platform. `docs/DEFERRED.md` D00001 is the item.
#
# Client-gated, beside shaderc and for its reason rather than by analogy: the
# only two consumers are `mono.tools/shadercross`, which translates the built-in
# shaders during the build, and `render::ShaderCompiler`, which translates
# runtime-authored ones. `render` is client-tier and a server preset configures
# neither, so a headless build pays nothing.
#
# **Three of its eight libraries, and the omissions are the point.** `core` is
# the parser, `glsl` is the emitter every other backend derives from, `msl` is
# the one thing here that is wanted. HLSL, the deprecated C++ backend, the JSON
# reflection backend, the C API and the CLI are all off - this engine authors
# GLSL and reads SPIR-V, and a target nothing links is build time spent on a
# claim we do not make.
if(MONO_BUILD_CLIENT)
	if(NOT EXISTS "${MONO_VENDOR}/spirv-cross/CMakeLists.txt")
		message(FATAL_ERROR "mono.vendor/spirv-cross is missing. Run `just setup`.")
	endif()

	set(SPIRV_CROSS_STATIC        ON  CACHE BOOL "" FORCE)
	set(SPIRV_CROSS_SHARED        OFF CACHE BOOL "" FORCE)
	set(SPIRV_CROSS_CLI           OFF CACHE BOOL "" FORCE)
	set(SPIRV_CROSS_ENABLE_TESTS  OFF CACHE BOOL "" FORCE)
	set(SPIRV_CROSS_ENABLE_GLSL   ON  CACHE BOOL "" FORCE)
	set(SPIRV_CROSS_ENABLE_MSL    ON  CACHE BOOL "" FORCE)
	set(SPIRV_CROSS_ENABLE_HLSL   OFF CACHE BOOL "" FORCE)
	set(SPIRV_CROSS_ENABLE_CPP    OFF CACHE BOOL "" FORCE)
	set(SPIRV_CROSS_ENABLE_REFLECT OFF CACHE BOOL "" FORCE)
	set(SPIRV_CROSS_ENABLE_UTIL   OFF CACHE BOOL "" FORCE)
	set(SPIRV_CROSS_ENABLE_C_API  OFF CACHE BOOL "" FORCE)
	# Unconditional install rules, so they are skipped as well as excluded from
	# `all`. Upstream's macro also calls `export(TARGETS ...)`, which writes a
	# stray config file into the build tree when it is left on.
	set(SPIRV_CROSS_SKIP_INSTALL  ON  CACHE BOOL "" FORCE)
	# Their code, our compiler. Same rule as shaderc above: a new warning in a
	# future GCC must not turn into a failed engine build.
	set(SPIRV_CROSS_WERROR        OFF CACHE BOOL "" FORCE)
	set(SPIRV_CROSS_MISC_WARNINGS OFF CACHE BOOL "" FORCE)

	add_subdirectory("${MONO_VENDOR}/spirv-cross" EXCLUDE_FROM_ALL)

	# One name for the three, so a consumer links what the job needs rather than
	# knowing which of upstream's libraries the emitter lives in.
	add_library(vendor_spirv_cross INTERFACE)
	add_library(Vendor::spirv-cross ALIAS vendor_spirv_cross)
	target_link_libraries(vendor_spirv_cross INTERFACE spirv-cross-msl spirv-cross-glsl spirv-cross-core)

	# SYSTEM, for the reason `mono.vendor/AGENTS.md` gives: the `ci` preset builds
	# first-party code with -Werror and a warning in a vendored header must never
	# be able to fail our build. Upstream declares its include directory PUBLIC
	# and not SYSTEM, so it is re-declared here rather than wrapped -
	# `INTERFACE_SYSTEM_INCLUDE_DIRECTORIES` is what CMake 3.24 has; the `SYSTEM`
	# target property is 3.25 and the root file asks for 3.24.
	mono_vendor_system(spirv-cross-core spirv-cross-glsl spirv-cross-msl)
endif()

# --- Crypto++ ---------------------------------------------------------------
# Two submodules, and the second one is the build system. `.gitmodules` has the
# long form of why; the short form is that weidai11/cryptopp ships a GNUmakefile
# and no CMakeLists, and that declaring the target here - the asio and imgui
# answer - does not survive contact with 202 translation units, a per-file ISA
# flag matrix across x86, ARM and POWER, and a link order that is load-bearing
# for static initialisation. abdes/cryptopp-cmake is the wrapper the Crypto++
# wiki points at. Vendoring it costs one more submodule and one more notice, and
# buys not maintaining a copy of upstream's makefile logic.
#
# Not gated on a tier, and deliberately so. Crypto++ is portable C++ with no
# platform dependency, so it is available to client-tier and server-tier targets
# alike - asio's situation, and for the same reason. Being configured here says
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
# `ci` preset builds first-party code with -Werror - a warning in a vendored
# header must never be able to fail our build. add_subdirectory(... SYSTEM)
# would say this in one word, but it is CMake 3.25 and the root file targets
# 3.24, so the property is set directly instead.
#
# Note what the second of those directories is: upstream puts the *parent* of
# the source dir on the include path so that <cryptopp/sha.h> resolves. Here
# that parent is mono.vendor/ itself, so anything linking this target can also
# reach <sdl/...> and <glm/...> by accident. Write <cryptopp/sha.h> and treat
# the bare <sha.h> spelling as unavailable.
mono_vendor_system(cryptopp)

# Vendor:: to match asio and imgui. cryptopp::cryptopp is upstream's own alias
# and keeps working; this is the spelling the rest of the tree should use.
add_library(Vendor::cryptopp ALIAS cryptopp)

# --- BLAKE3 -----------------------------------------------------------------
# The content hash. `assets` at L8 keys chunks, assets, bundles and the manifest
# on it, and the content model is built on the property that makes BLAKE3 the
# right choice rather than merely a fast one: BLAKE3 is natively tree-structured, so
# verifying one chunk of a large asset does not require the whole file. That is
# exactly the origin's access pattern, and it is what lets a client verify a
# stream as it lands instead of buffering an asset to check it.
#
# Crypto++ is already here and ships BLAKE2b, which would serve as a leaf hash.
# Taking it would not have been the same decision. Content addressing is the one
# place a hash is compared against an attacker's rather than against accident,
# and changing the algorithm afterwards rehashes every byte anyone has stored -
# so it is picked once, on purpose, and BLAKE3 is that pick.
#
# Only the upstream `c/` directory is built. The Rust crate above it is the
# reference implementation and is not vendored - one implementation of a format
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
# configure time - the same offline-and-reproducible line CRYPTOPP_SOURCES is.
# The parallel hashing it buys is not wanted here anyway: chunking is already
# fanned out a chunk at a time by Jobs::For, and two layers of parallelism over
# one file is how a job system gets oversubscribed.
set(BLAKE3_USE_TBB   OFF CACHE BOOL "" FORCE)
set(BLAKE3_FETCH_TBB OFF CACHE BOOL "" FORCE)
add_subdirectory("${MONO_VENDOR}/blake3/c" EXCLUDE_FROM_ALL)

# Same reason as Crypto++: the `ci` preset builds first-party code with -Werror
# and a warning in a vendored header must never be able to fail our build.
mono_vendor_system(blake3)

# Vendor:: to match the rest. BLAKE3::blake3 is upstream's own alias and keeps
# working; this is the spelling the rest of the tree should use.
add_library(Vendor::blake3 ALIAS blake3)

# --- Zstandard --------------------------------------------------------------
# Compression for delivery groups, and only for delivery groups.
#
# Compression sits at one level and no other: **per group, never per file and
# never per manifest.** Per file loses the cross-file redundancy that is most of
# the ratio on many small assets; per manifest defeats range requests, partial
# fetch and the whole hash tree. Chunks stay uncompressed at rest so that dedup
# works on them, and a group is the compressed thing in flight.
#
# The dictionary support is the reason this is Zstd rather than anything else
# already here. A game's content is thousands of small files that share a great
# deal - the same vertex layouts, the same material fields, the same strings -
# and a trained dictionary is what turns that into a ratio rather than into
# thousands of independently incompressible blobs.
#
# **BSD-3-Clause, not GPLv2.** Upstream is dual-licensed and ships both texts:
# LICENSE is the BSD one and COPYING is GPLv2. We take BSD. That is not a
# preference - GPLv2 would be incompatible with shipping this in a game binary
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
# Legacy support decodes frames from Zstd 0.x - formats that predate the
# stabilised one and that nothing here could ever have written. It costs a large
# amount of extra decoder surface, and that surface parses bytes an origin
# supplied. Anyone can run a server, so a decoder we
# cannot ever need is attack surface we should not carry.
set(ZSTD_LEGACY_SUPPORT  OFF CACHE BOOL "" FORCE)

# Off deliberately, and it is the same argument `parallel` already makes.
#
# Zstd's multithreading spawns its own worker threads per compression context.
# The origin already fans out over *groups* with its own pool, and two layers of
# parallelism over one workload is how a job system gets oversubscribed - the
# reasoning BLAKE3_USE_TBB is off for, one library up.
set(ZSTD_MULTITHREAD_SUPPORT OFF CACHE BOOL "" FORCE)

add_subdirectory("${MONO_VENDOR}/zstd/build/cmake" EXCLUDE_FROM_ALL)

# Same reason as Crypto++ and BLAKE3: the `ci` preset builds first-party code
# with -Werror, and a warning in a vendored header must never fail our build.
mono_vendor_system(libzstd_static)

# Vendor:: to match the rest.
add_library(Vendor::zstd ALIAS libzstd_static)

# --- Roblox files -----------------------------------------------------------
# The four Roblox place and model containers. The importer needs one complete
# decoder rather than separate Studio and Rojo dialects, and this library's DOM
# keeps every class, property and asset reference long enough for the Studio
# port report to explain what the engine cannot yet map.
#
# Upstream has no release branch or tag. The submodule follows its only branch,
# `main`, while the superproject pins the exact commit like every other vendor.
# Its tests are valuable in its own repository but are not a second copy of this
# repository's bake tests, and its CLI is not one of our shipped tools.
if(NOT EXISTS "${MONO_VENDOR}/roblox-files/CMakeLists.txt")
	message(FATAL_ERROR "mono.vendor/roblox-files is missing. Run `just setup`.")
endif()

set(RBXL_BUILD_CLI   OFF CACHE BOOL "" FORCE)
set(RBXL_BUILD_TESTS OFF CACHE BOOL "" FORCE)
add_subdirectory("${MONO_VENDOR}/roblox-files" EXCLUDE_FROM_ALL)

# A vendor header cannot turn a first-party warning into a CI failure.
mono_vendor_system(rbxl)
add_library(Vendor::roblox_files ALIAS rbxl)

# --- miniz ------------------------------------------------------------------
# ZIP reading and writing for portable project packages. Upstream has no
# release branch, so the superproject pins the 3.1.2 tag while `.gitmodules`
# follows its only development branch.
#
# The upstream CMake file changes global output and verbosity settings and uses
# generic BUILD_TESTS/BUILD_EXAMPLES cache names. Declare its four source files
# here so those settings cannot leak into first-party targets. `MINIZ_NO_TIME`
# makes every writer timestamp zero and removes local-time-zone drift from a
# reproducible archive.
if(NOT EXISTS "${MONO_VENDOR}/miniz/miniz_zip.c")
	message(FATAL_ERROR "mono.vendor/miniz is missing. Run `just setup`.")
endif()

include(GenerateExportHeader)
add_library(mono_miniz STATIC
	"${MONO_VENDOR}/miniz/miniz.c"
	"${MONO_VENDOR}/miniz/miniz_zip.c"
	"${MONO_VENDOR}/miniz/miniz_tinfl.c"
	"${MONO_VENDOR}/miniz/miniz_tdef.c")
generate_export_header(mono_miniz
	BASE_NAME miniz
	EXPORT_FILE_NAME "${CMAKE_CURRENT_BINARY_DIR}/miniz_export.h")
target_include_directories(mono_miniz PUBLIC
	"${MONO_VENDOR}/miniz"
	"${CMAKE_CURRENT_BINARY_DIR}")
target_compile_definitions(mono_miniz PUBLIC MINIZ_NO_TIME MINIZ_STATIC_DEFINE)
mono_vendor_system(mono_miniz)
add_library(Vendor::miniz ALIAS mono_miniz)

# --- Luau -------------------------------------------------------------------
# The script VM, vendored ahead of its consumer.
#
# Luau was settled as the VM before the bindings manifest generates its
# declaration files, because a `.d.ts` and a Luau type file are written against
# a value model rather than in the abstract. This is that answer. **Nothing
# links it yet** - v0.6 is where a script runs - and EXCLUDE_FROM_ALL is what
# makes vendoring it early cost nothing until then.
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
# Luau.Bytecode is on the list because it was the one Luau target still arriving
# as a plain -I, on 20 translation units.
mono_vendor_system(Luau.VM Luau.Compiler Luau.Ast Luau.Common Luau.Bytecode
	Luau.Analysis Luau.Config Luau.EqSat Luau.CodeGen)

# Two aliases, not eleven, and the omissions are the decision here.
#
# `Luau.VM` executes bytecode and `Luau.Compiler` produces it from source; those
# are what running a game script needs. The rest stay unaliased until something
# asks:
#
# - **`Luau.CodeGen` is the native-code backend, and it is off the table until
#   determinism is measured rather than assumed.** This repository diffs two runs
#   byte for byte - `just determinism` and `just replay-check` - and a JIT is a
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
# game binary contains type-checks anything - a shipped runtime compiles bytecode
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
# The second script VM, and the JavaScript/TypeScript half of the recorded
# choice: two languages, each its own VM, over one binding surface.
#
# Vendored alongside Luau rather than instead of it. One runtime was argued
# for and the decision went the other way deliberately, so what matters here
# is that the *engine* keeps one tick and one determinism story across both.
# Three properties of this engine are what make that possible, and each is an
# API rather than a hope:
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
#   and `JS_SetMaxStackSize` are what bound untrusted user code, which is every
#   script this engine runs.
#
# No JIT: it is a bytecode interpreter, which is the same reason `Luau.CodeGen`
# is left unaliased one block up. MIT, Bellard and Gordon, continued by the ng
# fork after the two projects merged efforts. No external dependencies.
#
# **Nothing links it yet** - v0.6 is where a script runs - and EXCLUDE_FROM_ALL
# is what keeps that free.
if(NOT EXISTS "${MONO_VENDOR}/quickjs/CMakeLists.txt")
	message(FATAL_ERROR "mono.vendor/quickjs is missing. Run `just setup`.")
endif()

# Off, and this is the load-bearing one rather than a tidy-up.
#
# `quickjs-libc` is upstream's `std` and `os` modules: file I/O, process spawn,
# `setTimeout` against a wall clock. Every part of that is either a capability a
# game script must not have or a source of non-determinism a recording cannot
# replay - a script that sleeps on real time is precisely the desync rule 5
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
mono_vendor_system(qjs)

# Vendor:: to match the rest. `qjs` is upstream's own target name and keeps
# working; this is the spelling the rest of the tree should use.
add_library(Vendor::quickjs ALIAS qjs)

# --- ngtcp2 -----------------------------------------------------------------
# QUIC. What belongs here is why this library and what is switched off.
#
# **Nothing under its `lib/` names a TLS stack**, which is the property that
# decided it over seven alternatives. Every other candidate hard-wires one:
# picoquic and quicly require picotls, lsquic requires BoringSSL which requires
# Go, quiche is Rust, mvfst depends on folly, and msquic brings its own event
# loop with Schannel on Windows and OpenSSL elsewhere. This one takes a callback
# table, so the crypto is `engine::net::quic` over Crypto++ and the fresh-clone
# rule in `mono.vendor/AGENTS.md` survives - CMake, Ninja and a C++ compiler and
# nothing else.
#
# **Every entry point takes an explicit `ngtcp2_tstamp`.** That is not a
# convenience, it is the only shape compatible with `net`'s rule that time is
# passed in and never read, and `just determinism` and `just replay-check` both
# rest on it.
#
# MIT. Not gated on a tier: portable C with no platform dependency, like asio,
# Crypto++ and BLAKE3. Who may link it stays a tier question, decided by
# whichever module first lists it - which is `Engine::net` at L11.
if(NOT EXISTS "${MONO_VENDOR}/ngtcp2/lib/CMakeLists.txt")
	message(FATAL_ERROR "mono.vendor/ngtcp2 is missing. Run `just setup`.")
endif()

# `lib/` and nothing else. Upstream's `crypto/` holds the helper libraries for
# OpenSSL, GnuTLS, wolfSSL, picotls and BoringSSL, and `examples/` wants libev
# and nghttp3 - none of which is vendored and none of which may be looked for at
# configure time. ENABLE_LIB_ONLY is what makes that a stated decision rather
# than a set of find_package calls that happen to fail.
set(ENABLE_LIB_ONLY   ON  CACHE BOOL "" FORCE)
set(ENABLE_STATIC_LIB ON  CACHE BOOL "" FORCE)
set(ENABLE_SHARED_LIB OFF CACHE BOOL "" FORCE)
set(BUILD_TESTING     OFF CACHE BOOL "" FORCE)

# Upstream defaults the OpenSSL backend ON. Off here, and it is the load-bearing
# line rather than a tidy-up: left on, the configure looks for a system OpenSSL
# and a build would silently differ between a machine that has one and a machine
# that does not.
set(ENABLE_OPENSSL   OFF CACHE BOOL "" FORCE)
set(ENABLE_BORINGSSL OFF CACHE BOOL "" FORCE)
set(ENABLE_GNUTLS    OFF CACHE BOOL "" FORCE)
set(ENABLE_WOLFSSL   OFF CACHE BOOL "" FORCE)
set(ENABLE_PICOTLS   OFF CACHE BOOL "" FORCE)

# Same reason LUAU_WERROR and QJS_BUILD_WERROR are off: the `ci` preset builds
# first-party code with -Werror and a vendored tree that promotes its own
# warnings is a build we cannot fix without a fork.
set(ENABLE_WERROR OFF CACHE BOOL "" FORCE)

add_subdirectory("${MONO_VENDOR}/ngtcp2" EXCLUDE_FROM_ALL)

# Same reason as Crypto++, BLAKE3, Zstd, Luau and QuickJS.
mono_vendor_system(ngtcp2_static)

# Vendor:: to match the rest. `ngtcp2_static` is upstream's own target name and
# keeps working; this is the spelling the rest of the tree should use.
add_library(Vendor::ngtcp2 ALIAS ngtcp2_static)
