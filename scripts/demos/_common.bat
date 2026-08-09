@echo off
REM
REM What every `run-*.bat` in this directory is made of.
REM
REM Called with the scene's filename and its own flags, then everything the
REM caller passed:
REM
REM   call "%~dp0_common.bat" Terrain.luau --stats %*
REM
REM An empty first argument means the built-in demo scene, which is C++ rather
REM than a script. Eleven copies of a build sequence is eleven places to fix a
REM preset name, and the copies would stop agreeing the first time one of them
REM was edited in a hurry - which is the same reason `_common.sh` exists.
REM
REM Run these from a Developer Command Prompt for Visual Studio. They build
REM before they run, the presets use the Ninja generator, and a plain cmd window
REM finds no compiler for it. `scripts\build-windows.bat` opens that environment
REM itself - build with it first and there is nothing left here to compile, at
REM which point these run from an ordinary window like everything else.
REM
REM --- The frame-rate policy -------------------------------------------------
REM
REM `--uncapped --max-fps 165`, and the pair is one decision rather than two.
REM `--uncapped` alone turns off the vblank wait and lets the loop run as fast
REM as the GPU allows, which on a cheap scene is several hundred frames a second
REM of heat for a display that shows a fraction of them. The vblank wait alone
REM paces to whatever the display reports, which is not comparable between two
REM machines and is not what a variable-refresh monitor does.
REM
REM So: do not wait for the display, and do not run away from it either. Set
REM MAX_FPS to hold a different rate, or to 0 for no limit.

setlocal EnableExtensions

set "SCENE=%~1"
shift

REM %~dp0 is this script's directory with a trailing backslash, so two levels up
REM is the repository root. Resolved through a for loop because `%~dp0..\..`
REM keeps the `..` in every message the script goes on to print.
for %%I in ("%~dp0..\..") do set "ROOT=%%~fI"

if "%PRESET%"=="" set "PRESET=dev"
if "%MAX_FPS%"=="" set "MAX_FPS=165"
set "BUILD=%ROOT%\.cache\build\%PRESET%"

REM The server preset configures no client target at all, so `--target client`
REM there fails inside CMake with `unknown target`, and the reason for it is not
REM in the message. Say what the preset is for instead.
if /i "%PRESET%"=="server" (
    >&2 echo the 'server' preset builds no client - there is nothing to run.
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

REM Configure is quiet and the build is not. Re-running a demo should show what
REM it is compiling, if anything, and nothing else - the configure has no news.
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

set "PACING=--uncapped"
if not "%MAX_FPS%"=="0" set "PACING=--uncapped --max-fps %MAX_FPS%"

REM The staged copy, not the source. `mono.engine\examples\` is where a scene is
REM written and `assets\examples\` under the build is where it is staged beside
REM the binary that runs it - a demo that ran the source tree would work here
REM and nowhere a staged tree was copied to.
set "SCRIPTARG="
if not "%SCENE%"=="" (
    set "STAGED=%BUILD%\client\assets\examples\%SCENE%"
    if not exist "%BUILD%\client\assets\examples\%SCENE%" set "STAGED=%BUILD%\assets\examples\%SCENE%"
)
if not "%SCENE%"=="" (
    if not exist "%STAGED%" (
        >&2 echo no staged scene at %STAGED%
        >&2 echo   the build did not stage %SCENE%. Check mono.engine\examples\CMakeLists.txt.
        exit /b 1
    )
    set "SCRIPTARG=--script "%STAGED%""
)

if "%SCENE%"=="" (
    echo running the built-in demo at %MAX_FPS% fps
) else (
    echo running %SCENE% at %MAX_FPS% fps
)

"%CLIENT%" %SCRIPTARG% %PACING% %*
exit /b %ERRORLEVEL%
