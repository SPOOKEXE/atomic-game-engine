@echo off
REM
REM A build on Windows, from any shell.
REM
REM This is `just build` for people who do not have `just`, and for the shell
REM that is not a Developer Command Prompt. Every other .bat in this repository
REM ends its header telling you to open one first; this is the script that does
REM not need you to, because it opens the compiler environment itself.
REM
REM   scripts\build-windows.bat                      everything, dev preset
REM   scripts\build-windows.bat client               one target and its deps
REM   scripts\build-windows.bat engine_ecs engine_world      several
REM   set PRESET=release  (then run it)              a different preset
REM
REM Arguments are target names, passed straight to `cmake --build --target`.
REM RUNNING.md lists what those are, and which presets configure them - under
REM `server` there is no engine_render to ask for, and CMake says `unknown
REM target` rather than explaining why.
REM
REM --- Why this script exists ------------------------------------------------
REM
REM The presets use the Ninja generator (CMakePresets.json), and Ninja has no
REM per-command environment: build.ninja is a flat list of literal command
REM lines, so CMake cannot record the toolchain's include and library paths in
REM it. MSVC reads those from INCLUDE and LIB in the environment instead. In a
REM plain cmd or PowerShell window they are unset, and every compile fails at
REM its first #include of anything at all:
REM
REM   fatal error C1083: Cannot open include file: 'cstddef': No such file
REM
REM which reads like the standard library is missing and is really the shell
REM being wrong. Note that configuring still succeeds, so the failure lands at
REM the first compile rather than anywhere near its cause. The Visual Studio
REM generator does not have this problem - MSBuild rebuilds the environment
REM from the .vcxproj - but it is not the generator the presets pin.
REM
REM `vcvars64.bat` is what sets INCLUDE, and a Developer Command Prompt is
REM nothing more than a cmd window that has already run it. So does this
REM script, inside its own `setlocal`, which means the environment it builds
REM lasts exactly as long as the build and never leaks into your shell.
REM
REM x64 host, x64 target. Nothing here cross-compiles, and neither does the
REM engine.
REM
REM --- Two parsing rules this file is shaped around --------------------------
REM
REM cmd expands every percent on a line before it evaluates any condition on
REM that line. A `:~` substring of a variable that does not exist therefore
REM cannot be guarded by an `if defined` in front of it: the expansion has
REM already failed, the percents are stripped, and what is left is not a valid
REM command. "The syntax of the command is incorrect." Skipping such a line
REM with goto is the guard that works, because a skipped line is never parsed.
REM
REM The same up-front expansion covers a whole `( ... )` block at once, so an
REM unquoted path holding `(x86)` ends the block at its own `)`. Nothing here
REM uses those blocks now, and every path in an echo is quoted regardless.

setlocal EnableExtensions

REM %~dp0 is this script's directory with a trailing backslash, so the parent is
REM the repository root. Resolved through a for loop because `%~dp0..` keeps the
REM `..` in every message the script goes on to print.
for %%I in ("%~dp0..") do set "ROOT=%%~fI"

if "%PRESET%"=="" set "PRESET=dev"
set "BUILD=%ROOT%\.cache\build\%PRESET%"

REM --- Finding Visual Studio -------------------------------------------------
REM
REM Quote both Program Files variables into plain names first. Their values
REM contain `(x86)`, and unquoted parentheses inside an if or for block end the
REM block early - a parse error that reports itself as something unrelated
REM several lines further down.
set "PF64=%ProgramFiles%"
set "PF86=%ProgramFiles(x86)%"
if not defined PF86 set "PF86=%PF64%"

REM VSINSTALLDIR is already set if the caller is a Developer Command Prompt.
REM Believe it rather than searching for a second opinion that may differ from
REM the environment the rest of this shell is already carrying.
set "VSPATH="
if not defined VSINSTALLDIR goto :no_devprompt
set "VSPATH=%VSINSTALLDIR%"
REM Reached only when VSPATH holds something. That matters: `set "VSPATH="`
REM deletes the variable rather than emptying it, and a :~ substring on a
REM variable that does not exist does not expand to "" - cmd cannot pair the
REM percent signs, strips them, and leaves `if "~-1VSPATH:~0,-1"` behind. An
REM `if defined VSPATH` guard on the same line does not help, because every
REM percent on a line is expanded before any condition on it is evaluated.
REM Skipping the line with goto is what keeps it from being parsed at all.
if "%VSPATH:~-1%"=="\" set "VSPATH=%VSPATH:~0,-1%"
:no_devprompt

REM vswhere ships with the installer, not with any one install, and is the only
REM supported way to locate them. `-products *` is load-bearing: without it
REM vswhere matches Community, Professional and Enterprise only, and returns
REM nothing at all on a machine that has Build Tools - which is the usual
REM machine for a build that is not also an IDE. `-requires` skips an install
REM that has the IDE but never had the C++ compiler component added.
REM
REM **Exactly one pair of quotes around the vswhere path, and none around the
REM whole command.** An outer pair looks harmless and is not: the parser pairs
REM quotes left to right, so a leading `""` is an empty string that closes
REM immediately and leaves the path itself unquoted. Its `(x86)` then ends the
REM `in ( ... )` clause at its own `)`, and cmd reports `\Microsoft was
REM unexpected at this time.` - a parse error naming the middle of a path, with
REM nothing to say about quoting. Parens are only protected while quoted.
set "VSWHERE=%PF86%\Microsoft Visual Studio\Installer\vswhere.exe"
if not defined VSPATH if exist "%VSWHERE%" for /f "usebackq tokens=* delims=" %%I in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath 2^>nul`) do set "VSPATH=%%I"

REM And if vswhere is missing or came back empty, the default locations. Build
REM Tools 2022 still installs under Program Files (x86) on most machines while
REM the IDE editions moved to Program Files, so both roots are worth a look.
REM
REM Written as chained `do` rather than nested `( ... )` blocks: the loop bodies
REM expand paths that contain `(x86)`, and chaining keeps the parser from having
REM any block depth to lose count of.
if defined VSPATH goto :vspath_done
for %%R in ("%PF64%" "%PF86%") do for %%Y in (2022 2019) do for %%E in (BuildTools Community Professional Enterprise) do if not defined VSPATH if exist "%%~R\Microsoft Visual Studio\%%Y\%%E\VC\Auxiliary\Build\vcvars64.bat" set "VSPATH=%%~R\Microsoft Visual Studio\%%Y\%%E"
:vspath_done

if defined VSPATH goto :vs_found
>&2 echo no Visual Studio C++ toolchain found.
>&2 echo   Install "Desktop development with C++" - the Build Tools alone are
>&2 echo   enough, no IDE required. CONTRIBUTING.md lists the prerequisites.
exit /b 1
:vs_found

set "VCVARS=%VSPATH%\VC\Auxiliary\Build\vcvars64.bat"
if exist "%VCVARS%" goto :vcvars_found
>&2 echo found Visual Studio at "%VSPATH%", but no x64 compiler in it:
>&2 echo   "%VCVARS%"
>&2 echo   The install is missing the "MSVC v143 - VS 2022 C++ x64/x86 build
>&2 echo   tools" component. Add it in the Visual Studio Installer.
exit /b 1
:vcvars_found

REM VSCMD_VER is set by vcvars and by every Developer Command Prompt. Running
REM vcvars a second time in a shell that has it appends another copy of the
REM toolchain to PATH, INCLUDE and LIB rather than replacing them, and enough
REM repeats overflow the 8191-character environment limit - at which point the
REM variables truncate and the compiler stops finding headers again, for a
REM reason that looks identical to the one this script exists to fix.
if not defined VSCMD_VER call "%VCVARS%" >nul

REM vcvars exits 0 in some partial-install cases having printed its complaint
REM and set nothing. Check the variable that matters rather than the exit code.
if defined INCLUDE goto :include_ok
>&2 echo vcvars64.bat ran but set no INCLUDE, so the compiler would report
>&2 echo   "Cannot open include file: 'cstddef'" on the first file it read.
>&2 echo   Run it on its own to see what it is complaining about:
>&2 echo   "%VCVARS%"
exit /b 1
:include_ok

REM --- CMake and Ninja -------------------------------------------------------
REM
REM vcvars does not put the C++ CMake tools on PATH, so a machine whose only
REM CMake is the one bundled with Visual Studio has both installed and neither
REM reachable. Fall back to them before giving up, and only for whichever of
REM the two is actually missing.
set "VSCMAKE=%VSPATH%\Common7\IDE\CommonExtensions\Microsoft\CMake"
where cmake >nul 2>nul
if errorlevel 1 if exist "%VSCMAKE%\CMake\bin\cmake.exe" set "PATH=%VSCMAKE%\CMake\bin;%PATH%"
where ninja >nul 2>nul
if errorlevel 1 if exist "%VSCMAKE%\Ninja\ninja.exe" set "PATH=%VSCMAKE%\Ninja;%PATH%"

where cmake >nul 2>nul
if not errorlevel 1 goto :cmake_ok
>&2 echo cmake is not on PATH, and Visual Studio has no bundled copy at
>&2 echo   "%VSCMAKE%\CMake\bin"
>&2 echo   Install CMake 3.24 or newer, or add the "C++ CMake tools for
>&2 echo   Windows" component in the Visual Studio Installer.
exit /b 1
:cmake_ok

where ninja >nul 2>nul
if not errorlevel 1 goto :ninja_ok
>&2 echo ninja is not on PATH, and every preset generates for it.
>&2 echo   The "C++ CMake tools for Windows" component ships one.
exit /b 1
:ninja_ok

REM --- Configure and build ---------------------------------------------------
REM
REM Configure is quiet, as it is in the Justfile's `build` recipe: re-running a
REM build should show what it is compiling and nothing else. It is safe to
REM re-run every time - CMake reuses the cache and does nothing when no
REM CMakeLists.txt has changed.
REM
REM The build is by directory rather than by preset, which is the one place
REM these two lines are not symmetrical, and the reason is in run-studio.bat:
REM `--build --preset` reads CMakePresets.json out of the working directory and
REM has no `-S` to say otherwise, so it works only when the script is run from
REM the repository root. The directory is derived from the preset name anyway.
cmake -S "%ROOT%" --preset "%PRESET%" >nul
if not errorlevel 1 goto :configured
>&2 echo configure failed. Re-run it to see why:
>&2 echo   cmake -S "%ROOT%" --preset "%PRESET%"
exit /b 1
:configured

set "TARGETS="
if not "%~1"=="" set "TARGETS=--target %*"

cmake --build "%BUILD%" %TARGETS%
exit /b %ERRORLEVEL%
