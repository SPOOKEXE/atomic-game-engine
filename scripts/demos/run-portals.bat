@echo off
REM
REM Holes in walls that lead somewhere the wall does not.
REM
REM Three rooms three hundred units apart and six portals between them: one pair
REM that describes an ordinary adjacency, and two that put the same room through
REM opposite walls of the one you are standing in.
REM
REM   scripts\demos\run-portals.bat                  uncapped, held at 165 fps
REM   scripts\demos\run-portals.bat --graph          extra flags reach the client
REM   set MAX_FPS=60   (then run it)                hold a different rate
REM   set MAX_FPS=0    (then run it)                no limit at all
REM   set PRESET=release  (then run it)             the shipped numbers instead
REM
REM Everything after the script name is appended to the client's own arguments,
REM so `client --help` is the list of what may go there. RUNNING.md has the rest.

call "%~dp0_common.bat" "Portals-1-world.luau" --stats %*
exit /b %ERRORLEVEL%
