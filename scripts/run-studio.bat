@echo off
REM
REM The editor, on Windows, from a clone that has run `just setup`.
REM
REM This is `just edit` for people who do not have `just`: it builds the studio
REM and opens it. It drives CMake directly rather than shelling out to `just`,
REM which is not something a Windows machine can be assumed to have, and which
REM would make this script and its POSIX half (`run-studio.sh`) do different
REM things on different platforms.
REM
REM   scripts\run-studio.bat                         dev preset, empty editor
REM   scripts\run-studio.bat --game My.agame         open a game at startup
REM   scripts\run-studio.bat --run play              start playing rather than editing
REM   set PRESET=release  (then run it)              the shipped numbers instead
REM
REM Everything after the script name is appended to the studio's own arguments,
REM so `studio --help` is the list of what may go there. RUNNING.md has the rest.
REM
REM Two of those flags go together: `--headless` refuses to start without
REM `--frames N`, because with no window there is nothing to close and no budget
REM means never stopping. That pairing is what `just studio-smoke` runs, and it
REM is the shape to use from anything that is not a person:
REM
REM   scripts\run-studio.bat --headless --frames 12 --run play --capture out.bmp
REM
REM This is the only program in the repository where `RunService:IsStudio()` is
REM true, and the one that writes a `.agame`. `just host --game X.agame` and
REM `just run --game X.agame` read the same file back.
REM
REM Run it from a Developer Command Prompt for Visual Studio. It builds before
REM it runs, the presets use the Ninja generator, and a plain cmd window finds
REM no compiler for it. `scripts\build-windows.bat` opens that environment
REM itself - build with it first and there is nothing left here to compile, at
REM which point this runs from an ordinary window like everything else.

setlocal EnableExtensions

REM %~dp0 is this script's directory with a trailing backslash, so the parent is
REM the repository root. Resolved through a for loop because `%~dp0..` keeps the
REM `..` in every message the script goes on to print.
for %%I in ("%~dp0..") do set "ROOT=%%~fI"

if "%PRESET%"=="" set "PRESET=dev"
set "BUILD=%ROOT%\.cache\build\%PRESET%"

REM The studio is configured only where both halves exist: it needs the renderer
REM to draw with and the server library to host "Play" with, so under the
REM `server` preset the target is absent rather than stubbed (CMakeLists.txt,
REM the MONO_BUILD_CLIENT AND MONO_BUILD_SERVER guard). `--target studio` there
REM fails inside CMake with `unknown target`, and the reason for it is not in
REM the message. Say what the preset is for instead.
if /i "%PRESET%"=="server" (
    >&2 echo the 'server' preset builds no client - the studio needs one to draw with.
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

REM Configure is quiet and the build is not. Re-running the editor should show
REM what it is compiling, if anything, and nothing else - the configure has no
REM news.
REM
REM The build is by directory rather than by preset, which is the one place
REM these two lines are not symmetrical. `--build --preset` reads
REM CMakePresets.json out of the working directory and has no `-S` to say
REM otherwise, so the preset form works only when the script is run from the
REM repository root and fails with "Could not read presets from <wherever you
REM were>" everywhere else. The directory is derived from the preset name
REM anyway, and for a single-config generator that is the whole of what the
REM build preset contributes.
cmake -S "%ROOT%" --preset %PRESET% >nul
if errorlevel 1 (
    >&2 echo configure failed. Re-run without the ^>nul above to see why.
    exit /b 1
)

cmake --build "%BUILD%" --target studio
if errorlevel 1 exit /b 1

set "STUDIO=%BUILD%\studio\studio.exe"
if not exist "%STUDIO%" (
    >&2 echo built, but there is no studio at %STUDIO%
    >&2 echo   the preset stages its programs somewhere this script does not
    >&2 echo   expect. MONO_STAGE_ROOT in CMakeLists.txt is what decides.
    exit /b 1
)

echo starting the %PRESET% studio: studio %*

"%STUDIO%" %*
exit /b %ERRORLEVEL%
