@echo off
REM
REM The mesh grid, end to end: bake, publish, fetch, draw.
REM
REM `mono.engine\examples\MeshGrid.luau` needs content that has been baked and
REM signed, so running it with `--script` alone shows nine fallback cubes. This
REM does the three steps in front of it, with the flags that matter already set.
REM
REM   scripts\demos\run-mesh-grid.bat ART                bake ART\ and run
REM   scripts\demos\run-mesh-grid.bat ART --frames 600   extra flags reach the client
REM   set MAX_FPS=60  (then run it)                      hold a different rate
REM
REM `ART` is a directory of source art laid out however you like; the scene names
REM the nine meshes it expects, so the paths under it have to match. The header
REM of `MeshGrid.luau` lists them. Its POSIX half (`run-mesh-grid.sh`) carries
REM the note about Blender files not being source art.
REM
REM Run this from a Developer Command Prompt for Visual Studio, or build first
REM with `scripts\build-windows.bat`, which opens that environment itself and
REM leaves nothing here to compile. `_common.bat` has the reason.

setlocal EnableExtensions EnableDelayedExpansion

for %%I in ("%~dp0..\..") do set "ROOT=%%~fI"
if "%PRESET%"=="" set "PRESET=dev"
if "%MAX_FPS%"=="" set "MAX_FPS=165"
set "BUILD=%ROOT%\.cache\build\%PRESET%"

set "ART=%~1"
if "%ART%"=="" set "ART=%ROOT%\ART"
if not "%~1"=="" shift

if not exist "%ART%\" (
    >&2 echo run-mesh-grid: '%ART%' is not a directory
    >&2 echo usage: scripts\demos\run-mesh-grid.bat ART [client flags...]
    exit /b 1
)

if "%MESH_GRID_WORK%"=="" set "MESH_GRID_WORK=%ROOT%\.cache\mesh-grid"
set "CONTENT=%MESH_GRID_WORK%\content"
set "STORE=%MESH_GRID_WORK%\store"

cmake --build "%BUILD%" --target assetc cdn client
if errorlevel 1 exit /b 1

REM A throwaway key, because this signs nothing anybody else will verify. A
REM publisher's real seed belongs somewhere a repository is not; RUNNING.md says
REM where. The public half is derived from it, so the client below can be pinned
REM to the same identity without a second file.
if "%MESH_GRID_KEY%"=="" set "MESH_GRID_KEY=7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a"

if exist "%CONTENT%" rmdir /s /q "%CONTENT%"
if exist "%STORE%" rmdir /s /q "%STORE%"

REM `--model-size 1`, and it is the flag this whole script exists to get right.
REM A `MeshPart`'s `Size` multiplies the mesh's own coordinates rather than
REM fitting it into a box, so baking at the default of four and asking for a
REM four-metre part draws sixteen metres and the grid overlaps itself.
"%BUILD%\tools\assetc.exe" --input "%ART%" --output "%CONTENT%" --no-copy --model-size 1
if errorlevel 1 exit /b 1

set "PUBLISHER="
for /f "tokens=2 delims= " %%K in ('"%BUILD%\cdn\cdn.exe" --publish "%CONTENT%" --store "%STORE%" --signing-key "%MESH_GRID_KEY%" 2^>^&1 ^| findstr /c:"publisher key"') do (
    set "PUBLISHER=%%K"
)
REM The line reads `cdn: publisher key <hex>`, so the hex is the last token
REM rather than the second. Take it off the end of what the loop collected.
for %%K in (%PUBLISHER%) do set "PUBLISHER=%%K"

if "%PUBLISHER%"=="" (
    >&2 echo run-mesh-grid: the publish step printed no publisher key
    exit /b 1
)

set "PACING=--uncapped"
if not "%MAX_FPS%"=="0" set "PACING=--uncapped --max-fps %MAX_FPS%"

"%BUILD%\client\client.exe" --cdn "dir:%STORE%" --publisher-key "%PUBLISHER%" --content-cache "%MESH_GRID_WORK%\cache" --script "%ROOT%\mono.engine\examples\MeshGrid.luau" --entities 2048 %PACING% %*
exit /b %ERRORLEVEL%
