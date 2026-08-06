@echo off
REM
REM Spells fired at generated terrain, which they dig holes in.
REM
REM The whole stack from data to a part on screen, through libraries ported from
REM a Rojo project without an edit.
REM
REM   scripts\demos\run-magic.bat                  uncapped, held at 165 fps
REM   scripts\demos\run-magic.bat --graph          extra flags reach the client
REM   set MAX_FPS=60   (then run it)                hold a different rate
REM   set MAX_FPS=0    (then run it)                no limit at all
REM   set PRESET=release  (then run it)             the shipped numbers instead
REM
REM Everything after the script name is appended to the client's own arguments,
REM so `client --help` is the list of what may go there. RUNNING.md has the rest.

call "%~dp0_common.bat" "Magic.luau" --stats %*
exit /b %ERRORLEVEL%
