@echo off
REM
REM A 16384 x 16384 noise-generated voxel world streamed around a camera.
REM
REM Chunks are generated on demand and evicted behind the camera. The view
REM channel grows itself to fit.
REM
REM   scripts\demos\run-terrain.bat                  uncapped, held at 165 fps
REM   scripts\demos\run-terrain.bat --graph          extra flags reach the client
REM   set MAX_FPS=60   (then run it)                hold a different rate
REM   set MAX_FPS=0    (then run it)                no limit at all
REM   set PRESET=release  (then run it)             the shipped numbers instead
REM
REM Everything after the script name is appended to the client's own arguments,
REM so `client --help` is the list of what may go there. RUNNING.md has the rest.

call "%~dp0_common.bat" "Terrain.luau" --stats %*
exit /b %ERRORLEVEL%
