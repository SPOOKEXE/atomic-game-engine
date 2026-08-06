@echo off
REM
REM A lattice of blocks in an empty sky, and nothing underneath.
REM
REM What a viewport has to survive: many small separate objects with sky visible
REM between them.
REM
REM   scripts\demos\run-skygrid.bat                  uncapped, held at 165 fps
REM   scripts\demos\run-skygrid.bat --graph          extra flags reach the client
REM   set MAX_FPS=60   (then run it)                hold a different rate
REM   set MAX_FPS=0    (then run it)                no limit at all
REM   set PRESET=release  (then run it)             the shipped numbers instead
REM
REM Everything after the script name is appended to the client's own arguments,
REM so `client --help` is the list of what may go there. RUNNING.md has the rest.

call "%~dp0_common.bat" "SkyGrid.luau" --stats %*
exit /b %ERRORLEVEL%
