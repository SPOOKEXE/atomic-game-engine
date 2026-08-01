# MonoVendor.cmake — third-party configuration, in one place.
#
# Vendor options are set here rather than in the root CMakeLists so that the
# root file stays what §3 says it is: options, tiers and add_subdirectory.

include_guard(GLOBAL)

set(MONO_VENDOR "${CMAKE_SOURCE_DIR}/mono.vendor")

if(NOT EXISTS "${MONO_VENDOR}/flecs/CMakeLists.txt")
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

# --- flecs ------------------------------------------------------------------
set(FLECS_STATIC ON  CACHE BOOL "" FORCE)
set(FLECS_SHARED OFF CACHE BOOL "" FORCE)
set(FLECS_TESTS  OFF CACHE BOOL "" FORCE)
add_subdirectory("${MONO_VENDOR}/flecs" EXCLUDE_FROM_ALL)

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
# `shared` in engine terms — `net` at L11 is where it lands, and both the client
# and the server link that. Nothing links it yet.
add_library(vendor_asio INTERFACE)
add_library(Vendor::asio ALIAS vendor_asio)
target_include_directories(vendor_asio SYSTEM INTERFACE "${MONO_VENDOR}/asio/asio/include")
# Without this asio looks for Boost. It is the difference between the two
# distributions and the only reason this is not a bare include path.
target_compile_definitions(vendor_asio INTERFACE ASIO_STANDALONE ASIO_NO_DEPRECATED)
find_package(Threads REQUIRED)
target_link_libraries(vendor_asio INTERFACE Threads::Threads)

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
# reach <sdl/...> and <flecs/...> by accident. Write <cryptopp/sha.h> and treat
# the bare <sha.h> spelling as unavailable.
get_target_property(_cryptopp_includes cryptopp INTERFACE_INCLUDE_DIRECTORIES)
set_target_properties(cryptopp PROPERTIES
	INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${_cryptopp_includes}")
unset(_cryptopp_includes)

# Vendor:: to match asio and imgui. cryptopp::cryptopp is upstream's own alias
# and keeps working; this is the spelling the rest of the tree should use.
add_library(Vendor::cryptopp ALIAS cryptopp)
