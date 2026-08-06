@echo off
REM
REM The ported libraries' own test suite, run by this engine's Luau.
REM
REM 183 data tests that ran in Roblox Studio, run here. It prints results rather
REM than drawing a scene, so there is nothing to look at.
REM
REM   scripts\demos\run-magic-tests.bat                  uncapped, held at 165 fps
REM   scripts\demos\run-magic-tests.bat --graph          extra flags reach the client
REM   set MAX_FPS=60   (then run it)                hold a different rate
REM   set MAX_FPS=0    (then run it)                no limit at all
REM   set PRESET=release  (then run it)             the shipped numbers instead
REM
REM Everything after the script name is appended to the client's own arguments,
REM so `client --help` is the list of what may go there. RUNNING.md has the rest.

call "%~dp0_common.bat" "MagicTests.luau"  %*
exit /b %ERRORLEVEL%
