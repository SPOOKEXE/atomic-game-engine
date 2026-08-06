@echo off
REM
REM Four worlds, four view producers, one composited frame.
REM
REM `--worlds 4` is what makes it four rather than one, and it is not optional.
REM
REM   scripts\demos\run-mirrors-4-worlds.bat                  uncapped, held at 165 fps
REM   scripts\demos\run-mirrors-4-worlds.bat --graph          extra flags reach the client
REM   set MAX_FPS=60   (then run it)                hold a different rate
REM   set MAX_FPS=0    (then run it)                no limit at all
REM   set PRESET=release  (then run it)             the shipped numbers instead
REM
REM Everything after the script name is appended to the client's own arguments,
REM so `client --help` is the list of what may go there. RUNNING.md has the rest.

call "%~dp0_common.bat" "Mirrors-4-worlds.luau" --worlds 4 --stats %*
exit /b %ERRORLEVEL%
