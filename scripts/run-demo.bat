@echo off
REM
REM The demo, on Windows, from a clone that has run `just setup`.
REM
REM This is `just demo` for people who do not have `just`: it builds the client
REM and runs the v0.1 demo scene with both debug panels open. It drives CMake
REM directly rather than shelling out to `just`, which is not something a
REM Windows machine can be assumed to have, and which would make this script and
REM its POSIX half (`run-demo.sh`) do different things on different platforms.
REM
REM   scripts\run-demo.bat                     dev preset, both panels
REM   scripts\run-demo.bat --uncapped          extra flags reach the client
REM   scripts\run-demo.bat --frames 600 --entities 4000
REM   set PRESET=release  (then run it)        the shipped numbers instead
REM
REM Everything after the script name is appended to the client's own arguments,
REM so `client --help` is the list of what may go there. RUNNING.md has the rest.
REM
REM Run it from a Developer Command Prompt for Visual Studio. The presets use
REM the Ninja generator, which finds no compiler in a plain cmd window.
REM
REM --- WHEN THE SCRIPTING RUNTIME LANDS (v0.5), THIS CHANGES -----------------
REM
REM The scene this runs is C++: `mono.client\src\Demo.cpp`, built into the
REM client and selected by nothing. That is a stand-in. A game is meant to be a
REM script, and once the Luau/TypeScript runtime exists the demo becomes a file
REM the client is pointed at:
REM
REM   "%CLIENT%" "%ROOT%\mono.engine\examples\Mirrors-1-world.luau" --stats --graph %*
REM
REM (written on one line on purpose: a trailing caret is a line continuation to
REM cmd's parser, and it is applied before REM discards the line, so a wrapped
REM comment quietly swallows the line after it.)
REM
REM Swap the run line at the bottom for that when it works, and change nothing
REM else - the build half of this script stays as it is. Two things have to land
REM first: the VM and the bindings (ROADMAP.md v0.4-v0.5), and content in the
REM example files, which are empty placeholders today. Until both are true the
REM path above loads nothing - a bare positional path is collected by the parser
REM and then ignored, with no message (RUNNING.md, "What happens today").
REM `Demo.hpp` and `Demo.cpp` are deleted at that point; `mono.client\AGENTS.md`
REM says so.

setlocal EnableExtensions

REM %~dp0 is this script's directory with a trailing backslash, so the parent is
REM the repository root. Resolved through a for loop because `%~dp0..` keeps the
REM `..` in every message the script goes on to print.
for %%I in ("%~dp0..") do set "ROOT=%%~fI"

if "%PRESET%"=="" set "PRESET=dev"
set "BUILD=%ROOT%\.cache\build\%PRESET%"

REM The server preset configures no client target at all, so `--target client`
REM there fails inside CMake with `unknown target`, and the reason for it is not
REM in the message. It is not a mistake worth reading a build log over: say what
REM the preset is for instead.
if /i "%PRESET%"=="server" (
    >&2 echo the 'server' preset builds no client - there is nothing to demo.
    >&2 echo   try:  set PRESET=dev
    exit /b 1
)

where cmake >nul 2>nul
if errorlevel 1 (
    >&2 echo cmake is not on PATH. CONTRIBUTING.md lists the prerequisites.
    >&2 echo   A Developer Command Prompt for Visual Studio has it, along with
    >&2 echo   the compiler the Ninja generator needs.
    exit /b 1
)

REM Configure is quiet and the build is not. Re-running the demo should show
REM what it is compiling, if anything, and nothing else - the configure has no
REM news.
REM
REM The build is by directory rather than by preset, which is the one place
REM these two lines are not symmetrical. `--build --preset` reads
REM CMakePresets.json out of the working directory and has no `-S` to say
REM otherwise, so the preset form works only when the demo is run from the
REM repository root and fails with "Could not read presets from <wherever you
REM were>" everywhere else. The directory is derived from the preset name
REM anyway, and for a single-config generator that is the whole of what the
REM build preset contributes.
cmake -S "%ROOT%" --preset %PRESET% >nul
if errorlevel 1 (
    >&2 echo configure failed. Re-run without the ^>nul above to see why.
    exit /b 1
)

cmake --build "%BUILD%" --target client
if errorlevel 1 exit /b 1

set "CLIENT=%BUILD%\client\client.exe"
if not exist "%CLIENT%" (
    >&2 echo built, but there is no client at %CLIENT%
    >&2 echo   the preset stages its programs somewhere this script does not
    >&2 echo   expect. MONO_STAGE_ROOT in CMakeLists.txt is what decides.
    exit /b 1
)

echo running the %PRESET% demo: client --stats --graph %*

"%CLIENT%" --stats --graph %*
exit /b %ERRORLEVEL%
