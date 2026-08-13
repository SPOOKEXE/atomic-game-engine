@echo off
REM
REM Six rooms that lie about their own size.
REM
REM A row of exhibits, each one a hole onto a space that does not fit behind it:
REM a tunnel shorter inside than out, one longer inside than out, a house with
REM four doors onto three rooms, a pillar with two backs, a hill climbed by
REM walking down, and a cell holding four times its own volume. The camera
REM sweeps the row; hold the stats overlay to watch the twelve surface passes.
REM
REM   scripts\demos\run-non-euclidean.bat                uncapped, held at 165 fps
REM   scripts\demos\run-non-euclidean.bat --graph        extra flags reach the client
REM   set MAX_FPS=60   (then run it)                hold a different rate
REM   set MAX_FPS=0    (then run it)                no limit at all
REM   set PRESET=release  (then run it)             the shipped numbers instead
REM
REM Everything after the script name is appended to the client's own arguments,
REM so `client --help` is the list of what may go there. RUNNING.md has the rest.

call "%~dp0_common.bat" "NonEuclidean.luau" --stats %*
exit /b %ERRORLEVEL%
