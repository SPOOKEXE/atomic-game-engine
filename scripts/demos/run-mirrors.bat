@echo off
REM
REM A room made of mirrors, and what it takes to make one.
REM
REM The rendering path: shadows, cameras that draw into a texture, and surfaces
REM that sample the result a frame later.
REM
REM   scripts\demos\run-mirrors.bat                  uncapped, held at 165 fps
REM   scripts\demos\run-mirrors.bat --graph          extra flags reach the client
REM   set MAX_FPS=60   (then run it)                hold a different rate
REM   set MAX_FPS=0    (then run it)                no limit at all
REM   set PRESET=release  (then run it)             the shipped numbers instead
REM
REM Everything after the script name is appended to the client's own arguments,
REM so `client --help` is the list of what may go there. RUNNING.md has the rest.

call "%~dp0_common.bat" "Mirrors-1-world.luau" --stats %*
exit /b %ERRORLEVEL%
