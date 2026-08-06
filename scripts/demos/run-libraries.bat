@echo off
REM
REM The shipped Luau libraries, loaded and exercised.
REM
REM `MagicCore` and `TerrainCore` unchanged from the Rojo project they came from.
REM
REM   scripts\demos\run-libraries.bat                  uncapped, held at 165 fps
REM   scripts\demos\run-libraries.bat --graph          extra flags reach the client
REM   set MAX_FPS=60   (then run it)                hold a different rate
REM   set MAX_FPS=0    (then run it)                no limit at all
REM   set PRESET=release  (then run it)             the shipped numbers instead
REM
REM Everything after the script name is appended to the client's own arguments,
REM so `client --help` is the list of what may go there. RUNNING.md has the rest.

call "%~dp0_common.bat" "Libraries.luau"  %*
exit /b %ERRORLEVEL%
