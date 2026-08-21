# Cross-compiling the whole engine to Windows x86_64 from Linux, with mingw-w64.
#
#   cmake --preset windows-cross -DMONO_GLSLC=<path to a native glslc>
#   cmake --build --preset windows-cross
#   scripts/package-release.sh .cache/build/windows-cross 0.18.0 windows-x86_64 dist
#
# This is not the supported way to build for Windows. `scripts/build-windows.bat`
# on a Windows machine with MSVC is, and the release workflow's `windows-2022`
# runner is what publishes. This exists because it is the only way to find out
# on a Linux machine whether a change compiles and links for Windows at all, and
# because two portability bugs were found by trying it that no Linux build could
# have reported:
#
#   - `constinit std::mutex` in `mono.engine/core/src/HeapProfile.cpp`, which
#     libstdc++ on glibc accepts only because `pthread_mutex_t` happens to have
#     a constant initialiser.
#   - asio's `AcceptEx`, requested through `#pragma comment(lib, "mswsock")`,
#     which is an MSVC extension GCC parses and ignores.
#
# Both are real on MSVC's side of the fence too - the first is a latent
# violation of that file's own "nothing here may have a constructor" invariant,
# and the second is a library the link only found by luck of the compiler.
#
# --- What it needs --------------------------------------------------------
#
# Debian and Ubuntu:  apt install mingw-w64 wine
#
# **A native `glslc`, passed in as `MONO_GLSLC`.** The vendored shaderc is built
# for the target, so a cross build produces a `glslc.exe` that cannot run on the
# machine doing the building. `MONO_VENDORED_GLSLC=OFF` in the preset turns that
# off; the path is not set here because there is no location to guess. A native
# `release` build leaves one at:
#
#   .cache/build/release/mono.vendor/shaderc/glslc/glslc
#
# --- Why wine is in here --------------------------------------------------
#
# `shadercross` is the second build-time tool, and unlike glslc it is ours: the
# shader rules run it on every `.spv` to write the `.msl` beside it. It has no
# equivalent of `MONO_VENDORED_GLSLC` because it has never had a reason to be
# anything but the one just built.
#
# `CMAKE_CROSSCOMPILING_EMULATOR` is the answer CMake provides, and it fits
# because of how those rules are spelled: CMake prepends the emulator to an
# `add_custom_command` COMMAND that names an executable *target*, which
# `COMMAND shadercross` is. Note that it would not fit glslc even if that were
# built here - `MONO_GLSLC` holds `$<TARGET_FILE:glslc_exe>`, a generator
# expression rather than a target name, and CMake leaves those alone.

set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

set(prefix x86_64-w64-mingw32)

# **The `-posix` variants, deliberately.** mingw-w64 ships two thread models.
# The win32 one grew `std::thread`, `std::mutex` and `std::condition_variable`
# support only in GCC 13 and is still the newer path; the posix one is
# winpthreads, which has carried them for a decade. The engine uses all three.
set(CMAKE_C_COMPILER   ${prefix}-gcc-posix)
set(CMAKE_CXX_COMPILER ${prefix}-g++-posix)
set(CMAKE_RC_COMPILER  ${prefix}-windres)
set(CMAKE_AR           ${prefix}-ar)
set(CMAKE_RANLIB       ${prefix}-ranlib)

# Libraries and headers come from the target sysroot; programs come from the
# host, because the programs CMake looks for are the ones it intends to run.
set(CMAKE_FIND_ROOT_PATH /usr/${prefix})
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# **The GCC runtime linked in rather than shipped beside the binaries.** Without
# this every program imports `libstdc++-6.dll`, `libgcc_s_seh-1.dll` and
# `libwinpthread-1.dll` from the mingw installation that built it, and starts on
# no machine that does not have one. Stated as `_INIT` so a caller can still
# override it.
#
# SDL keeps its own line, because it is a DLL we ship and `-static` on a shared
# library is not a thing to ask for. Its two `-static-lib` flags do the same job
# for the same reason.
set(CMAKE_EXE_LINKER_FLAGS_INIT    "-static")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "-static-libgcc -static-libstdc++")

# See the header. Only `shadercross` goes through this.
find_program(MONO_WINE wine REQUIRED
	DOC "Runs the Windows build-time tools during a cross build")
set(CMAKE_CROSSCOMPILING_EMULATOR "${MONO_WINE}")
