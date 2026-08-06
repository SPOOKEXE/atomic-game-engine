@echo off
REM
REM Imported meshes, built-ins, textures, and a mirror.
REM
REM Needs published content. Without `--cdn` every mesh name resolves to the
REM built-in fallback and the scene draws cubes - which is not a failure, and is
REM not what it is for:
REM
REM   scripts\demos\run-meshes.bat --cdn dir:.\store --publisher-key HEX
REM
REM   scripts\demos\run-meshes.bat                  uncapped, held at 165 fps
REM   scripts\demos\run-meshes.bat --graph          extra flags reach the client
REM   set MAX_FPS=60   (then run it)                hold a different rate
REM   set MAX_FPS=0    (then run it)                no limit at all
REM   set PRESET=release  (then run it)             the shipped numbers instead
REM
REM Everything after the script name is appended to the client's own arguments,
REM so `client --help` is the list of what may go there. RUNNING.md has the rest.

call "%~dp0_common.bat" "Meshes.luau" --stats %*
exit /b %ERRORLEVEL%
