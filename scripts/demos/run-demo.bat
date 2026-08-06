@echo off
REM
REM The built-in demo scene, which is C++ rather than a script.
REM
REM `mono.client\src\Demo.cpp`, selected by nothing and built into the client. It
REM is the one thing here that is not a file the client is pointed at, which is
REM why it passes no scene.
REM
REM   scripts\demos\run-demo.bat                  uncapped, held at 165 fps
REM   scripts\demos\run-demo.bat --graph          extra flags reach the client
REM   set MAX_FPS=60   (then run it)                hold a different rate
REM   set MAX_FPS=0    (then run it)                no limit at all
REM   set PRESET=release  (then run it)             the shipped numbers instead
REM
REM Everything after the script name is appended to the client's own arguments,
REM so `client --help` is the list of what may go there. RUNNING.md has the rest.

call "%~dp0_common.bat" ""  %*
exit /b %ERRORLEVEL%
